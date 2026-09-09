# 分享与资产指南（Share / Replay / Export / Template）

## 概述

NexusAI 的分享与资产能力覆盖四类场景：把一次会话以只读链接**分享**出去（TTL + 可撤销）、把历史查询**重放**复现（Replay）、把会话**导出**为 Markdown/HTML（Export）、把编排结构沉淀为可复用的**模板**（Template）。

这些能力在 7-PR 大优化后统一遵循两条安全底线：

1. **高熵令牌 + 摘要存储**：分享 token 由 `std::random_device` 熵源直接生成，PostgreSQL 只存其 SHA-256 摘要——数据库泄露不等于分享泄露；
2. **异常固定脱敏**：Replay / Export 的失败消息使用固定文案，**不回显 `error.what()`**——内部异常细节不外泄给客户端。

## 核心特性

| 特性 | 描述 |
|------|------|
| 96 字符高熵 token | 48 字节逐字节取自 `std::random_device`，hex 编码后 96 字符 |
| 仅存 SHA-256 摘要 | 原始 token 只在创建响应中返回一次，PG 存摘要 |
| TTL + 撤销 | `expiry_days` 控制有效期（0 = 永不过期），owner 可随时 RevokeShare |
| 公共读白名单脱敏 | ReadSharedConversation 无需登录，但消息体不含任何身份信息 |
| 双模式重放 | ReplayQuery 支持 exact（完整复现）与 route（只复现路由） |
| 双格式导出 | ExportConversation 输出 Markdown / HTML |
| 命名模板资产 | Template 以 JSONB 存储服务端校验过的 DAG 定义；同名唯一，无版本历史（workflow_templates.version 列存在但恒为默认值） |

## 机制一：Share 分享（TTL + 撤销）

### 创建分享

`SharingService/ShareSession` 为一个会话创建只读分享：

```protobuf
message ShareSessionRequest {
    string context_id = 1;
    string mode = 2;           // 仅支持 "view"
    int32 expiry_days = 3;     // 分享链接 TTL（0 = 永不过期）
}
```

服务端流程：

1. **生成 token**：`randomHex(48)` —— 48 个字节逐字节取自 `std::random_device` 熵源，hex 编码为 96 字符字符串。不使用伪随机数，不可预测；
2. **摘要落库**：PG 的分享行只保存 token 的 SHA-256 摘要与元数据（share_id 形如 `share-<24字符hex>`），原文不留存；
3. **一次性返回**：响应中 `token` 字段是原始令牌的唯一露出时机，`share_url` 是相对路径 `/share/<token>`（绝对地址由前端拼接），`expires_at` 为 ISO-8601 过期时间（永不过期时为空）。

`expiry_days` 的边界语义：负值被拒绝，`0` 表示永不过期。

### 公共只读访问

`ReadSharedConversation` 是**无需登录**的公共读接口，凭原始 token 换回会话内容。脱敏是结构性的：

- 服务端用 token 的 SHA-256 摘要匹配分享行（校验 TTL 与 revoked 状态）；
- 返回的 `SharedMessage` 刻意**不携带任何 owner / 用户身份字段**——proto 层面就不存在泄露通道；
- 返回内容：会话 title、按 `sequence_no` 排序的消息、分享时间。

### 列表与撤销

- `ListShares`：owner 只取认证上下文（请求不接受任何客户端提供的 user id），返回本 owner 的全部 `ShareEntry`（含 `revoked` / `revoked_at` / `expires_at`）；
- `RevokeShare`：按 `share_id` 撤销。撤销后的 token 即使仍在 TTL 内也无法再读取——这是"链接泄露后止损"的最后手段。

## 机制二：Replay 查询重放

`OrchestrationService/ReplayQuery` 复现一次历史查询，支持两种模式：

| mode | 语义 |
|------|------|
| `exact` | 完整复现：路由 + 执行全链路重跑 |
| `route` | 只复现路由决策，不真正执行 |

owner 从认证上下文解析（`AuthInterceptor::currentUserId()`）。失败时返回的异常消息是**固定脱敏文案，不回显 `error.what()`**——排查问题应走服务端日志与 PG trace，而不是依赖客户端错误细节。

## 机制三：Export 会话导出

`OrchestrationService/ExportConversation` 将会话导出为两种格式：

- **Markdown**：适合归档、贴入文档系统；
- **HTML**：适合独立分发浏览。

与 Replay 同样遵循异常固定脱敏：导出失败只返回固定文案。导出的内容边界即会话消息本身，不包含平台内部元数据。

## 机制四：Template 编排模板

模板把一次 DAG 编排沉淀为可复用资产：

| RPC | 语义 |
|------|------|
| `SaveTemplate` | 保存模板（name / description / dag_json），定义经服务端校验后以 JSONB 存储 |
| `UseTemplate` | 用模板实例化一个新会话（返回 `context_id`） |
| `ListTemplates` | 列出当前 owner 的模板 |
| `GetTemplate` | 读取单个模板详情 |

模板以 `name` 唯一：`SaveTemplate` 对同名模板返回 `ALREADY_EXISTS`，不存在“修改即新版本、历史不被覆盖”的更新或版本递增路径。`workflow_templates.version` 列保留供未来版本化，当前恒为默认值——面试/文档口径请按“命名唯一、无版本管理”表述。

## 使用示例

### 创建分享并读取（经 HTTP 网关）

```bash
GW=http://localhost:8081
AUTH="Authorization: Bearer $TOKEN"

# 1. 创建 7 天有效期的只读分享
curl -X POST $GW/agent_communication.SharingService/ShareSession \
  -H "$AUTH" -H 'Content-Type: application/json' \
  -d '{"context_id":"conv-demo-001","mode":"view","expiry_days":7}'
# → {"share_id":"share-xxxx","share_url":"/share/<96字符token>","token":"<96字符token>","expires_at":"2026-08-17T..."}

# 2. 任何人（无需登录）凭 token 读取分享内容
curl -X POST $GW/agent_communication.SharingService/ReadSharedConversation \
  -H 'Content-Type: application/json' \
  -d '{"token":"<96字符token>"}'
# → {"title":"...","messages":[{"role":"user","content":"...","sequence_no":1,...}],...}

# 3. owner 查看自己的分享清单
curl -X POST $GW/agent_communication.SharingService/ListShares \
  -H "$AUTH" -H 'Content-Type: application/json' -d '{}'

# 4. 发现链接外泄？立即撤销
curl -X POST $GW/agent_communication.SharingService/RevokeShare \
  -H "$AUTH" -H 'Content-Type: application/json' \
  -d '{"share_id":"share-xxxx"}'
```

### 重放与导出

```bash
# 完整重放一次历史查询
curl -X POST $GW/agent_communication.OrchestrationService/ReplayQuery \
  -H "$AUTH" -H 'Content-Type: application/json' \
  -d '{"trace_id":"trace-xxxx","mode":"exact"}'

# 导出会话为 Markdown
curl -X POST $GW/agent_communication.OrchestrationService/ExportConversation \
  -H "$AUTH" -H 'Content-Type: application/json' \
  -d '{"context_id":"conv-demo-001","format":"markdown"}'
```

### 模板沉淀与复用

```bash
# 保存模板
curl -X POST $GW/agent_communication.SharingService/SaveTemplate \
  -H "$AUTH" -H 'Content-Type: application/json' \
  -d '{"name":"每日摘要流水线","description":"检索→摘要→推送","dag_json":"{...}"}'
# → {"template_id":"tpl-xxxx"}

# 用模板实例化新会话
curl -X POST $GW/agent_communication.SharingService/UseTemplate \
  -H "$AUTH" -H 'Content-Type: application/json' \
  -d '{"template_id":"tpl-xxxx"}'
# → {"context_id":"conv-new-xxxx"}
```

## 注意事项

1. **token 只在创建时返回一次**：PG 只存 SHA-256 摘要，丢失 token 等于链接作废，只能重新创建分享；请妥善保存。
2. **不要把 token 写进日志或 URL 参数埋点**：它是 bearer 凭证，任何拿到它的人都能读取分享内容（TTL 内且未撤销）。
3. **`mode` 目前只支持 `"view"`**：编辑类分享不在能力面内，不要依赖。
4. **`expiry_days` 传负值会被拒绝**：需要永不过期请显式传 `0`。
5. **Revoke 是最快止损手段**：token 一旦怀疑泄露，撤销立即生效，无需等待 TTL 到期。
6. **Replay / Export 的错误信息是刻意的固定文案**：这不是信息丢失，而是防内部细节外泄的设计；排障请看服务端日志与 PG 中的 trace/query_log 记录。
7. **分享内容的脱敏是结构性的**：`SharedMessage` 不含身份字段，但会话消息本身的用户输入内容会如实展示——分享前请自行确认会话内容不涉敏。

## 相关文档

- [Durable Query Pipeline 指南](durable-query-pipeline-guide.md)
- [工作流控制指南](workflow-control-guide.md)
- [部署指南](deployment.md)
