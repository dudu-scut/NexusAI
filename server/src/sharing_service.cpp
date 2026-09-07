#include "agent_rpc/server/sharing_service.h"
#include "agent_rpc/server/auth_interceptor.h"
#include "agent_rpc/common/logger.h"
#include "agent_rpc/common/postgres_store.h"
#include "agent_rpc/common/query_domain_repository.h"

#include <openssl/sha.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>

namespace agent_rpc { namespace server {

namespace {

// Cryptographically sensitive random bytes for bearer tokens and
// identifiers. Every byte is drawn DIRECTLY from the operating-system
// entropy source (std::random_device — getrandom()/BCryptGenRandom
// backed); no PRNG state is maintained, so observing outputs can never
// help predict future ones. Identifier collisions (ids, not tokens) are
// absorbed by the primary-key constraints.
std::string randomHex(std::size_t bytes) {
    std::random_device entropy;
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    std::size_t emitted = 0;
    while (emitted < bytes) {
        // Combine two 32-bit device outputs into one 64-bit word.
        const std::uint64_t low = entropy();
        const std::uint64_t high = entropy();
        const std::uint64_t word = (high << 32) | (low & 0xFFFFFFFFull);
        const std::size_t take = std::min<std::size_t>(8, bytes - emitted);
        for (std::size_t i = 0; i < take; ++i) {
            out << std::setw(2)
                << static_cast<int>((word >> (8 * i)) & 0xFF);
        }
        emitted += take;
    }
    return out.str();
}

// PostgreSQL stores ONLY this digest; the raw token can never be recovered
// from the database.
std::string sha256Hex(const std::string& input) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
    std::ostringstream hex;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        hex << std::setw(2) << std::setfill('0') << std::hex
            << static_cast<int>(digest[i]);
    }
    return hex.str();
}

// ISO-8601 UTC timestamp for share expiry (expires_days > 0).
std::string isoUtcAfterDays(int days) {
    const auto moment = std::chrono::system_clock::now() + std::chrono::hours(24 * days);
    const std::time_t time = std::chrono::system_clock::to_time_t(moment);
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream formatted;
    formatted << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return formatted.str();
}

grpc::Status requireOwnerSession(std::string& owner_id) {
    if (!AuthInterceptor::isAuthenticated()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                            "Authentication required");
    }
    owner_id = AuthInterceptor::currentUserId();
    if (owner_id.empty()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                            "Authentication required");
    }
    return grpc::Status::OK;
}

} // anonymous namespace

grpc::Status SharingServiceImpl::ObserveSession(
    grpc::ServerContext* ctx, const agent_communication::ObserveSessionRequest* request,
    grpc::ServerWriter<agent_communication::AIStreamEvent>* writer) {
    (void)ctx;
    std::string owner_id;
    const auto auth = requireOwnerSession(owner_id);
    if (!auth.ok()) return auth;
    if (!domain_repository_) {
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "Sharing persistence is not configured");
    }
    if (request->trace_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "trace_id is required");
    }
    // Owner-scoped snapshot of the durable trace state from PostgreSQL.
    // NOTE: this is a one-shot snapshot stream (single event), not a live
    // feed; a real-time feed would require durable change notifications.
    try {
        const auto trace = domain_repository_->getTraceById(owner_id, request->trace_id());
        if (!trace.has_value()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND,
                                "Trace not found: " + request->trace_id());
        }
        // The real conversation id comes from the associated query log —
        // the trace row itself only carries the query_log reference.
        std::string context_id;
        const auto query_log =
            domain_repository_->getQueryLogById(owner_id, trace->query_log_id);
        if (query_log.has_value()) {
            context_id = query_log->conversation_id;
        }
        agent_communication::AIStreamEvent event;
        event.set_event_type("snapshot");
        event.set_content(trace->status);
        event.set_task_state(trace->status);
        event.set_context_id(context_id);
        writer->Write(event);
        return grpc::Status::OK;
    } catch (const std::exception& error) {
        LOG_ERROR(std::string("ObserveSession failed: ") + error.what());
        return grpc::Status(
            common::isPostgresError(error) ? grpc::StatusCode::UNAVAILABLE
                                           : grpc::StatusCode::INTERNAL,
            "Failed to observe session");
    }
}

grpc::Status SharingServiceImpl::ShareSession(
    grpc::ServerContext* ctx, const agent_communication::ShareSessionRequest* request,
    agent_communication::ShareSessionResponse* response) {
    (void)ctx;
    std::string owner_id;
    const auto auth = requireOwnerSession(owner_id);
    if (!auth.ok()) return auth;
    if (!store_ || !domain_repository_) {
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "Sharing persistence is not configured");
    }
    // Only read-only sharing is supported.
    if (request->mode() != "view") {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "Only read-only 'view' sharing is supported");
    }
    if (request->context_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "context_id is required");
    }
    if (request->expiry_days() < 0 || request->expiry_days() > 3650) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "expiry_days must be in [0, 3650] (0 = no expiry)");
    }
    // Sharing requires owning the conversation; foreign ids look NOT_FOUND.
    if (!domain_repository_->getConversationById(owner_id, request->context_id())
             .has_value()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "Conversation not found: " + request->context_id());
    }

    const std::string expires_at =
        request->expiry_days() > 0 ? isoUtcAfterDays(request->expiry_days()) : "";

    // Declared outside the guard (the response echoes them) but generated
    // inside it — a failing entropy source must not escape the handler.
    std::string raw_token;
    std::string token_hash;
    std::string share_id;
    try {
        raw_token = randomHex(48);
        token_hash = sha256Hex(raw_token);
        share_id = "share-" + randomHex(12);
        bool inserted = false;
        store_->executeTransaction([&](pqxx::work& transaction) {
            const auto result = transaction.exec_params(
                "INSERT INTO shares (id, owner_id, conversation_id, token_hash, permission,"
                " expires_at) VALUES ($1, $2, $3, $4, 'view',"
                " COALESCE(NULLIF($5, '')::timestamptz, NULL)) RETURNING id",
                share_id, owner_id, request->context_id(), token_hash, expires_at);
            inserted = !result.empty();
        });
        if (!inserted) {
            return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to persist share");
        }
    } catch (const std::exception& error) {
        LOG_ERROR(std::string("ShareSession persist failed: ") + error.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to persist share");
    }

    response->mutable_status()->set_code(0);
    response->mutable_status()->set_message("OK");
    response->set_share_id(share_id);
    // Relative link only — the frontend assembles any absolute URL itself.
    response->set_share_url("/share/" + raw_token);
    // The raw bearer token is returned exactly once, at creation time.
    response->set_token(raw_token);
    response->set_expires_at(expires_at);
    LOG_INFO("ShareSession: share=" + share_id + " conversation=" +
             request->context_id() + " expires=" + (expires_at.empty() ? "never" : expires_at));
    return grpc::Status::OK;
}

grpc::Status SharingServiceImpl::ReadSharedConversation(
    grpc::ServerContext* ctx, const agent_communication::ReadSharedConversationRequest* request,
    agent_communication::ReadSharedConversationResponse* response) {
    (void)ctx;
    // Deliberately NO authentication: this is the restricted public read
    // path. Possession of the raw token is the only credential, and the
    // response is sanitized (no owner identity fields exist in the schema).
    if (!store_ || !domain_repository_) {
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "Sharing persistence is not configured");
    }
    if (request->token().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "token is required");
    }
    // deep-review R8: the raw token is the only credential on this
    // unauthenticated endpoint — cap its length BEFORE hashing so anonymous
    // callers cannot make the server digest arbitrarily large inputs.
    // (Real tokens are 96 lowercase hex chars; the cap only guards against
    // multi-MB inputs, deliberately not enforcing the exact shape so
    // operator-inserted test/legacy hashes keep their semantic errors.)
    constexpr std::size_t kMaxShareTokenLength = 256;
    if (request->token().size() > kMaxShareTokenLength) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Share link not found");
    }

    struct ShareRow {
        std::string owner_id;
        std::string conversation_id;
        std::string created_at;
        bool revoked = false;
        bool expired = false;
        bool found = false;
    } row;

    try {
        store_->executeTransaction([&](pqxx::work& transaction) {
            const auto result = transaction.exec_params(
                "SELECT owner_id, conversation_id, created_at::text,"
                " (revoked_at IS NOT NULL) AS revoked,"
                " (expires_at IS NOT NULL AND expires_at <= NOW()) AS expired"
                " FROM shares WHERE token_hash = $1",
                sha256Hex(request->token()));
            if (!result.empty()) {
                row.found = true;
                row.owner_id = result[0][0].as<std::string>();
                row.conversation_id = result[0][1].as<std::string>();
                row.created_at = result[0][2].as<std::string>();
                row.revoked = result[0][3].as<bool>();
                row.expired = result[0][4].as<bool>();
            }
        });
    } catch (const std::exception& error) {
        LOG_ERROR(std::string("ReadSharedConversation lookup failed: ") + error.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, "Share lookup failed");
    }

    if (!row.found) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Share link not found");
    }
    if (row.revoked) {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                            "Share link has been revoked");
    }
    if (row.expired) {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                            "Share link has expired");
    }

    try {
        const auto conversation =
            domain_repository_->getConversationById(row.owner_id, row.conversation_id);
        response->set_title(conversation ? conversation->title : "");
        response->set_shared_at(row.created_at);
        for (const auto& message :
             domain_repository_->listMessages(row.owner_id, row.conversation_id)) {
            auto* shared_message = response->add_messages();
            shared_message->set_role(message.role);
            shared_message->set_content(message.content);
            shared_message->set_sequence_no(message.sequence_no);
            shared_message->set_created_at(message.created_at);
        }
    } catch (const std::exception& error) {
        LOG_ERROR(std::string("ReadSharedConversation read failed: ") + error.what());
        return grpc::Status(
            common::isPostgresError(error) ? grpc::StatusCode::UNAVAILABLE
                                           : grpc::StatusCode::INTERNAL,
            "Failed to read shared conversation");
    }
    response->mutable_status()->set_code(0);
    response->mutable_status()->set_message("OK");
    return grpc::Status::OK;
}

grpc::Status SharingServiceImpl::ListShares(
    grpc::ServerContext* ctx, const agent_communication::ListSharesRequest* request,
    agent_communication::ListSharesResponse* response) {
    (void)ctx;
    (void)request;
    std::string owner_id;
    const auto auth = requireOwnerSession(owner_id);
    if (!auth.ok()) return auth;
    if (!store_) {
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "Sharing persistence is not configured");
    }

    try {
        store_->executeTransaction([&](pqxx::work& transaction) {
            const auto rows = transaction.exec_params(
                "SELECT id, conversation_id, permission, created_at::text,"
                " COALESCE(expires_at::text, ''), (revoked_at IS NOT NULL),"
                " COALESCE(revoked_at::text, '')"
                " FROM shares WHERE owner_id = $1 ORDER BY created_at DESC",
                owner_id);
            for (const auto& sql_row : rows) {
                auto* entry = response->add_shares();
                entry->set_share_id(sql_row[0].as<std::string>());
                entry->set_conversation_id(sql_row[1].as<std::string>());
                entry->set_permission(sql_row[2].as<std::string>());
                entry->set_created_at(sql_row[3].as<std::string>());
                entry->set_expires_at(sql_row[4].as<std::string>());
                entry->set_revoked(sql_row[5].as<bool>());
                entry->set_revoked_at(sql_row[6].as<std::string>());
                // The raw token is never returned after creation — the
                // listing only ever sees the hash, which is not exposed.
            }
        });
    } catch (const std::exception& error) {
        LOG_ERROR(std::string("ListShares failed: ") + error.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to list shares");
    }

    response->mutable_status()->set_code(0);
    response->mutable_status()->set_message("OK");
    return grpc::Status::OK;
}

grpc::Status SharingServiceImpl::RevokeShare(
    grpc::ServerContext* ctx, const agent_communication::RevokeShareRequest* request,
    agent_communication::RevokeShareResponse* response) {
    (void)ctx;
    std::string owner_id;
    const auto auth = requireOwnerSession(owner_id);
    if (!auth.ok()) return auth;
    if (!store_) {
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "Sharing persistence is not configured");
    }
    if (request->share_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "share_id is required");
    }

    bool revoked = false;
    try {
        store_->executeTransaction([&](pqxx::work& transaction) {
            const auto result = transaction.exec_params(
                "UPDATE shares SET revoked_at = NOW(), updated_at = NOW()"
                " WHERE id = $1 AND owner_id = $2 RETURNING id",
                request->share_id(), owner_id);
            revoked = !result.empty();
        });
    } catch (const std::exception& error) {
        LOG_ERROR(std::string("RevokeShare failed: ") + error.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to revoke share");
    }
    if (!revoked) {
        // Missing or foreign share id — identical surface (no existence leak).
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "Share not found: " + request->share_id());
    }
    response->mutable_status()->set_code(0);
    response->mutable_status()->set_message("OK");
    LOG_INFO("RevokeShare: share=" + request->share_id() + " owner=" + owner_id);
    return grpc::Status::OK;
}

grpc::Status SharingServiceImpl::SaveTemplate(
    grpc::ServerContext* ctx, const agent_communication::SaveTemplateRequest* request,
    agent_communication::SaveTemplateResponse* response) {
    (void)ctx;
    std::string owner_id;
    const auto auth = requireOwnerSession(owner_id);
    if (!auth.ok()) return auth;
    if (!store_) {
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "Sharing persistence is not configured");
    }
    if (request->name().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "Template name is required");
    }
    // Size guard BEFORE parsing: nlohmann::json parses recursively, so an
    // oversized/deeply nested payload could exhaust the stack. Definitions
    // above 64 KiB are rejected outright.
    static constexpr std::size_t kMaxDefinitionBytes = 65536;
    if (request->dag_json().size() > kMaxDefinitionBytes) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "Template definition exceeds 64 KiB");
    }
    // Validate the definition BEFORE persisting: it must be a JSON object
    // carrying a non-empty initial_message string.
    const auto definition = nlohmann::json::parse(request->dag_json(), nullptr, false);
    if (definition.is_discarded() || !definition.is_object()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "Template definition must be valid JSON object");
    }
    if (!definition.contains("initial_message") ||
        !definition["initial_message"].is_string() ||
        definition["initial_message"].get<std::string>().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "Template definition must contain a non-empty 'initial_message'");
    }

    std::string template_id;
    try {
        template_id = "tpl-" + randomHex(12);
        bool inserted = false;
        store_->executeTransaction([&](pqxx::work& transaction) {
            const auto result = transaction.exec_params(
                "INSERT INTO workflow_templates (id, owner_id, name, description, definition)"
                " VALUES ($1, $2, $3, $4, $5::jsonb) RETURNING id",
                template_id, owner_id, request->name(), request->description(),
                definition.dump());
            inserted = !result.empty();
        });
        if (!inserted) {
            return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to persist template");
        }
    } catch (const pqxx::unique_violation&) {
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS,
                            "A template with this name already exists");
    } catch (const std::exception& error) {
        LOG_ERROR(std::string("SaveTemplate persist failed: ") + error.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to persist template");
    }

    response->mutable_status()->set_code(0);
    response->mutable_status()->set_message("OK");
    response->set_template_id(template_id);
    LOG_INFO("SaveTemplate: template=" + template_id + " name=" + request->name());
    return grpc::Status::OK;
}

grpc::Status SharingServiceImpl::ListTemplates(
    grpc::ServerContext* ctx, const agent_communication::ListTemplatesRequest* request,
    agent_communication::ListTemplatesResponse* response) {
    (void)ctx;
    (void)request;
    std::string owner_id;
    const auto auth = requireOwnerSession(owner_id);
    if (!auth.ok()) return auth;
    if (!store_) {
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "Sharing persistence is not configured");
    }

    try {
        store_->executeTransaction([&](pqxx::work& transaction) {
            const auto rows = transaction.exec_params(
                "SELECT id, name, description, definition::text, created_at::text, version"
                " FROM workflow_templates WHERE owner_id = $1 ORDER BY created_at DESC",
                owner_id);
            for (const auto& sql_row : rows) {
                auto* entry = response->add_templates();
                entry->set_template_id(sql_row[0].as<std::string>());
                entry->set_name(sql_row[1].as<std::string>());
                entry->set_description(sql_row[2].as<std::string>());
                entry->set_definition(sql_row[3].as<std::string>());
                entry->set_created_at(sql_row[4].as<std::string>());
                entry->set_version(sql_row[5].as<int>());
            }
        });
    } catch (const std::exception& error) {
        LOG_ERROR(std::string("ListTemplates failed: ") + error.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to list templates");
    }

    response->mutable_status()->set_code(0);
    response->mutable_status()->set_message("OK");
    return grpc::Status::OK;
}

grpc::Status SharingServiceImpl::GetTemplate(
    grpc::ServerContext* ctx, const agent_communication::GetTemplateRequest* request,
    agent_communication::GetTemplateResponse* response) {
    (void)ctx;
    std::string owner_id;
    const auto auth = requireOwnerSession(owner_id);
    if (!auth.ok()) return auth;
    if (!store_) {
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "Sharing persistence is not configured");
    }
    if (request->template_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "template_id is required");
    }

    bool found = false;
    try {
        store_->executeTransaction([&](pqxx::work& transaction) {
            const auto rows = transaction.exec_params(
                "SELECT id, name, description, definition::text, created_at::text, version"
                " FROM workflow_templates WHERE id = $1 AND owner_id = $2",
                request->template_id(), owner_id);
            if (!rows.empty()) {
                found = true;
                auto* entry = response->mutable_template_();
                entry->set_template_id(rows[0][0].as<std::string>());
                entry->set_name(rows[0][1].as<std::string>());
                entry->set_description(rows[0][2].as<std::string>());
                entry->set_definition(rows[0][3].as<std::string>());
                entry->set_created_at(rows[0][4].as<std::string>());
                entry->set_version(rows[0][5].as<int>());
            }
        });
    } catch (const std::exception& error) {
        LOG_ERROR(std::string("GetTemplate failed: ") + error.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to read template");
    }
    if (!found) {
        // Missing or cross-owner template ids share one surface.
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "Template not found: " + request->template_id());
    }
    response->mutable_status()->set_code(0);
    response->mutable_status()->set_message("OK");
    return grpc::Status::OK;
}

grpc::Status SharingServiceImpl::UseTemplate(
    grpc::ServerContext* ctx, const agent_communication::UseTemplateRequest* request,
    agent_communication::UseTemplateResponse* response) {
    (void)ctx;
    std::string owner_id;
    const auto auth = requireOwnerSession(owner_id);
    if (!auth.ok()) return auth;
    if (!store_ || !domain_repository_) {
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "Sharing persistence is not configured");
    }
    if (request->template_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "template_id is required");
    }

    std::string name;
    std::string definition_text;
    bool found = false;
    try {
        store_->executeTransaction([&](pqxx::work& transaction) {
            // Owner-scoped lookup: a foreign template id is NOT_FOUND and can
            // never seed a conversation for another user.
            const auto rows = transaction.exec_params(
                "SELECT name, definition::text FROM workflow_templates"
                " WHERE id = $1 AND owner_id = $2",
                request->template_id(), owner_id);
            if (!rows.empty()) {
                found = true;
                name = rows[0][0].as<std::string>();
                definition_text = rows[0][1].as<std::string>();
            }
        });
    } catch (const std::exception& error) {
        LOG_ERROR(std::string("UseTemplate lookup failed: ") + error.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to read template");
    }
    if (!found) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "Template not found: " + request->template_id());
    }

    const auto definition = nlohmann::json::parse(definition_text, nullptr, false);
    if (definition.is_discarded() || !definition.is_object() ||
        !definition.contains("initial_message") ||
        !definition["initial_message"].is_string() ||
        definition["initial_message"].get<std::string>().empty()) {
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "Stored template definition is invalid");
    }
    const std::string initial_message = definition["initial_message"].get<std::string>();

    std::string context_id;
    try {
        // Create a REAL conversation for the current owner through the same
        // repository path the durable Query pipeline uses.
        context_id = "tpl-ctx-" + randomHex(12);
        if (!domain_repository_->ensureConversation(owner_id, context_id, name)) {
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                "Failed to create conversation from template");
        }
        if (!domain_repository_
                 ->appendMessageAutoSequence(owner_id, context_id, "user", initial_message)
                 .has_value()) {
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                "Failed to write initial template message");
        }
    } catch (const std::exception& error) {
        LOG_ERROR(std::string("UseTemplate persist failed: ") + error.what());
        return grpc::Status(
            common::isPostgresError(error) ? grpc::StatusCode::UNAVAILABLE
                                           : grpc::StatusCode::INTERNAL,
            "Failed to create conversation from template");
    }

    response->mutable_status()->set_code(0);
    response->mutable_status()->set_message("OK");
    response->set_context_id(context_id);
    LOG_INFO("UseTemplate: template=" + request->template_id() +
             " conversation=" + context_id + " owner=" + owner_id);
    return grpc::Status::OK;
}

}} // namespaces
