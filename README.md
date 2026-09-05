<h1 align="center">NexusAI — 多 Agent 通信与工具选择框架</h1>

<div align="center">

**C++20 · gRPC · A2A 协议 · MCP · Vue 3**

高性能多 Agent 协作框架 — 四级智能路由 · DAG 任务编排 · 持久化查询管线 · 全链路可观测

[![C++](https://img.shields.io/badge/C%2B%2B-20-blue)](https://en.cppreference.com/w/cpp/20)
[![gRPC](https://img.shields.io/badge/gRPC-1.51%2B-green)](https://grpc.io/)
[![License](https://img.shields.io/badge/license-MIT-orange)](LICENSE)
[![测试](https://img.shields.io/badge/测试-37%20套-brightgreen)](tests/)

</div>

---

## 项目简介

NexusAI 是一个基于 C++20 与 gRPC 构建的高性能多 Agent 协作与智能工具选择框架。用户以自然语言提问，后端自动将复杂问题拆解为带依赖关系的子任务 DAG 并行执行，通过四级渐进式智能路由调度最合适的 Agent，经 A2A 协议完成 Agent 间通信，全程以流式方式将执行进度推送到前端可视化。

框架刚完成一轮系统性优化，核心链路已收敛为真实可验证的闭环：**登录用户发起 Query/QueryStream → 服务端从认证上下文取得数据归属者 owner（绝不采信请求体中的 user_id）→ PostgreSQL 持久化查询日志、消息、链路追踪、成本与预算 → 前端展示真实结果**。所有业务事实均以 PostgreSQL 为唯一持久事实源，Redis 仅承担缓存、心跳、限流与短锁等运行时职责。

## 核心特性

### Durable Query Pipeline（持久化查询管线）

每一次 Query / QueryStream 请求都按六步固定顺序执行，任何一步失败即终止并落库终态：

1. **认证取 owner**：owner 只从认证拦截器上下文获取，请求体中的 `user_id` 一律被无条件覆盖；未认证直接返回 UNAUTHENTICATED，不创建任何数据库行。
2. **确保会话存在**：按 `request_id`/`context_id` 幂等创建会话，同 owner 重复请求复用，跨 owner 拒绝。
3. **登记运行中记录**：在 `query_logs` 与 `traces` 表写入 status=running 的记录。
4. **预算预留**：四层预算（全局 / 用户日 / 用户月 / 会话）在 PostgreSQL 中预留额度，预算不足时先将记录落库为 rejected 终态，再返回 RESOURCE_EXHAUSTED。
5. **组装系统上下文**：从 PostgreSQL 读取会话消息与记忆摘要，组装 SystemContext 注入 AI 查询。
6. **单次终结**：以 compare-exchange 原子操作保证 finalize 恰好执行一次，写入消息、查询日志、追踪与 Token 台账。

管线以确定性主键实现幂等：消息 id 固定派生为 `msg-user-<request_id>` 与 `msg-assistant-<request_id>`，Token 台账主键为 `usage-<request_id>`。同一 `request_id` 的重试永远只产生一条预算预留与一条台账；已终结的请求再次被调用会**短路返回落库结果**，不会重跑 LLM；终态行带状态守卫，任何迟到写入都无法回退已持久化的终态（completed / failed / cancelled / rejected / planned）。客户端断开会持久化 cancelled 终态；预算拒绝持久化 rejected 终态。

### PostgreSQL 为持久事实源

- 会话、查询日志、链路追踪、预算预留、Token 台账、反馈、分享、Agent 注册表等全部业务事实落库 PostgreSQL，由 `db/migrations` 下 V001–V015 共 15 条只追加迁移建表（V014 健康快照、V015 记忆域）。
- RPC 服务端启动时自行执行迁移并记录校验和，不存在独立的 migrate 服务；迁移失败即启动中止。
- Redis 清空后所有业务数据仍可完整读取；Redis 仅用于对话历史缓存、Agent 心跳存活标记、限流与短时锁。
- 旧 `sql/` 参考 schema 已从仓库移除（内容被 V001+ 迁移完全取代）。

### DAG 任务编排引擎

TaskPlanner 调用 LLM 分析请求，自动生成带依赖关系的子任务 DAG，经 Kahn 拓扑排序分层后，TaskExecutor 以独立工作线程并行执行同层无依赖子任务（子任务超时可安全放弃，不阻塞后续层推进），前置任务的结果自动注入下游上下文，ResultAggregator 聚合多 Agent 输出为统一响应。全局超时防止无限等待，委派深度限制 5 层防止 Agent 递归失控，可配置重试策略自动处理瞬态错误。前端以 Mermaid 实时渲染 DAG 状态流转。

### 四级智能路由与熔断容错

路由引擎按 Embedding 向量语义匹配 → LLM 意图识别 → IDF 加权关键词匹配 → 兜底路由的顺序逐级降级，确保在各种条件下都能选出合适 Agent。用户反馈经 Beta 平滑聚合为路由质量系数并驱动加权选择；三态熔断器（Closed / Open / HalfOpen）按 Agent 粒度自动隔离故障；六种负载均衡策略含 FNV-1a 一致性哈希，支持运行时热切换。

### A2A 协议与 MCP 工具调用

A2A（Agent-to-Agent）协议基于 HTTP/JSON-RPC 2.0 实现，支持 `message/send` 同步调用与 `message/stream` SSE 流式调用，Task 状态机覆盖 Submitted → Running → Completed / Failed / Canceled 全生命周期。MCP 工具调用支持 STDIO 本地进程管道与 SSE 远程 HTTP 两种传输模式；集成 MCP Server 提供 6 个内置插件。MCP/RAG 为可选能力，默认关闭，需以 `-DENABLE_MCP=ON` 配置 CMake 显式启用。

### 工作流控制

- **Sandbox 沙箱**：隔离试运行环境，沙箱流量同样计入预算，不写入用户长期记忆。
- **Compare 对比**：同一问题并行发给多个 Agent，认证上下文快照传播到工作线程，结果并排对比。
- **Intervention 人工干预**：执行过程中可批准、修改或跳过指定动作，状态流转以单条 SQL 的 CAS 更新保证并发安全。
- **Undo 撤销**：在单个数据库事务内以原子 CAS 标记撤销并执行 inverse 补偿操作，补偿失败整体回滚、撤销动作不被消耗，可安全重试。
- **Autonomy 自主性分级**：用户可设置 Agent 自主级别，高消耗操作自动触发人工确认。

### 分享与资产

- **Replay 回放**：exact 模式以全新 request_id 走完整查询管线并生成新追踪，原追踪保持不变；route 模式仅对比历史路由决策与当前路由结果，不产生执行记录。
- **Export 导出**：将会话导出为 Markdown 或结构化 HTML，HTML 输出对用户内容做完整转义。
- **Share 分享**：生成 96 字符高熵一次性令牌，PostgreSQL 仅存储令牌的 SHA-256 哈希，支持 TTL 过期与主动撤销；公开读取接口只读且脱敏。
- **Template 模板市场**：保存、列举、应用会话模板，应用模板会为当前用户创建真实会话与初始消息。

### 全链路可观测性

TraceContext 以嵌套 Span 结构实现全链路追踪并跨线程自动传播，Span 数据批量刷入 Redis 保留 7 天，追踪事实同时持久化到 PostgreSQL 并按 owner 隔离查询。Token 成本按用户与日期聚合为报表，估算成本与精确成本分桶统计。`agent_invocations` 事实表由查询管线逐请求写入，后台任务按小时聚合调用指标。用户反馈聚合为路由质量信号并即时刷新，驱动路由决策持续优化。

### 三层记忆系统

记忆服务在每次查询前自动整合上下文注入 AI 请求：对话历史按 Agent 粒度隔离；用户长期记忆以 PostgreSQL 为事实源（V015 `user_memory_hints`，变更走批量事务并记录 append-only 事件表），Redis 仅作读投影（写后失效、读时重建），事实型/偏好型键冲突分别走规则覆盖保留历史与候选集消歧；用户陈述的事实经 `SystemContext.user_facts` 独立字段注入，与平台推断的画像（`[User Profile]`）分层区分；跨 Agent 摘要由 LLM 按目标 Agent 职责特化生成。大规模 hints 下可开启 Top-K 向量召回（`NEXUSAI_MEMORY_VECTOR_RECALL`）。

### 网关错误语义

Node.js 网关将 gRPC 状态码映射为稳定的 HTTP 语义码：UNAUTHENTICATED → 401、PERMISSION_DENIED → 403、NOT_FOUND → 404、ALREADY_EXISTS → 409、RESOURCE_EXHAUSTED → 429、CANCELLED → 499、INVALID_ARGUMENT → 400、FAILED_PRECONDITION → 412、UNIMPLEMENTED → 501、UNAVAILABLE → 503、DEADLINE_EXCEEDED → 504（INTERNAL/UNKNOWN/DATA_LOSS → 500）。一元调用错误返回统一结构化错误体；SSE 流以 in-band 结构化 error 事件传递错误后关闭，且已投递终态事件后不再追加第二帧错误；浏览器断开经网关传播为后端 stream.cancel()，服务端据此持久化 cancelled 终态。

### 认证与安全

密码采用 OpenSSL scrypt（EVP_PBE_scrypt，32 字节随机盐）哈希存储；会话令牌为 32 字节 CSPRNG 随机数，服务端仅存 SHA-256 摘要，会话事实源在 PostgreSQL `auth_sessions`（登出/封禁即时撤销），拦截器层附带短 TTL 读缓存（撤销延迟有上限）。gRPC 拦截器对全部接口鉴权，白名单内的公开 RPC 免认证。匹配 `NEXUSAI_ADMIN_USERNAME` 的用户在注册时获得 ADMIN 角色，作为 RegisterAgent / UnregisterAgent 等管理面 RPC 的强制门槛。

## 架构概览

```text
                     ┌──────────────────────────────┐
                     │  Browser（浏览器）            │
                     │  生产: Nginx :8080            │
                     │  开发: Vite :5173             │
                     └──────────────┬───────────────┘
                                    │ HTTP JSON / SSE
                     ┌──────────────▼───────────────┐
                     │  Node.js 网关代理 (:8081)     │
                     │  HTTP JSON ↔ gRPC 转码        │
                     │  六码错误映射 · 断开传播       │
                     └──────────────┬───────────────┘
                                    │ gRPC / Protobuf
                     ┌──────────────▼───────────────┐
                     │  gRPC Server (:50051)         │
                     │  9 个 Service · 41 个 RPC     │
                     │  认证拦截器 · 成本拦截器       │
                     └────┬─────────┬─────────┬─────┘
                          │         │         │
          ┌───────────────┤         │         ├──────────────┐
          │               │         │         │              │
  ┌───────▼──────┐ ┌──────▼─────┐ ┌─▼───────────┐ ┌─────────▼──────┐
  │ AgentRouter  │ │ Orchestrator│ │ A2A Adapter │ │ Durable 管线   │
  │ 四级路由+熔断 │ │ DAG 编排    │ │ Proto↔JSON  │ │ 预算·会话·追踪 │
  └───────┬──────┘ └──────┬─────┘ └─┬───────────┘ └─────────┬──────┘
          │               │         │                       │
          └───────────────┼─────────┘                       │
                          │                                 │
             ┌────────────▼────────────┐       ┌────────────▼──────────┐
             │  Redis (:6379)          │       │  PostgreSQL (:5432)   │
             │  缓存·心跳·限流·短锁     │       │  持久事实源            │
             └────────────┬────────────┘       │  V001–V015 只追加迁移  │
                          │                    └───────────────────────┘
             ┌────────────▼────────────┐
             │  外部 Agent              │
             │  A2A HTTP / JSON-RPC    │
             │  Orchestrator (:5000)   │
             │  示例 Agent (:5100)      │
             └─────────────────────────┘
```

**数据流**：浏览器 → Nginx :8080（生产）→ Node JSON 代理 :8081 → gRPC Server :50051 → A2A 适配器 → Orchestrator :5000 → 各 Agent。

**分层职责**：

| 层 | 组件 | 职责 |
| --- | --- | --- |
| 前端层 | Vue 3 + Pinia + TypeScript SPA | 对话、DAG 可视化、Agent 拓扑、Token/成本仪表盘 |
| 网关层 | Node.js JSON-gRPC 代理 | 协议转码、SSE 流式、错误语义映射、断开传播 |
| 服务层 | C++20 gRPC Server | 认证鉴权、查询管线、DAG 编排、智能路由、A2A 通信 |
| 数据层 | PostgreSQL 16 + Redis 7 | 持久事实源 + 运行时缓存/心跳/限流 |

## 快速开始

### 方式一：Docker Compose 一键启动（推荐）

仓库根目录执行：

```bash
docker compose up --build
```

将启动 5 个服务：PostgreSQL、Redis、RPC 服务端、Node 代理、Nginx 前端。RPC 服务端启动时自动执行 `db/migrations` 迁移，无需单独的迁移服务。`.env` 中配置的 `LLM_API_KEY` / `LLM_MODEL` / `NEXUSAI_ADMIN_USERNAME` 等会自动透传给 rpc-server 容器（缺省时服务照常启动，LLM 相关能力优雅降级）。服务之间通过 Compose DNS 互通（代理以 `rpc-server:50051` 连接后端），无需配置主机地址。

启动完成后访问 **<http://127.0.0.1:8080>** 即可使用（生产模式前端由 Nginx 托管，端口为 8080 而非 5173）。PostgreSQL 数据持久化在已加入 .gitignore 的 `./.nexusai-data/postgres` 挂载卷中。

> 排障：若 rpc-server 容器退出并报 `migration checksum mismatch`，说明该数据卷是在迁移文件的换行符（CRLF/LF）或内容与当前检出不一致时初始化的旧卷——本地开发数据可丢弃，删除 `./.nexusai-data/postgres` 后重新执行一键启动即可（迁移以当前文件重新初始化）。仓库已通过 `.gitattributes` 将 `*.sql` 固定为 LF，避免再次出现换行符漂移。

受限网络环境下可通过构建参数指定 Debian apt 镜像（默认为空即官方源）：

```bash
APT_MIRROR_PREFIX=http://mirrors.aliyun.com/debian docker compose up --build
```

停止服务：

```bash
docker compose down
# 或
./run.sh stop
```

### 方式二：本地开发模式（Linux 原生，或 Windows 下的 WSL2）

C++ 后端的编译、测试与运行在 **Linux 原生环境或 Windows 下的 WSL2 Ubuntu** 中进行（WSL2 方式建议把仓库检出到 Linux 文件系统，不要放在 `/mnt/c` 下）；前端与网关代理运行在宿主机终端（Windows PowerShell 或任意 Linux shell）均可。

**1. 环境准备（Linux / WSL2 内）**

> 注：`scripts/` 为本地辅助脚本，已不入库（本机工作区保留）。新环境可手动安装依赖：gRPC/protobuf 开发包、libpqxx、hiredis、GTest、RapidCheck、curl 等（或按需自建 bootstrap 脚本），随后：

```bash
./run.sh setup               # 检测开发环境，缺依赖会明确列出
```

**2. 配置环境变量**

```bash
cp .env.example .env         # 填入 LLM_API_KEY 等实际值
```

**3. 编译与测试（Linux / WSL2 内）**

```bash
./run.sh build               # CMake + make 编译
./run.sh test                # 运行全部 36 套测试
```

**4. 启动后端（Linux / WSL2 内）**

```bash
./run.sh start-all           # 启动容器化后端栈（等价于 ./run.sh gateway）
```

**5. 启动网关代理（宿主机终端）**

```powershell
$env:GRPC_TARGET="localhost:50051"
node gateway/proxy/server.mjs    # 监听 :8081
```

**6. 启动前端（宿主机终端）**

```bash
cd frontend && npm ci        # 首次安装依赖
cd frontend && npm run dev   # Vite 开发服务器
```

开发模式下访问 **<http://localhost:5173>**，支持热更新。Vite 仅将浏览器 RPC 路径转发到本地 Node 代理 :8081。

> 注意：生产模式（docker compose）前端端口为 8080，开发模式（npm run dev）为 5173，两者不通用，访问前请确认启动方式。

**7. 注册 Agent（Linux / WSL2 内，可选）**

通过 `client/` 交互式 CLI 或自定义脚本调用 `RegisterAgent`（ADMIN 专属 RPC，登录用户须匹配 `NEXUSAI_ADMIN_USERNAME` 配置）。

### 环境要求

| 要求 | 版本 | 说明 |
| --- | --- | --- |
| 操作系统 | Linux（Ubuntu 20.04+）或 WSL2 | C++ 后端编译运行 |
| CMake | 3.20+ | 构建系统 |
| GCC | 10+ | 支持 C++20 |
| gRPC | 1.51.1+ | RPC 框架 |
| PostgreSQL | 16 | 持久事实源 |
| Redis | 7.0+ | 缓存 / 心跳 / 限流 |
| Node.js | 18+ | 前端 + 网关代理 |

### 服务端口

| 端口 | 服务 | 说明 |
| --- | --- | --- |
| 8080 | Nginx 前端 | 生产模式浏览器入口（docker compose） |
| 5173 | Vite 开发服务器 | 开发模式浏览器入口（npm run dev） |
| 8081 | Node.js 网关代理 | HTTP JSON ↔ gRPC |
| 50051 | gRPC Server | 主 RPC 服务 |
| 5000 | Orchestrator | A2A HTTP 示例编排 Agent |
| 5100 | 示例 Agent | A2A HTTP 端点 |
| 5432 | PostgreSQL | 容器内通过服务名访问 |
| 6379 | Redis | 容器内通过服务名访问 |

## 目录结构

```text
agent-communication-and-tool-selection-framework/
│
├── proto/                           # Protocol Buffer 定义（9 个 .proto）
│   ├── ai_query.proto               #   AI 查询 — Query / QueryStream / Status / Metrics
│   ├── agent_service.proto          #   Agent 通信 — 注册 / 心跳 / 注销 / 发现 / 消息
│   ├── agent_lifecycle.proto        #   Agent 生命周期 — 反馈 / 自主性 / 撤销
│   ├── orchestration.proto          #   编排 — ExecutePlan / Replay / Export
│   ├── observability.proto          #   可观测性 — Trace / CostReport
│   ├── sharing.proto                #   会话共享 — Share / Observe / Templates
│   ├── user_experience.proto        #   用户体验 — Intervention / Sandbox / Compare
│   ├── user.proto                   #   用户服务 — Login / Register
│   └── common.proto                 #   公共消息 — ServiceInfo / Status / 枚举
│
├── server/                          # gRPC 服务端 — 9 个 Service 实现与 durable 查询管线
├── orchestrator/                    # DAG 任务编排与智能路由
│   ├── task_planner                 #   DAG 规划器（Kahn 拓扑排序）
│   ├── task_executor                #   并行执行器（独立工作线程，超时可放弃）
│   ├── agent_router                 #   四级路由引擎
│   ├── result_aggregator            #   结果聚合器
│   ├── replay_service / export_service  # 回放与导出
│   └── feedback_aggregator          #   反馈聚合（驱动路由质量）
│
├── common/                          # 公共组件库
│   ├── query_domain_repository      #   查询域仓储（会话/消息/日志/追踪，owner 隔离）
│   ├── agent_runtime_repository     #   Agent 运行时事实仓储（注册表/反馈/调用指标）
│   ├── circuit_breaker              #   三态熔断器
│   ├── load_balancer                #   负载均衡（6 种策略）
│   ├── memory_service               #   三层记忆系统
│   ├── cost_tracker / trace_context #   成本追踪与链路追踪
│   └── background_scheduler         #   后台周期任务（span 刷盘/反馈聚合/指标聚合等）
│
├── a2a/                             # A2A 协议核心库（HTTP/JSON-RPC 2.0）
├── a2a_adapter/                     # gRPC ↔ A2A 协议适配层（同步/异步/流式/直连）
├── mcp/                             # MCP 客户端（STDIO + SSE，可选模块）
├── mcp_server_integrated/           # 集成 MCP Server（独立构建，6 个插件）
├── registry/                        # Agent 注册与发现
├── client/                          # gRPC CLI + Agent 注册 SDK
│
├── db/
│   └── migrations/                  # V001–V015 只追加迁移（权威 schema）
│
├── frontend/                        # Vue 3 + TypeScript + Vite
│   └── src/
│       ├── views/                   #   10 个功能视图
│       ├── components/              #   UI 组件
│       ├── services/grpc-client.ts  #   Fetch + ReadableStream 网关客户端
│       ├── stores/                  #   Pinia 状态管理（chat / auth / agents）
│       └── types/proto.ts           #   与 proto/ 字段级对齐的类型定义
│
├── gateway/proxy/                   # Node.js JSON-gRPC 网关代理 + 契约测试
├── tests/                           # C++ 测试（36 套，GTest + RapidCheck）
│   └── e2e/                         #   发布 E2E 脚本
├── examples/                        # Agent 接入示例（Python）
├── docs/                            # 项目文档
├── docker/                          # 容器构建文件
├── docker-compose.yml               # 一键容器栈（5 服务）
├── run.sh                           # 统一运行脚本
├── .env.example                     # 环境变量示例
└── CMakeLists.txt                   # 根 CMake（C++20）
```

## 前端

基于 Vue 3 + TypeScript + Vite 的 SPA，通过 Node.js 网关代理完成 gRPC → SSE 流式对接，7 种 SSE 事件类型（`partial` / `status` / `complete` / `error` / `plan` / `subtask_start` / `subtask_complete`）支持逐字渲染、DAG 进度更新与活动流推送。

### 功能视图

| 视图 | 路由 | 说明 |
| --- | --- | --- |
| 对话 | `/` | 自然语言提问，SSE 流式回答，Mermaid 实时渲染 DAG 执行计划 |
| Agent 拓扑 | `/topology` | ECharts 力导向图展示 Agent 关系网络与健康状态 |
| 仪表板 | `/dashboard` | Token 消耗趋势、Agent 调用排行、成本分布 |
| 监控 | `/monitor` | 系统健康度、延迟分布、链路追踪信息 |
| 管理后台 | `/admin` | Agent 健康、预算配置、查询重放（ADMIN 角色门控） |
| 沙箱 | `/sandbox` | Agent 隔离试运行，真实后端闭环 |
| 对比 | `/compare` | 同一问题多 Agent 并行对比 |
| 分享 | `/share/:id` | 会话分享（TTL + 撤销），真实后端闭环 |
| 模板市场 | `/templates` | 模板保存与应用，真实后端闭环 |
| 登录 | `/login` | 注册登录，Token 持久化与过期自动登出 |

### 前端技术选型

| 技术 | 用途 |
| --- | --- |
| Vue 3 + TypeScript | UI 框架，手写与 proto 字段级对齐的类型映射 |
| Pinia | 状态管理（chat / auth / agents 三个 Store） |
| ECharts | 拓扑力导向图与仪表盘多图表联动 |
| Mermaid.js | ExecutionPlan JSON 实时渲染为 DAG 流程图 |
| Tailwind CSS | 原子化样式 |

## 测试与验证

### C++ 测试（Linux / WSL2 内，36 套）

GTest 集成测试与 RapidCheck 属性测试相结合，覆盖 durable 查询管线、查询域仓储契约、预算仓储契约、工作流控制契约、Agent 运行时仓储契约、路由属性、熔断器、任务状态机等。涉及 PostgreSQL 的用例连接真实数据库执行，缺失环境变量时按约定 SKIP 而非伪造通过。

```bash
./run.sh test                       # 全部测试
cd build && ctest --output-on-failure   # 等价方式
```

### 网关契约测试（116 例）

位于 `gateway/proxy/test/`，覆盖平台契约、gRPC→HTTP 错误映射运行时契约、proto 与前端类型定义的字段级防漂移契约、前端静态守卫：

```bash
cd gateway/proxy && npm test
```

### 发布 E2E（真实进程 + 真实数据库）

`tests/e2e/e2e_pr_g_release.py` 启动编译产物 rpc_server 进程，连接真实 Docker PostgreSQL/Redis 与真实 HTTP A2A Agent，6 个场景共 25 个断言全部通过 psql 直查 PostgreSQL 验证，不做任何 mock：

```bash
# Linux/WSL2 仓库根目录（前置：./run.sh build、compose 的 postgres/redis 已发布宿主端口、grpcurl、psql）
python3 tests/e2e/e2e_pr_g_release.py
```

覆盖场景：注册登录 → QueryStream 恰好一次 complete → 查询日志/追踪/消息/台账归属调用者；客户端中止 → cancelled 落库；预算拒绝 → RESOURCE_EXHAUSTED + rejected；跨 owner 访问 trace/反馈/导出/查询状态全部拒绝；反馈驱动路由质量变化；分享 TTL 过期与撤销拒绝。前置条件缺失时明确 SKIP。

### 前端类型检查与构建

```bash
cd frontend && npm run typecheck
cd frontend && npm run build
```

## 环境变量

复制 `.env.example` 为 `.env` 并填入实际值，`run.sh` 与服务启动时自动加载。关键项：

| 变量 | 用途 | 默认值 |
| --- | --- | --- |
| `LLM_API_KEY` | LLM API 密钥（多 Agent 编排、意图路由、DAG 规划依赖，未设置时相关能力优雅降级） | 必填 |
| `LLM_MODEL` | 模型名称 | `deepseek-v4-flash` |
| `LLM_API_URL` | LLM completions 端点（OpenAI 兼容） | `https://api.deepseek.com/v1/chat/completions` |
| `EMBEDDING_API_URL` / `EMBEDDING_MODEL` | 向量化端点与模型 | DeepSeek |
| `NEXUSAI_POSTGRES_HOST` / `PORT` / `DATABASE` / `USER` / `PASSWORD` | PostgreSQL 连接（代码仅读取这组变量名，Compose 将其映射到服务 DNS） | `127.0.0.1:5432/nexusai` |
| `POSTGRES_DB` / `POSTGRES_USER` / `POSTGRES_PASSWORD` | Compose postgres 容器初始化参数 | `nexusai` / `nexusai` / `nexusai-dev-password`（仅本地） |
| `REDIS_HOST` / `REDIS_PORT` | Redis 地址 | `127.0.0.1:6379` |
| `NEXUSAI_ADMIN_USERNAME` | 匹配该用户名的账号注册时获得 ADMIN 角色；留空则无人可执行 Agent 管理 RPC | 空 |
| `NEXUSAI_BUDGET_GLOBAL_TOKENS` | 全局 Token 预算，0 表示不限制 | `0` |
| `NEXUSAI_BUDGET_USER_DAILY_TOKENS` | 用户日 Token 预算 | `200000` |
| `NEXUSAI_BUDGET_USER_MONTHLY_TOKENS` | 用户月 Token 预算 | `4000000` |
| `NEXUSAI_BUDGET_SESSION_TOKENS` | 会话 Token 预算 | `100000` |
| `RPC_SERVER_PORT` / `PROXY_PORT` / `GRPC_TARGET` | 服务端口与代理目标 | `50051` / `8081` / `localhost:50051` |

预算按估算 Token 数预扣（64 + 问题字符数 ÷ 4），超限的查询返回 RESOURCE_EXHAUSTED 并在 `query_logs` 落库 status=rejected。

## Agent 接入

将外部 Agent 接入框架的核心步骤（详见 [agent-integration-guide.md](agent-integration-guide.md)）：

1. **实现 A2A AgentCard**：声明 Agent 的技能、端点、版本等元数据。
2. **实现 A2A 接口**：支持 `message/send`（同步）或 `message/stream`（SSE 流式）。
3. **启动并注册**：运行 Agent 服务后，由具备 ADMIN 角色的用户调用 `RegisterAgent` RPC 注册到 gRPC Server（`client/` CLI 可用）。

Python 示例 Agent 见 [examples/](examples/) 目录（echo / math / translator / orchestrator）。

## 能力边界说明

为保证文档与实现一致，以下边界如实说明：

- `RealTimeCommunication`（双向流）返回 UNIMPLEMENTED，Agent 间消息请使用 SendMessage / ReceiveMessage / BroadcastMessage / ListenMessages。
- `GetQueryStatus` 直接读取 PostgreSQL 中的持久化状态（按 owner 隔离），不存在内存任务缓存。
- etcd 注册中心不在本地支持边界内，仅当显式配置 `RPC_REGISTRY_ADDRESS=etcd://` 时可达。
- MCP/RAG 为可选模块，默认关闭，需 `-DENABLE_MCP=ON` 构建。
- Cron 定时调度与 Canary 灰度已按本地目标边界移除。
- 用户画像链路已闭环：LLM 提取后写入 PostgreSQL（V004 `user_profiles`，成功即失效缓存投影）并在查询时读回注入；冲突消歧的消费者（画像重算）为规划中方向。

## 许可证

MIT License
