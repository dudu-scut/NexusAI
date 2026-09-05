# Durable Query Pipeline 指南

## 概述

Durable Query Pipeline（持久化查询管线）是 NexusAI 在 7-PR 大优化后的核心查询链路：登录用户发起 `Query` / `QueryStream` 后，整条链路以 PostgreSQL 为事实源，六步串行推进，任何一步失败都会落库明确的终态；成功或失败路径最终都通过一次 CAS（compare-and-swap）完成"恰好一次"的终态持久化。

本指南讲解管线的工作机制、幂等主键设计、Token 预算预留与 finalize CAS 语义，并给出使用示例与注意事项。

## 核心特性

| 特性 | 描述 |
|------|------|
| Owner 只取认证上下文 | 查询归属者一律来自认证令牌，绝不采信请求体中的 user_id |
| 六步串行管线 | auth → 会话 → running 落库 → 预算预留 → SystemContext → finalize |
| 确定性幂等主键 | 消息/用量/trace 主键全部由 request_id 确定性派生，重试零重复 |
| PostgreSQL 事实源 | 查询、消息、trace、成本、预算全部持久化 PG；Redis 仅缓存 |
| 预算原子预留 | PG 事务内 advisory lock + counter upsert，拒绝先落 rejected |
| 恰好一次终态 | finalize 用原子 CAS 保证每次运行只持久化一次终态；内部 PG 故障会释放 CAS 并重入落 failed，不会把行卡在 running |
| 终态不可回退 | 终态行的 UPDATE 带状态守卫（WHERE status NOT IN 终态集合），迟到的写入无法改写已持久化终态 |
| 重放短路 | 命中既有终态行的同 request_id 请求直接返回落库结果，不重跑 LLM；planned 请求重放返回 FAILED_PRECONDITION 引导走 ExecutePlan |
| 两阶段计划执行 | `plan_only` 查询规划后交付 DAG 并落 `planned` 终态（规划照常计费），确认后经独立幂等的 ExecutePlan 执行 |

## 背景：为什么需要 Durable Pipeline

优化前的查询链路存在三类问题：

1. **身份可伪造**：请求体中的 `user_id` 被直接采信，任何登录用户都可以冒充他人发起查询。
2. **状态易丢失**：运行时状态散落在内存与 Redis，rpc-server 重启后查询记录、成本、trace 无从追溯。
3. **重试有副作用**：客户端超时重试会重复写消息、重复扣预算，缺少幂等锚点。

Durable Pipeline 的解决思路是：**把身份锚定在认证上下文、把所有状态锚定在 PostgreSQL、把所有写操作锚定在确定性主键上**。

## 六步管线机制

```
用户 Query/QueryStream（携带 Bearer token）
        │
        ▼
┌─ Step 1: auth owner ─────────────────────────────┐
│  AuthInterceptor::currentUserId()                │
│  请求体 user_id 被无条件忽略                       │
└──────────────────────────────────────────────────┘
        │
        ▼
┌─ Step 2: ensureConversation ─────────────────────┐
│  subtransaction 内确认/创建会话（幂等）            │
│  跨 owner 冲突直接拒绝                             │
└──────────────────────────────────────────────────┘
        │
        ▼
┌─ Step 3: running 落库 ───────────────────────────┐
│  query_logs 写入 running 状态（id = request_id）  │
│  traces 写入 trace-<request_id>                   │
└──────────────────────────────────────────────────┘
        │
        ▼
┌─ Step 4: 预算 reserve ───────────────────────────┐
│  PG 事务原子预留估算 token（request_id 幂等）      │
│  超额 → query_logs 落 rejected → RESOURCE_EXHAUSTED │
└──────────────────────────────────────────────────┘
        │
        ▼
┌─ Step 5: SystemContext 组装 ─────────────────────┐
│  从 PG 读取历史/记忆/画像，组装系统上下文           │
│  （Redis 只作缓存，不作为事实源）                   │
└──────────────────────────────────────────────────┘
        │
        ▼
┌─ Step 6: finalizeDurableQuery（CAS）─────────────┐
│  compare_exchange_strong：恰好一次终态持久化       │
│  写入用户/助手消息 + 用量 + trace 终态             │
└──────────────────────────────────────────────────┘
```

### Step 1：Owner 只取认证上下文

管线第一步通过 `AuthInterceptor::currentUserId()` 解析查询归属者。请求体中即使携带 `user_id` 字段也会被无条件覆盖——这是防伪造的底线设计。后续所有落库记录的 `owner_id` 都来自这一步。

### Step 2：ensureConversation（subtransaction 幂等）

管线在 subtransaction 中确认或创建 owner 的会话行。幂等语义：

- 会话已存在且属于当前 owner → 直接通过；
- 会话不存在 → 创建；
- 会话存在但属于其他 owner → 拒绝（防跨用户访问）。

并发重试时多个请求可能同时走到创建分支，subtransaction + 唯一键保证只有一行落库，其余安全回滚。

### Step 3：running 落库

`query_logs` 写入 `running` 状态行，**query_log 的 id 就是 request_id**；同时在 `traces` 表写入 `trace-<request_id>` 行。从这一刻起，该查询在 PG 中"存在过"就是不可抵赖的事实，后续任何崩溃都能审计到这条 running 记录。

### Step 4：预算 reserve（详见下节）

四级 Token 配额在 PG 事务中原子预留。预留以 `request_id` 幂等；超额时先把 `query_logs` 落 `rejected` 终态，再返回 gRPC `RESOURCE_EXHAUSTED`（HTTP 网关映射为 429）。

### Step 5：SystemContext 组装

从 PostgreSQL 读取对话历史、长期记忆等持久化记录，组装发给 Agent 的 SystemContext。Redis 在这一层只承担缓存角色——缓存未命中时回源 PG，PG 永远是事实源。

### Step 6：finalizeDurableQuery（恰好一次 CAS）

管线的成功路径、失败路径、客户端断开（abort）路径都会尝试调用 `finalizeDurableQuery`。函数入口用 `std::atomic<bool>` + `compare_exchange_strong` 做 CAS：

- 第一个到达的调用者赢得 CAS，执行终态持久化（写消息、用量、trace 终态、query_log 终态）；
- 后续任何路径再调用时 CAS 失败，直接返回，**保证每次运行恰好一次终态持久化**。

## 幂等主键设计

管线所有持久化记录的主键都由 `request_id` 确定性派生，重试不会写出重复行：

| 记录 | 主键 | 说明 |
|------|------|------|
| 查询日志 | `request_id` | query_logs.id 即请求标识 |
| 用户消息 | `msg-user-<request_id>` | appendMessageAutoSequence 幂等写入 |
| 助手消息 | `msg-assistant-<request_id>` | 同上 |
| Token 用量 | `usage-<request_id>` | 预算预留与记账共用同一幂等键 |
| Trace | `trace-<request_id>` | 与 query_log 一对一 |

这套确定性主键带来两个直接收益：

1. **重试安全**：客户端超时重试、管线内部重试都只会触发主键冲突上报，不会产生第二条记录；被预算拒绝后重试成功的请求也只会被计费一次。
2. **可对账**：给定 request_id 就能在 PG 中拼出完整证据链（query_log + 两条消息 + usage + trace）。

### 重放短路（批次八）

幂等主键保证"不重复写"，批次八进一步保证"不重复跑"：`beginDurableRows` 发现 query_log 行已处于终态（completed / failed / cancelled / rejected / planned）时，本次调用直接短路——同步路径返回落库的 answer 与对应状态码，流式路径补发一条终态事件后返回。预算也只在首次真实执行时预留。语义约定：

- 重放 completed 返回 OK + 已落库回答；
- 重放 failed / rejected 返回与原终态一致的错误语义；
- 重放 planned 返回 `FAILED_PRECONDITION`，提示该计划的执行应走 `ExecutePlan` 确认通道，而不是重跑本查询。

## Token 预算：PG 原子预留

### 四级配额

| 环境变量 | 默认值 | 说明 |
|------|------|------|
| `NEXUSAI_BUDGET_GLOBAL_TOKENS` | `0`（无限） | 全局总配额 |
| `NEXUSAI_BUDGET_USER_DAILY_TOKENS` | `200000` | 单用户日配额 |
| `NEXUSAI_BUDGET_USER_MONTHLY_TOKENS` | `4000000` | 单用户月配额 |
| `NEXUSAI_BUDGET_SESSION_TOKENS` | `100000` | 单会话配额 |

### 预留语义

预留发生在 PG 事务内：advisory lock 覆盖"计数器首行尚不存在"的竞争场景，counter upsert 原子累加；预留以 `request_id` 幂等（同一 request_id 重复预留只计一次）。

估算 token 的口径是 `64 + question.size() / 4`（问题长度的线性近似），记账标记为 estimated——目前没有 provider 侧结算，精确用量回填是后续演进方向。

### 拒绝路径

任一层级超额时：

1. `query_logs` 先落 `rejected` 终态（拒绝本身也是持久化事实）；
2. 返回 gRPC `RESOURCE_EXHAUSTED`；
3. HTTP 网关将 `RESOURCE_EXHAUSTED` 映射为 **429**，错误体包含 `{error, code, code_name, details}`。

沙箱（sandbox）与对比流量**不豁免**预算，与主链路共用同一配额——预算以 PG 为事实源，不存在绕道。

## 使用示例

### 发起一次查询（经 HTTP 网关）

```bash
TOKEN=$(curl -s -X POST http://localhost:8081/agent_communication.auth.UserService/Login \
  -H 'Content-Type: application/json' \
  -d '{"username":"demo","password":"demo-pass"}' | python3 -c 'import sys,json;print(json.load(sys.stdin)["token"])')

curl -X POST http://localhost:8081/agent_communication.AIQueryService/Query \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{
    "request_id": "req-demo-001",
    "context_id": "conv-demo-001",
    "question": "帮我解释一下什么是幂等"
  }'
```

注意：请求体中不需要也不应该携带 `user_id`——owner 完全由 `Authorization` 令牌决定。

### 观察预算拒绝（429）

把会话配额调到极小后，长问题会被拒绝：

```bash
# rpc-server 启动时设置 NEXUSAI_BUDGET_SESSION_TOKENS=70
curl -i -X POST http://localhost:8081/agent_communication.AIQueryService/Query \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"request_id":"req-buster","context_id":"conv-demo-001","question":"<很长的提问……>"}'
# HTTP/1.1 429 Too Many Requests
# {"error":"...","code":8,"code_name":"RESOURCE_EXHAUSTED",...}
```

对应 PG 中 `query_logs` 该行的状态为 `rejected`。

### 重试的幂等表现

同一 `request_id` 重复调用（例如第一次网络超时后重试）：

- 消息、用量、trace 不会重复写入（主键冲突被仓储层上报而非抛异常）；
- 预算只被计费一次；
- `tests/e2e/e2e_pr_g_release.py` 中的 25 个断言覆盖了"短问题通过、超长问题被拒、拒绝可持久化"这条完整链路。

## 错误码与网关映射

管线返回的 gRPC 状态码经网关固定映射为 HTTP 状态码：

| gRPC 状态 | HTTP 状态 | 典型场景 |
|------|------|------|
| `UNAUTHENTICATED` | 401 | 未携带/无效令牌 |
| `PERMISSION_DENIED` | 403 | 角色不足（如 ADMIN-only RPC） |
| `NOT_FOUND` | 404 | 会话/资源不存在 |
| `ALREADY_EXISTS` | 409 | 幂等键冲突语义 |
| `RESOURCE_EXHAUSTED` | 429 | 预算超额 |
| `CANCELLED` | 499 | 客户端断开（abort 传播到管线） |
| `INVALID_ARGUMENT` | 400 | 参数校验失败（如 ExecutePlan 空 DAG / 环依赖） |
| `FAILED_PRECONDITION` | 412 | 委派深度超限、planned 重放等前置条件不满足 |
| `UNIMPLEMENTED` | 501 | RPC 未实现（如 RealTimeCommunication） |
| `UNAVAILABLE` | 503 | 后端过载 / 断路器打开 |
| `DEADLINE_EXCEEDED` | 504 | 请求预算耗尽 |

客户端断开连接时，网关通过 `res.on('close')` 触发流的 `cancel()`，abort 沿管线传播，finalize CAS 保证断连场景下落库的仍是恰好一次的终态。

## 注意事项

1. **不要在请求体里传 user_id**：它会被无条件忽略，依赖它做任何业务判断都会出错。
2. **request_id 必须由调用方保证唯一**：幂等主键全部派生自它。复用已终结请求的 request_id 会触发重放短路（返回落库结果，不重新执行）；需要重新执行请换新的 request_id。
3. **估算 token 不等于实际消耗**：当前口径是 `64 + question.size() / 4` 的估算值，预算规划请按估算口径留余量。
4. **rejected 是终态事实**：预算拒绝不会"消失"，排查费用问题时可直接在 PG 中审计 rejected 记录。
5. **迁移只追加**：管线依赖的表结构来自 `db/migrations`（V001–V015，只追加不修改），rpc-server 启动时自动迁移，迁移失败即中止启动。
6. **沙箱流量共享预算**：sandbox 请求不豁免任何配额层级，压测时注意合并计算。

### 两阶段计划执行（plan-only，批次八）

`AIQueryRequest.plan_only = true` 时，查询在规划完成后即停止：流式路径经既有 `plan` 事件交付完整 DAG 并补发 `awaiting_confirmation` 标记，随后以 `planned` 终态落库（规划 LLM 照常计预算，route 标注为 `plan-only`）；同步路径把 plan JSON 放入 answer 返回。前端展示"确认执行 / 放弃"，确认后把 DAG 组装为 `ExecutePlanRequest` 走独立的持久化执行通道（聚合回答落库为 assistant 消息回填会话）。

设计要点：**两阶段语义由两个各自幂等的持久化请求组合实现**，服务端没有"挂起等待确认"的状态机——不需要为挂起设计取消传播、超时自动审批或崩溃恢复。代价是重复确认会以新 request_id 各自独立执行（用户驱动的多次执行，各计各的预算）。

## 相关文档

- [启动指南](startup-guide.md)
- [部署指南](deployment.md)
- [工作流控制指南](workflow-control-guide.md)
