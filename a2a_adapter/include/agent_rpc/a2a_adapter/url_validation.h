/**
 * @file url_validation.h
 * @brief P21 SSRF defense-in-depth: agent URL validation and host resolution
 *        blacklisting shared by the direct paths and the DAG delegation path.
 */

#pragma once

#include <string>
#include <vector>

namespace agent_rpc {
namespace a2a_adapter {

/**
 * @brief L1 + L3: validate an agent URL with a real parse.
 *
 * Enforces: non-empty URL, no leading whitespace/control bytes, scheme is
 * strictly http/https (case-insensitive), no userinfo (@), no ambiguity
 * bytes (backslash) in the authority, and the port (when present) belongs
 * to the whitelist {80, 443} plus NEXUSAI_AGENT_ALLOWED_PORTS
 * (comma-separated). A URL without an explicit port is accepted (scheme
 * default port applies).
 *
 * @param url   Agent URL to validate
 * @param err   Filled with a rejection reason when returning false
 * @return true when the URL passes all checks
 */
bool validateAgentUrl(const std::string& url, std::string& err);

/**
 * @brief Whether the strict SSRF layers (L2 host blacklist, L3 port
 * whitelist) are enabled via NEXUSAI_SSRF_STRICT=1.
 * L1 (scheme/userinfo parse) is always on; strict layers default off so
 * local/dev agents on loopback or custom ports keep working unchanged.
 */
bool ssrfStrictModeEnabled();

/**
 * @brief Split a validated agent URL into host and explicit port.
 * The port string is empty when the URL carries no explicit port (the
 * scheme default applies).
 *
 * @param url   Validated agent URL
 * @param host  Filled with the authority host
 * @param port  Filled with the explicit port (empty when absent)
 * @return true on success (false only for malformed input)
 */
bool splitAgentUrlHostPort(const std::string& url,
                           std::string& host,
                           std::string& port);

/**
 * @brief L2: resolve a host and verify every resulting address is outside
 * the private / loopback / link-local / metadata ranges.
 *
 * Blacklist: 0.0.0.0/8, 10.0.0.0/8, 127.0.0.0/8, 169.254.0.0/16,
 * 172.16.0.0/12, 192.168.0.0/16, IPv6 ::, ::1, fe80::/10, fc00::/7,
 * and IPv4-mapped IPv6 forms of the above. Results are cached per host
 * with a short TTL so repeated calls avoid per-request getaddrinfo; the
 * caller should still pin the returned addresses to the connection
 * (CURLOPT_RESOLVE) to close the DNS-rebinding TOCTOU window.
 *
 * @param host     Hostname or IP literal to resolve
 * @param out_ips  Filled with the validated IP strings (presentation form)
 * @param err      Filled with a rejection reason when returning false
 * @return true when the host resolves and every address is allowed
 */
bool validateResolvedHost(const std::string& host,
                          std::vector<std::string>& out_ips,
                          std::string& err);

} // namespace a2a_adapter
} // namespace agent_rpc
