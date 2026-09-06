# AGENTS.md

This file provides guidance to AI coding agents (ZCode / Lingma / Claude Code) when working with code in this repository.

## 环境约束（最重要）

- **C++ 后端的编译、测试、服务启动必须在 WSL2 Ubuntu 内运行**，且仓库须检出到 Linux 文件系统（不能在 `/mnt/c` 下）。前端和网关代理在 Windows 原生运行。
- 首次构建前先运行 `./scripts/bootstrap-wsl.sh`。它会在 Ubuntu 26.04 上用 SHA-256 校验的 user-prefix 安装 pin 版 libpqxx 8.0.1（Ubuntu 24.04 用系统包）；`run.sh build` 会自动发现该前缀，无需手动 export。**注意：`scripts/` 目录已从 git 移除（2026-09-04，属本地辅助脚本不入库），仅存在于本机工作区——本文档中的 `scripts/*.sh` 命令在本机可用，新 clone 需自行重建这些辅助脚本。**
- 普通的一次 CMake 配置不会下载任何依赖。
- Shell 为 PowerShell 时不支持 `&&`，用 `;` 分隔。

## 常用命令

```bash
# 构建 / 测试 / 运行（WSL2 内）
./run.sh build                # cmake + make（输出到 build/）
./run.sh test                 # 全部 37 套测试（ctest --output-on-failure --timeout 30）
./run.sh test -R <name>       # 运行单个测试（附加参数透传给 ctest）
cd build && ctest -N          # 列出全部测试名
cd build && ctest --output-on-failure -R test_agent_router_properties   # 单测示例

# 服务（WSL2 内）
./run.sh start-all            # 一键启动容器化后端栈（= ./run.sh gateway）
./run.sh stop                 # 停止本地进程 + Docker 容器
./run.sh start                # 仅启动 rpc_server 二进制（后台，写 logs/ 与 pids/）
./run.sh start-mock-agent     # Mock Agent :5100（E2E 验证用）
./run.sh start-orchestrator   # A2A Orchestrator :5000
./run.sh setup                # 环境依赖检测（缺依赖会明确列出）

# 前端（Windows 原生）
cd frontend && npm ci && npm run dev    # Vite :5173，代理到 Node proxy :8081
cd frontend && npm run typecheck        # lint 即 typecheck（vue-tsc）
cd frontend && npm run build

# 网关契约测试（116 例，Windows 原生）
cd gateway/proxy && npm test

# E2E
./run.sh verify                       # 8 批 32 个场景
python3 tests/e2e/e2e_pr_g_release.py # 发布 E2E（WSL）：真实 rpc_server + 真实 Docker PG/Redis，断言全部经 psql 直查，缺前置时明确 SKIP

# WSL 直调辅助脚本（Windows 侧用 wsl -e bash scripts/<name>，内含 LD_LIBRARY_PATH 与 .env 注入）
scripts/run_full_gate.sh [ctest args]              # 全量 ctest 门禁（--timeout 60）
CTEST_REGEX='A|B' scripts/run_ctest_group.sh [..]  # 任意 ctest 正则分组
scripts/run_test_group.sh                          # 基线回归组：RedisServices|DurableQueryPipeline|WorkflowControlContract
scripts/build_mcp.sh [make targets]                # 备用构建树 build-mcp/（-DENABLE_MCP=ON）
CTEST_REGEX='A|B' scripts/run_ctest_group_mcp.sh   # 对 build-mcp/ 跑 ctest 分组
```

Docker 一键栈（仓库根目录）：`docker compose up --build` 启动 PostgreSQL、Redis、rpc-server、Node 代理、Nginx 前端，浏览器入口 `http://127.0.0.1:8080`（生产端口是 8080，不是开发模式的 5173）。

## 红线与测试约定

- 修改核心源码（如 `ai_query_service.cpp`、`agent_router.cpp`、`multi_agent_handler.cpp`）前先跑 `scripts/redline_check.sh` —— 静态契约测试会以文本断言锁定源码内容，误改锁定文本会导致测试失败。
- 新测试用例必须追加到既有测试文件（如 `test_redis_services.cpp`），**严禁修改 `tests/CMakeLists.txt`**。
- 需要 Redis 在 `localhost:6379` 运行的测试：auth / memory / agent-communication 相关。PG 相关用例连真实数据库，缺环境变量时按约定 SKIP 而不是伪造通过。PG/Redis 可用 `docker compose up -d postgres redis` 启动（`docker-compose.override.yml` 发布 5432/6379 宿主端口供 WSL ctest 使用）；全量门禁用 `scripts/run_full_gate.sh`。
- 前端类型 `frontend/src/types/proto.ts` 与 `proto/` 字段级对齐，改动 proto 后必须同步，网关契约测试里有防漂移断言。
- `db/migrations/VNNN__name.sql` 是权威 schema（只追加）；RPC 服务端启动时自行执行迁移（`NEXUSAI_MIGRATIONS_DIR`），没有独立 migrate 服务；旧 `sql/` 参考 schema 已从仓库移除（2026-09-04，内容被 V001+ 迁移完全取代）。

## 架构总览

数据流：`Browser → Nginx :8080（生产）/ Vite :5173（开发）→ Node JSON 代理 :8081 → gRPC Server :50051 → A2AAdapter → Orchestrator :5000 → 各 Agent`

```text
proto/          → 9 个 proto，9 个 gRPC Service，41 个 RPC
server/         → gRPC Server :50051（AuthInterceptor、CostInterceptor、durable 查询管线）
orchestrator/   → AgentRouter（四级路由）+ TaskPlanner/TaskExecutor（DAG）+ ResultAggregator + replay/export/feedback
a2a/            → 纯 A2A 协议库（HTTP/JSON-RPC 2.0，AgentCard、message/send、message/stream SSE）
a2a_adapter/    → Protobuf ↔ A2A JSON-RPC 桥（sync / async / streaming / direct 四种模式）
common/         → Logger、CircuitBreaker、LoadBalancer(6 种)、RedisClient、MemoryService、
                  query_domain_repository / agent_runtime_repository、TraceContext、CostTracker、BackgroundScheduler
registry/       → ServiceRegistry（Agent 发现与健康）
mcp/            → 可选 MCPClient（默认关闭，需 -DENABLE_MCP=ON）
mcp_server_integrated/ → 独立构建的集成 MCP Server（6 个内置插件）
client/         → 交互式 gRPC CLI + Agent 注册 SDK
gateway/proxy/  → Node JSON↔gRPC 转码代理（唯一的浏览器网关）：错误码映射（401/403/404/429/499/409）、
                  SSE in-band error、浏览器断开传播为 stream.cancel()
frontend/       → Vue 3 + TS + Pinia SPA，10 个视图（Chat/Topology/Dashboard/Monitor/Admin/Sandbox/Compare/Share/Templates/Login）
db/             → PostgreSQL 迁移 V001–V015（PostgreSQL 是唯一持久事实源，含记忆域/画像/健康快照；Redis 仅投影/缓存/心跳/限流/短锁）
```

## 关键抽象

- **Durable Query Pipeline**：Query/QueryStream 六步固定顺序（认证取 owner → 确保会话 → 登记 running → 预算预留 → 组装 SystemContext → compare-exchange 单次终结），任何一步失败落库终态。**owner 一律来自认证上下文，请求体 `user_id` 被无条件覆盖**。幂等由确定性主键保证（`msg-user-<request_id>`、`usage-<request_id>`）；同 `request_id` 重试只产生一条预算预留与台账；客户端中止 → `cancelled`，预算不足 → `rejected` + RESOURCE_EXHAUSTED。
- **AgentRouter 四级路由**：Embedding（约 80% 查询，<100ms）→ LLM → IDF 关键词 → Fallback，线程安全，用户反馈经 Beta 平滑聚合成路由质量系数驱动加权选择。
- **TaskPlanner/TaskExecutor**：LLM 分解为 DAG → Kahn 拓扑分层 → 同层 `std::async` 并行，前置结果注入下游；全局超时；委派深度限制 5 层。
- **MemoryService 三层记忆**：按 Agent 隔离的对话历史 + 用户长期记忆（Hash）+ 跨 Agent LLM 摘要，每次查询前注入 SystemContext。
- **CircuitBreaker**：CLOSED→OPEN→HALF_OPEN 三态，按 Agent 粒度。
- **BackgroundScheduler**：协调者 + Worker 池的周期任务（span 刷盘、反馈聚合、指标聚合、画像提取、健康评估、缓存清理）；Cron/Canary 已移除。

## 代码约定

- 头文件 `include/agent_rpc/<module>/<name>.h`，源码 `src/<module>/<name>.cpp`。
- 命名空间：`agent_rpc::a2a_adapter`、`agent_rpc::orchestrator`、`agent_rpc::mcp`、`agent_rpc::common`；proto 包 `agent_communication`。
- C++20（根 CMakeLists.txt `CMAKE_CXX_STANDARD 20`），CMake 3.20+，依赖经 pkg-config / find_package（gRPC、protobuf、libpqxx、jsoncpp、hiredis、GTest、RapidCheck）。
- 测试：GTest 集成 + RapidCheck 属性测试（属性测试命名 `test_*_properties.cpp`）。
- `ai_interface/` 模块已在根 CMakeLists.txt 中注释掉。
- **a2a/libcurl 红线**：`CurlHandle`/`CurlSList`（http_client.cpp）禁止隐式转换后传入 `curl_easy_setopt` 等变参函数——变参传参会按位拷贝对象并在调用表达式末析构副本，导致句柄/slist 在传输期间被释放（heap-use-after-free）。必须用 `.get()` 取裸指针。
- LLM_MODEL 代码兜底值统一为 `deepseek-v4-flash`（与 .env.example 一致）；`EMBEDDING_MODEL` 兜底 `deepseek-v4-pro`。

## 关键文档（改敏感区前先读）

`docs/guides/` 下的权威指南：`durable-query-pipeline-guide.md`（改 server/ 查询管线前必读）、`workflow-control-guide.md`、`a2a-protocol.md`、`mcp-plugin-development.md`、`rag-mcp-guide.md`、`sharing-and-assets-guide.md`、`startup-guide.md`、`deployment.md`。

## 外部依赖与环境变量

- `.env`（从 `.env.example` 复制）由 `run.sh` 和 `env_loader` 自动加载。
- `LLM_API_KEY`（必填，OpenAI 兼容端点，缺省 DeepSeek）；`LLM_MODEL`、`LLM_API_URL`、`EMBEDDING_API_URL`/`EMBEDDING_MODEL`。缺 key 时路由第 2 级、DAG 规划、RAG embedding 优雅降级。
- PostgreSQL 仅读 `NEXUSAI_POSTGRES_HOST/PORT/DATABASE/USER/PASSWORD`（无 `PG_URL`）；Compose 用服务 DNS 互联，代理容器内必须 `GRPC_TARGET=rpc-server:50051`。
- 预算四层：`NEXUSAI_BUDGET_GLOBAL_TOKENS`(0=不限)、`_USER_DAILY_TOKENS`(200000)、`_USER_MONTHLY_TOKENS`(4000000)、`_SESSION_TOKENS`(100000)；估算 token = 64 + 问题字节数/4。
- `NEXUSAI_ADMIN_USERNAME` 匹配的用户注册时获得 ADMIN 角色，是 RegisterAgent 等管理 RPC 的门槛；留空则无人可管理 Agent。

## 能力边界（不要臆造功能）

- `RealTimeCommunication` 返回 UNIMPLEMENTED（用 SendMessage / ReceiveMessage / BroadcastMessage / ListenMessages）。
- `GetQueryStatus` 直接读 PG（无内存缓存）；etcd 注册仅当显式 `RPC_REGISTRY_ADDRESS=etcd://`；MCP 默认关闭；Cron/Canary 已移除。
- ProfileSummarizer::processPending() 有真实 Redis+LLM 逻辑，但画像只写 Redis 不写 PG。

## Redis 键规范（P23 存储分层治理，批次七拍板）

键分三类（memory_service.h 有权威标注，新增键必须归类）：

- **事实源→投影（V015 已收口）**：`nexusai:memory:<uid>`（Tier-2 hints）、`nexusai:summary:*`（Tier-3 摘要）、`user_profile:<uid>`（画像）已由 V015/V004 迁入 PG（批次八 B5/C1），这些 Redis 键降级为 cache-only 投影——写入路径为"PG 成功 → DEL Redis 键"，读取为"PG miss → Redis → 回填 insert-if-absent"。
- **cache-only**（PG 为事实源的投影，丢失可重建）：`nexusai:conv:*`、`auth:session:<token_hash>`（拦截器层会话缓存）。
- **transient**（可丢、无业务后果）：限流计数、分布式短锁、预算实时计数面、`nexusai:last_agent:*`、`profile:queued:<uid>`/`profile:pending`（画像提取调度）、`trace:spans:<id>`（span 批量冗余，PG 兜底读取）。

**约定：不再新增事实源键**；新增键必须按上述三类标注归类，写入路径首选“PG 成功 → 失效 Redis 键”的投影语义。
