/**
 * @file url_validation.cpp
 * @brief P21 SSRF defense-in-depth implementation (L1 parse, L2 host
 *        blacklist with TTL cache, L3 port whitelist).
 */

#include "agent_rpc/a2a_adapter/url_validation.h"
#include "agent_rpc/common/env_loader.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace agent_rpc {
namespace a2a_adapter {

namespace {

// Strict mode gate: NEXUSAI_SSRF_STRICT=1 enables the L2 host blacklist
// enforcement and the L3 port whitelist. Default off keeps legacy behavior
// byte-equivalent (local/dev agents often live on loopback and non-standard
// ports); L1 (scheme/userinfo parse) is always on.
bool ssrfStrictEnabled() {
    static const bool enabled =
        agent_rpc::common::envOrDefault("NEXUSAI_SSRF_STRICT", "0") == "1";
    return enabled;
}

std::set<int> allowedPorts() {
    std::set<int> ports = {80, 443};
    const std::string extra = agent_rpc::common::envOrDefault(
        "NEXUSAI_AGENT_ALLOWED_PORTS", "");
    if (!extra.empty()) {
        std::istringstream stream(extra);
        std::string token;
        while (std::getline(stream, token, ',')) {
            const std::string trimmed = [&token]() {
                size_t first = token.find_first_not_of(" \t");
                size_t last = token.find_last_not_of(" \t");
                return first == std::string::npos
                           ? std::string()
                           : token.substr(first, last - first + 1);
            }();
            if (trimmed.empty()) continue;
            bool numeric = !trimmed.empty() &&
                           std::all_of(trimmed.begin(), trimmed.end(),
                                       [](unsigned char c) { return std::isdigit(c); });
            if (!numeric) continue;
            const int port = std::atoi(trimmed.c_str());
            if (port > 0 && port <= 65535) {
                ports.insert(port);
            }
        }
    }
    return ports;
}

// Split an authority string (host[:port], [v6]:port, or raw v6) into host
// and explicit port. Shared by validateAgentUrl and splitAgentUrlHostPort.
bool splitAuthority(const std::string& authority,
                    std::string& host,
                    std::string& port,
                    std::string& err) {
    host.clear();
    port.clear();
    if (authority.empty()) {
        err = "Agent URL authority must not be empty";
        return false;
    }
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos) {
            err = "Agent URL has an unterminated IPv6 literal";
            return false;
        }
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                err = "Agent URL has unexpected characters after the IPv6 literal";
                return false;
            }
            port = authority.substr(close + 2);
        }
    } else {
        const auto colon = authority.rfind(':');
        const auto first_colon = authority.find(':');
        if (colon == first_colon && colon != std::string::npos) {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        } else {
            host = authority;  // raw IPv6 (multiple colons) or no port
        }
    }
    if (host.empty()) {
        err = "Agent URL host must not be empty";
        return false;
    }
    return true;
}

// ── L2: IP blacklist ──────────────────────────────────────────────────────

bool isBlacklistedIpv4(const uint8_t* b) {
    if (b[0] == 0) return true;                       // 0.0.0.0/8
    if (b[0] == 10) return true;                      // 10.0.0.0/8
    if (b[0] == 127) return true;                     // 127.0.0.0/8
    if (b[0] == 169 && b[1] == 254) return true;      // 169.254.0.0/16
    if (b[0] == 172 && (b[1] >= 16 && b[1] <= 31)) return true;  // 172.16/12
    if (b[0] == 192 && b[1] == 168) return true;      // 192.168.0.0/16
    return false;
}

bool isBlacklistedIpv6(const uint8_t* b) {
    // :: (all zero)
    bool all_zero = true;
    for (int i = 0; i < 16; ++i) {
        if (b[i] != 0) { all_zero = false; break; }
    }
    if (all_zero) return true;

    // ::1
    bool is_loopback = true;
    for (int i = 0; i < 15; ++i) {
        if (b[i] != 0) { is_loopback = false; break; }
    }
    if (is_loopback && b[15] == 1) return true;

    // fe80::/10 (link-local)
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return true;
    // fc00::/7 (unique local)
    if ((b[0] & 0xfe) == 0xfc) return true;

    // IPv4-mapped (::ffff:a.b.c.d) — re-check the embedded IPv4.
    if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0 && b[4] == 0 &&
        b[5] == 0 && b[6] == 0 && b[7] == 0 && b[8] == 0 && b[9] == 0 &&
        b[10] == 0xff && b[11] == 0xff) {
        return isBlacklistedIpv4(b + 12);
    }
    return false;
}

// TTL cache for host resolutions (short TTL keeps the cache fresh without
// paying a getaddrinfo round-trip per request).
const std::chrono::seconds kResolveTtl{300};

struct ResolveCacheEntry {
    std::chrono::steady_clock::time_point stamped;
    std::vector<std::string> ips;
};

std::mutex& resolveCacheMutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, ResolveCacheEntry>& resolveCache() {
    static std::unordered_map<std::string, ResolveCacheEntry> cache;
    return cache;
}

} // anonymous namespace

bool ssrfStrictModeEnabled() {
    return ssrfStrictEnabled();
}

bool validateAgentUrl(const std::string& url, std::string& err) {
    if (url.empty()) {
        err = "Agent URL must not be empty";
        return false;
    }

    // Leading whitespace or control bytes are a rejection condition, not
    // something to silently trim (trimming would mask smuggling tricks).
    if (static_cast<unsigned char>(url[0]) <= 0x20) {
        err = "Agent URL must not start with whitespace or control bytes";
        return false;
    }

    const std::string::size_type sep = url.find("://");
    if (sep == std::string::npos) {
        err = "Agent URL must use http:// or https:// scheme";
        return false;
    }

    // Scheme: case-insensitive strict whitelist (lowercased before the
    // comparison, so any case of http/https is accepted).
    std::string scheme = url.substr(0, sep);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (scheme != "http" && scheme != "https") {
        err = "Agent URL must use http:// or https:// scheme";
        return false;
    }

    // Authority: everything after the separator up to the first '/', '?' or '#'.
    const std::string::size_type authority_start = sep + 3;
    std::string::size_type authority_end = authority_start;
    while (authority_end < url.size() &&
           url[authority_end] != '/' && url[authority_end] != '?' &&
           url[authority_end] != '#') {
        const unsigned char c = static_cast<unsigned char>(url[authority_end]);
        if (c <= 0x20) {
            err = "Agent URL authority contains whitespace or control bytes";
            return false;
        }
        if (c == '@') {
            err = "Agent URL must not contain userinfo";
            return false;
        }
        if (c == '\\') {
            err = "Agent URL authority contains an ambiguity byte";
            return false;
        }
        ++authority_end;
    }

    std::string authority =
        url.substr(authority_start, authority_end - authority_start);
    if (authority.empty()) {
        err = "Agent URL authority must not be empty";
        return false;
    }

    std::string host;
    std::string port;
    if (!splitAuthority(authority, host, port, err)) {
        return false;
    }

    // L3: port whitelist (strict mode only). Absent port → scheme default
    // (80/443) is fine.
    if (ssrfStrictEnabled() && !port.empty()) {
        bool numeric = std::all_of(port.begin(), port.end(),
                                   [](unsigned char c) { return std::isdigit(c); });
        if (!numeric) {
            err = "Agent URL port must be numeric";
            return false;
        }
        const long port_parsed = std::strtol(port.c_str(), nullptr, 10);
        if (port_parsed <= 0 || port_parsed > 65535) {
            err = "Agent URL port out of range";
            return false;
        }
        const int port_num = static_cast<int>(port_parsed);
        if (allowedPorts().count(port_num) == 0) {
            err = "Agent URL port " + port + " is not in the allowed list";
            return false;
        }
    }

    return true;
}

bool splitAgentUrlHostPort(const std::string& url,
                           std::string& host,
                           std::string& port) {
    host.clear();
    port.clear();
    const std::string::size_type sep = url.find("://");
    if (sep == std::string::npos) {
        return false;
    }
    std::string authority = url.substr(sep + 3);
    const auto end = authority.find_first_of("/?#");
    if (end != std::string::npos) {
        authority = authority.substr(0, end);
    }
    std::string err;
    return splitAuthority(authority, host, port, err);
}

bool validateResolvedHost(const std::string& host,
                          std::vector<std::string>& out_ips,
                          std::string& err) {
    out_ips.clear();
    if (host.empty()) {
        err = "Host must not be empty";
        return false;
    }

    // TTL cache hit: reuse the previously validated addresses.
    {
        std::lock_guard<std::mutex> lock(resolveCacheMutex());
        auto it = resolveCache().find(host);
        if (it != resolveCache().end()) {
            const auto age = std::chrono::steady_clock::now() - it->second.stamped;
            if (age < kResolveTtl) {
                out_ips = it->second.ips;
                return true;
            }
            resolveCache().erase(it);
        }
    }

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
        err = "Failed to resolve agent host: " + host;
        return false;
    }

    std::vector<std::string> ips;
    bool blacklisted = false;
    for (const struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        char buffer[INET6_ADDRSTRLEN] = {0};
        if (ai->ai_family == AF_INET) {
            const auto* sin = reinterpret_cast<const struct sockaddr_in*>(ai->ai_addr);
            const uint8_t* b =
                reinterpret_cast<const uint8_t*>(&sin->sin_addr.s_addr);
            if (isBlacklistedIpv4(b)) {
                blacklisted = true;
                break;
            }
            inet_ntop(AF_INET, &sin->sin_addr, buffer, sizeof(buffer));
        } else if (ai->ai_family == AF_INET6) {
            const auto* sin6 = reinterpret_cast<const struct sockaddr_in6*>(ai->ai_addr);
            const uint8_t* b =
                reinterpret_cast<const uint8_t*>(&sin6->sin6_addr.s6_addr);
            if (isBlacklistedIpv6(b)) {
                blacklisted = true;
                break;
            }
            inet_ntop(AF_INET6, &sin6->sin6_addr, buffer, sizeof(buffer));
        } else {
            continue;
        }
        ips.emplace_back(buffer);
    }
    freeaddrinfo(res);

    if (blacklisted || ips.empty()) {
        err = "Agent host resolves to a forbidden address: " + host;
        return false;
    }

    out_ips = ips;
    {
        std::lock_guard<std::mutex> lock(resolveCacheMutex());
        resolveCache()[host] = ResolveCacheEntry{
            std::chrono::steady_clock::now(), ips};
    }
    return true;
}

} // namespace a2a_adapter
} // namespace agent_rpc
