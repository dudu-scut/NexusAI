#pragma once

#include <cstddef>
#include <string>

namespace agent_rpc {
namespace common {

// P25 (批次十一): 入口级字符集白名单 — 与 sanitizeKeyComponent 的有损清洗
// 互补：sanitize 把 ":" 折叠成 "_"（非单射），本函数在数据进入键拼接前把
// 含这类字符的输入直接拒绝，让有损字符到不了清洗函数（清洗退化为纯纵深
// 防御）。规则 = 禁止 sanitize 有损字符集（: \n \r 0x01–0x1F）+ 允许字符集
// alnum ._- + 长度上限。
inline bool isSafeKeyComponent(const std::string& value,
                               std::size_t max_length = 128) {
    if (value.empty() || value.size() > max_length) {
        return false;
    }
    for (const unsigned char c : value) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                             c == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

// Agent 注册原料 host：hostname[:port] 结构 —— 主体限 alnum .-（DNS 主机名
// 不含下划线），可选的 :port 段必须为纯数字。与 isSafeKeyComponent 一样禁
// 止 sanitize 有损字符集。
inline bool isSafeHostComponent(const std::string& value,
                                std::size_t max_length = 255) {
    if (value.empty() || value.size() > max_length) {
        return false;
    }
    const std::size_t colon = value.find(':');
    const std::string host =
        colon == std::string::npos ? value : value.substr(0, colon);
    if (host.empty() || host.size() > 253) {
        return false;
    }
    for (const unsigned char c : host) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (!allowed) {
            return false;
        }
    }
    if (colon != std::string::npos) {
        const std::string port = value.substr(colon + 1);
        if (port.empty() || port.find(':') != std::string::npos ||
            port.size() > 5) {
            return false;
        }
        for (const unsigned char c : port) {
            if (c < '0' || c > '9') {
                return false;
            }
        }
    }
    return true;
}

}  // namespace common
}  // namespace agent_rpc
