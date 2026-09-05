# NexusAI — 从零构建的多 Agent 协作平台

> **C++20 · gRPC · A2A 协议 · Vue 3 · Redis · MCP · RAG**

### 🎯 30 秒电梯演讲

> 市面上所有 AI Agent 框架都在做同一件事——让 LLM 决定调用哪个函数。但这有三个致命问题：**每次路由都要调一次 LLM**（600ms+ 延迟，$15/天的 API 费用），**Agent 之间只能在同一 Python 进程内通信**（无法跨语言、无法独立部署、无法跨组织协作），**所有工具一股脑塞给 LLM**（上下文溢出、模型幻觉）。
>
> 我花 48 次迭代从零构建了 NexusAI——一个真正在网络协议层面让 Agent 互相通信的 C++20 分布式平台。核心创新是四层渐进式路由（Embedding 向量匹配覆盖 80% 查询，P50 延迟从 600ms 降到 100ms），A2A 标准化网络协议（Agent 可跨语言独立部署，类似 SMTP 让不同邮件服务商互通），RAG 向量检索做工具智能选择（100+ 工具压缩为 Top-5），DAG 拓扑并行编排（多 Agent 协作 12s→4s）。全链路自研——从 Protobuf 协议定义到 Vue 3 前端 UI，从三态熔断器到三层记忆系统，从 Token 成本追踪到以 PostgreSQL 为事实源的 Durable Query Pipeline，21 项优化、37 套 CTest 测试全绿。
>
> **一句话：** 别人在用 LangChain 搭积木，我在给积木设计新的连接标准。

### 📊 面试官一眼看懂

| 你关心的 | 我的答案 | 量化证据 |
|----------|---------|---------|
| 技术深度？ | C++20 自研四层路由 + A2A 协议库 + DAG 编排引擎 | 10 个 CMake 模块，41 个 RPC，全链路自研 |
| 系统设计？ | 类 Spring Cloud 的 Agent 基础设施（注册/发现/路由/熔断/负载均衡） | 6 种 LB 策略，三态熔断器，6 个后台调度任务 |
| AI 工程化？ | RAG 工具检索 + 语义缓存 + 上下文压缩 + 三层记忆 | 路由成本 80%↓，工具上下文 90%↓，长对话 Token 87%↓ |
| 产品质量？ | 37 套 CTest + 116 例网关契约 + 真实 PG E2E 25 断言 | 21 项优化全覆盖，持久化链路全真实 |
| 前端能力？ | Vue 3 + TS + Vite SPA，10 个视图，SSE 流式渲染 | 独立完成，从 Proto 类型到 UI 组件，全部视图接入真实 RPC |
| 工程素养？ | 轻量化设计（自建替代 K8s/Prometheus/Jaeger），48 次增量提交 | 零重型外部依赖，编译即运行 |

---

## 目录

1. [项目概述](#1-项目概述)
2. [解决的痛点（12 个，覆盖全部 P0-P3 + U0-U6）](#2-解决的痛点--为什么需要这个项目)
3. [技术架构总览](#3-技术架构总览)
4. [核心模块详解](#4-核心模块详解)
5. [核心技术亮点](#5-核心技术亮点)
6. [8 批次优化工程详解](#6-8-批次优化工程详解)
7. [技术难点与解决方案](#7-技术难点与解决方案)
8. [前端架构](#8-前端架构)
9. [测试与质量保障体系](#9-测试与质量保障体系)
10. [部署与运维](#10-部署与运维)
11. [求职竞争力分析](#11-求职竞争力分析)
12. [项目展望](#12-项目展望)

---

## 1. 项目概述

### 1.1 一句话描述

NexusAI 是一个**企业级多 Agent 通信与智能工具选择框架**——让不同语言、不同能力、不同部署位置的 AI Agent 能像微服务一样被自动发现、智能路由、编排协作。它不是 LangChain 的"Python 函数调用"模式，而是真正在**网络协议层面**让 Agent 互相通信的分布式系统。

### 1.2 关键数据

| 维度 | 指标 |
|------|------|
| 代码语言 | C++20（后端核心）+ TypeScript（前端）+ Python（辅助服务） |
| 通信协议 | gRPC/Protobuf (内部) + A2A JSON-RPC (Agent 间) |
| 编译模块 | 10 个 C++ CMake 模块 + Vue 3 前端 + Python 辅助服务 |
| Proto 文件 | 9 个功能域拆分（含 9 个 gRPC Service，41 个 RPC 方法） |
| 测试套件 | 37 套 CTest（GTest 集成 + RapidCheck 属性测试 + 真实 PG 契约测试）+ 116 例网关契约测试 |
| E2E 场景 | verify/ 8 批次自动化场景 + Release E2E 25 个真实断言（tests/e2e/e2e_pr_g_release.py） |
| 前端页面 | 10 个 Vue 3 视图（ChatView, AgentTopology, Dashboard, Monitor, AdminView, AgentSandbox, CompareView, ShareView, TemplateMarket, LoginView） |
| 迭代历史 | 48+ 次增量迭代 + 7-PR PostgreSQL 持久化大优化，从基础 RPC 框架演进到全功能平台 |
| 数据库迁移 | db/migrations V001–V015（只追加），rpc-server 启动时自动执行，失败即中止启动 |
| 服务端口 | Docker Compose 五服务（postgres/redis/rpc-server/proxy/frontend，前端生产端口 :8080）；本地开发 gRPC :50051、Orchestrator :5000、Mock Agent :5100、Proxy :8081 |

### 1.3 技术栈全景

```text
┌─────────────────────────────────────────────────────────────────────┐
│  前端层    Vue 3 + TypeScript + Vite + Pinia                         │
│           ChatView | AgentTopology | Dashboard | Monitor          │
│           AdminView | AgentSandbox | CompareView                 │
│           ShareView | TemplateMarket | LoginView                 │
├─────────────────────────────────────────────────────────────────────┤
│  网关层    Node.js gRPC Proxy (主力，gRPC→SSE 转换，六码 HTTP 错误映射) │
│           Nginx + Envoy (Docker 部署可选，gRPC-Web 协议转换)        │
├─────────────────────────────────────────────────────────────────────┤
│  服务层    C++ gRPC Server :50051                                    │
│           AIQueryService | AgentCommunicationService                 │
│           AgentLifecycleService | OrchestrationService               │
│           ObservabilityService (已实现: GetTraceDetail + GetCostReport)│
│           UserExperienceService                                       │
│           SharingService | HealthService | UserService (Auth)        │
│           AuthInterceptor | CostInterceptor (Token 成本追踪)         │
├─────────────────────────────────────────────────────────────────────┤
│  适配层    C++ A2AAdapter — gRPC/Protobuf ↔ A2A JSON-RPC 双向转换   │
│           RequestAdapter | ResponseAdapter | ErrorMapper             │
├─────────────────────────────────────────────────────────────────────┤
│  编排层    C++ AgentRouter (四层路由) + TaskPlanner (DAG 分解)       │
│           + TaskExecutor (拓扑并行执行) + ResultAggregator           │
│           Python Orchestrator :5000 (A2A 协议路由代理)               │
├─────────────────────────────────────────────────────────────────────┤
│  MCP 层    C++ MCPClient (STDIO + SSE 双传输)                       │
│           RAG-MCP: EmbeddingService + VectorIndex + ToolRetriever    │
│           SemanticCacheIndex (语义缓存)                              │
├─────────────────────────────────────────────────────────────────────┤
│  基础层    C++ Common: Logger | RedisClient | MemoryService          │
│           CircuitBreaker | LoadBalancer (6 种策略)                   │
│           BackgroundScheduler (统一后台任务调度)                      │
│           EnvLoader | TraceContext | TokenCostTracker                │
├─────────────────────────────────────────────────────────────────────┤
│  存储层    PostgreSQL 16 — 持久事实源：查询/消息/trace/成本/预算/    │
│           注册表/反馈/分享/工作流；db/migrations V001–V015 只追加，  │
│           rpc-server 启动时自动迁移（失败即中止）                    │
│           Redis 7 — 仅缓存/心跳存活/限流/短锁，可随时清空不丢数据    │
├─────────────────────────────────────────────────────────────────────┤
│  Agent 层  任意语言 HTTP Server 实现 A2A JSON-RPC 接口即可接入       │
│           Mock Agent (Python, :5100) | 未来: Math/Code/... Agent     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 2. 解决的痛点 — 为什么需要这个项目？

### 后端基础能力（P0-P3）

#### 痛点 1：每次请求都调 LLM 做意图分类 → 延迟高、成本高

**痛在哪：** 主流 Agent 框架（LangChain、CrewAI 等）在每个用户请求到来时都调用一次 LLM 来判断"该交给哪个 Agent"。单次调用 600-1200ms，消耗 ~500 tokens。按每天 10000 次查询算，仅路由环节就产生 1.6-3.3 小时的累积延迟和 $15/天的 API 费用。更关键的是——**这些延迟和费用没有任何用户价值**，它只是在"选择由谁处理"，而不是在"处理问题本身"。

**我的考量：** 如果把路由看作一个分类问题，那它不应该每次都需要 LLM。Agent 的技能是静态的（至少在短时间内），可以预先向量化。只有边界模糊的查询才需要 LLM 的语义理解能力。所以我设计了一个"从便宜到贵"的渐近管线——先用几乎免费的 Embedding 向量匹配，命中就直接返回；不确定时再升级到 LLM；LLM 不可用时降级到关键词索引；最后用健康 Agent 兜底。**关键设计决策是管线顺序——必须从便宜到贵，而不是反过来。**

**技术落地：**
- **Layer 1 — Embedding 高置信度匹配**：启动时将已注册 Agent 的所有 skill 描述预向量化（格式 `"skill_name: description"`，把名称和语义信息一起编码进 1024 维向量）。查询到来时实时向量化并计算余弦相似度，≥ 0.85 直接路由。命中即返回，不触发后续层。延迟 < 100ms（缓存命中 ~1ms），零 Token 消耗。
- **Layer 2 — LLM 动态意图分类**：仅在 Embedding 相似度 < 0.85 时触发。`buildDynamicIntentPrompt()` 从 Agent 注册表动态生成 Prompt——不硬编码 skill 列表，Agent 增删后 Prompt 自动更新。延迟 300-800ms。
- **Layer 3 — IDF 加权关键词索引**：LLM 不可用或超时时的降级路径。倒排索引 `keyword → [(skill, idf_weight)]`，IDF = 1/含此关键词的 skill 数（独有词权重高，通用词天然降权）。纯内存 O(1) 查找，延迟 < 1ms。
- **Layer 4 — 反馈驱动加权兜底**：前三层都未命中时，用 `selectWeightedByQuality()` 基于历史点赞率（质量系数 [0.5, 1.0]）加权随机选一个健康通用 Agent。

**量化结果：**

| 指标 | 纯 LLM 路由 | 四层渐进式 | 改善 |
|------|-----------|-----------|------|
| P50 延迟 | 600ms | < 100ms | **83%↓** |
| 日均 LLM 调用 | 10000 次 | ~2000 次 | **80%↓** |
| 日均路由 API 费用 | ~$15 | ~$3 | **80%↓** |
| Embedding 命中率 | N/A | ~80% | — |

#### 痛点 2：Agent 故障无保护 → 级联雪崩

**痛在哪：** 当某个 Agent 宕机或持续超时时，如果没有熔断保护，每个请求都会尝试连接 → 等待超时（30s）→ 失败 → 下一个请求再重复。5 个并发请求就是 150 秒的累积等待。更严重的是，路由层不知道这个 Agent 已经不可用，仍然把它作为候选，导致用户反复遇到失败。

**我的考量：** 熔断器是一个经典模式，但不能简单套用。Agent 场景的特殊性在于：(1) Agent 可能“半坏”——部分 skill 正常、部分超时；(2) 需要按调用类型分组（如 `a2a_orchestrator`、`direct_agent:{url}`、`rpc_client` 等）而非仅按 agent_id 做熔断；(3) 熔断后的回退不能是“报错给用户”，而是要自动切换到备选 Agent。

**技术落地：**
- 在 `a2a_adapter.cpp` 的 HTTP 调用前后各插入 5 行熔断检查代码——`allowRequest(agent_id)` → `execute()` → `recordSuccess()/recordFailure()`
- 三态状态机：CLOSED（正常）→ 连续 5 次失败 → OPEN（拒绝，60s）→ HALF_OPEN（探测）→ 成功 3 次 → CLOSED
- 熔断后的 fallback 不是报错，而是通过 `findFallbackAgent(skill, exclude_agent)` 找到同 skill 的下一个健康 Agent
- 配置参数通过 `circuit_breaker.h` 中的 `CircuitBreakerConfig` 结构体设置（`failure_threshold = 5`, `timeout = 60s`）

**量化结果：** Agent 宕机后首次请求即被熔断（而非等待 30s 超时），后续请求自动路由到备选 Agent，用户无感知。故障恢复时间从"手动下线 Agent"的分钟级降至"熔断器自动检测"的秒级。

#### 痛点 3：工具全量塞给 LLM → 上下文溢出、模型幻觉

**痛在哪：** Agent 有 100+ 个 MCP 工具时，全部 name + description + JSON Schema 传入 prompt 占用 4000+ tokens。模型上下文窗口有限（即使 128K 窗口，工具描述占太多也会挤压真正的对话空间）。更致命的是，工具太多时 LLM 容易"挑花眼"——选错工具或产生幻觉调用不存在的工具。

**我的考量：** 这是一个典型的"检索增强"场景——不需要让 LLM 看所有工具，只需要让它看最相关的。关键是向量化什么文本。我的实验结论是：`"tool_name: description"` 这种格式比单独向量化 name 或 description 效果更好——名称提供精确匹配信号，描述提供语义泛化能力。

**技术落地：**
- `EmbeddingService` 调用 OpenAI 兼容 API 生成 1024 维向量，支持批量向量化和指数退避重试（最多 3 次，1s/2s/4s）
- `VectorIndex` 内存索引，余弦相似度 Top-K 搜索 + 阈值过滤，线程安全
- `EmbeddingCache`：LRU 缓存（1000 条上限，1h TTL），对相同工具描述复用向量，减少 API 调用
- `ToolRetriever` 编排整个流程：查询向量化 → 搜索 → 返回 Top-5 工具 → 传给 LLM 选择

**量化结果：** 工具上下文从 4000+ tokens 降至 ~400 tokens（**90%↓**），检索延迟 < 50ms。当工具数从 10 增长到 100 时，LLM 工具选择准确率不下降。

#### 痛点 4：没有可观测性 → 出问题只能盲猜

**痛在哪：** 一个查询从用户输入到 Agent 返回结果，经过 5 个节点（前端 → 代理 → gRPC → Adapter → Agent）。当用户说"回答太慢了"或"返回为空"时，你根本不知道是哪个环节出了问题。没有 trace、没有 span、没有 token 成本追踪——只能靠加日志 + 重新部署来排查。

**我的考量：** 企业级方案（Jaeger + OpenTelemetry + Prometheus）太重了——对个人项目引入这些等于引入 3-4 个新服务、新语言、新配置。我需要一个"够用但不重"的方案。thread_local 是关键洞察——gRPC 每个请求分配独立线程，TraceContext 用 thread_local 存储，不需要传参、不需要改函数签名、零侵入。

**技术落地：**
- **TraceContext**（`thread_local`）：任意调用栈深度的代码通过 `TraceContext::current()` 直接获取 trace_id，零侵入——不改任何函数签名
- **TraceSpan**：router → planner → agent_call → executor → aggregator 每个阶段的 start/end/duration/status 经 Redis 批量缓冲后刷入 PG `traces` 表（trace_payload）
- **TokenCostTracker**：每个 LLM 调用的 prompt_tokens / completion_tokens / cost_usd 记录到 PG `token_usage_ledger` 表，按 user/agent/component 多维度聚合
- **GetTraceDetail RPC**：前端输入 trace_id → 返回完整调用链时间线（“路由 12ms → 规划 340ms → Echo Agent 856ms”）
- **GetCostReport RPC**：按 user/agent/component 多维度查询 Token 消耗与成本报表
- **BackgroundScheduler** 每 100ms 批量刷 span 到 PG，避免每条 span 单独写库

**量化结果：** 全链路排查时间从"加日志+重新部署+复现"的 20 分钟降至"输入 trace_id 点查询"的 5 秒。零外部依赖——仅用 PG + thread_local。

#### 痛点 5：Agent 重复查询浪费 Token → 成本无管控

**痛在哪：** 用户经常用不同措辞问同一个问题（"帮我写个排序" vs "写个排序算法"）。传统方案会重新调用 Agent → 重新调 LLM → 重新消耗 Token。更隐蔽的浪费是长对话场景——每轮都把完整历史发给 LLM，第 20 轮对话的 prompt 可能有 15000+ tokens 其中 80% 是历史。

**我的考量：** 两个层面解决——(1) 语义缓存拦截重复查询；(2) 上下文压缩防止长对话膨胀。语义缓存的关键是相似度阈值：设太高（0.95）命中率低没效果，设太低（0.80）可能返回不相关结果。我选了 0.92——在 1024 维空间中这意味"语义高度一致但措辞可以不同"。上下文压缩的触发时机也很关键——压缩太早丢失细节，压缩太晚没节省效果。我选 70% 窗口占用作为触发点——既保证历史在前几轮不被压缩（保留细节），又在长对话中及时介入。

**技术落地：**
- **SemanticCacheIndex**：复用 VectorIndex 基础设施。查询向量化 → 与缓存中历史查询做余弦相似度 → ≥ 0.92 直接返回缓存响应（含 agent_id + response + hit_count）。`cacheable` 字段让 Agent 声明"我的响应是否可被缓存"。
- **ContextCompressor**：每轮构建 Agent 请求前估算 prompt token 数量。当对话历史占比 > 70% 模型窗口时，调用 LLM 将历史压缩为 2-3 句话的"对话脉络摘要"，替换原始历史。后续轮次在摘要基础上追加新消息。
- **Token 预算（PostgreSQL 持久化）**：全局 → 用户日 → 用户月 → 会话四个配额层级，在 PG 事务中原子 reserve（advisory lock + counter upsert），预留以 request_id 幂等；超额时先把 query_logs 落 `rejected` 终态再返回 RESOURCE_EXHAUSTED（网关映射 429）。配额经 `NEXUSAI_BUDGET_*` 环境变量配置，详见 [Durable Query Pipeline 指南](guides/durable-query-pipeline-guide.md)。

**量化结果：** 语义缓存命中率约 15-20%（取决于查询模式），每次命中节省 100% Token。上下文压缩使第 20 轮 prompt 从 ~15000 tokens 降至 ~2000 tokens（**87%↓**），长对话延迟不再线性增长。

---

### 用户体验（U0-U6）

#### 痛点 6：用户看不到 Agent "在想什么" → 黑箱体验、信任缺失

**痛在哪（U3 活动流）：** 当用户发了一个复杂查询后，可能需要等待 10-30 秒。传统聊天界面只显示一个 loading 动画。用户不知道 Agent 在干什么——是卡死了？还是在思考？还是在调工具？这种不确定性是所有 AI 产品最致命的体验问题——用户等 10 秒后就会关掉页面。

**我的考量：** 实时活动流不是简单的"把日志展示出来"。需要设计事件粒度和视觉语言——太细（每个 HTTP 请求都显示）用户看不懂，太粗（只显示"处理中"）跟 loading 动画没区别。我选择了三个关键事件：💭 思考（LLM 调用）、🔧 工具调用（MCP tool call）、✅ 完成。同时需要干预点——当 Agent 要做高风险操作（写文件、高费用 LLM 调用）时暂停等待用户确认。

**技术落地：**
- A2A Adapter 解析 Agent SSE 状态事件时，顺手多写一条 activity 记录到 Redis List（`activity_feed:{trace_id}`）
- SSE 流新增 `activity_json` 事件类型，前端 ActivityPanel 组件实时消费渲染
- 干预机制：`shouldIntervene(action_type, estimated_tokens, confidence)` 根据操作类型和 token 估算判断是否需要用户确认；SSE 流发送 `intervention_required` 事件 → 前端弹出 InterventionDialog → 用户选择 PROCEED/MODIFY/SKIP/ABORT → 流继续或中止
- 自动判断逻辑：写操作 → 高 Token 消耗（≥ 4000 tokens）→ 总是暂停；high_cost_llm 操作（≥ 8000 tokens）→ 总是暂停；写操作且置信度 < 0.6 → 总是暂停

**量化结果：** 用户等待时的焦虑感大幅降低——活动流让等待从"不知道在干什么"变为"看着它一步步做"。干预机制避免了一次误操作可能导致的高额 API 费用或文件损坏。

#### 痛点 7：用户偏好说了 100 遍 Agent 还是记不住 → 记忆断裂

**痛在哪（U2 统一记忆）：** 用户告诉 Agent A "我喜欢简洁的回答"，切换到 Agent B 后又要重新说一遍。更糟糕的是，用户在不同会话中说过的偏好（"我是后端工程师"、"用中文回答"、"代码用 Rust"）完全丢失。每个 Agent 都像第一次见到用户。

**我的考量：** 记忆系统需要同时解决三个问题：(1) 短期记忆——当前对话的上下文；(2) 长期记忆——跨会话的用户画像；(3) 跨 Agent 传递——切换 Agent 时不让用户重复。而且 Agent 不能直接写长期记忆——安全性（恶意 Agent 注入虚假记忆）和一致性（多 Agent 并发写）都无法保证。所以我的设计是"Agent 建议 + 平台审核写入"。

**技术落地：**
- **Tier 1 对话历史**：对话历史按 Agent 粒度隔离并支持尾部裁剪——同一会话中每个 Agent 只看到自己的对话分片，相互不可见，防止多 Agent 会话中的上下文污染；过长历史从尾部裁剪，防止无限增长。
- **Tier 2 用户长期记忆**：用户长期记忆基于 Redis Hash 存储偏好特征，由平台唯一写入方统一落库（`updateUserMemoryFromHints`，Agent 不可直接写入，保证安全性与一致性），查询时读回注入。写端为平台侧对话段提取管道（已落地，批次七：每 4 轮触发、PG 历史为素材、LLM 增量提取只输出新增/变更 hints、异步执行，见 docs/interview/optimization-notes.md P16(a)(c)）；Agent 通过 `AIQueryResponse.memory_hints` 上报仅为备选方向（当前无生产者）。
- **Tier 3 跨 Agent 摘要**：跨 Agent 摘要由 LLM 生成保障切换连贯性——`handleAgentSwitch()` 检测到 Agent 变化时，将前一个 Agent 的最近几轮对话送 LLM 生成短摘要，新 Agent 的 SystemContext 包含此摘要，只继承脉络而非细节。
- **SystemContext 自动注入**：每个 AI 查询前，`buildSystemContext()` 将三层记忆组装为 Protobuf `SystemContext` 注入请求——Agent 无需关心记忆管理。Durable Query Pipeline 第 5 步直接从 PostgreSQL（会话消息 + memory summary）组装该上下文，Redis 仅作加速缓存。
- **沙箱隔离**：沙箱/对比执行（sandbox 标记的请求）不触碰长期记忆写入——memory_hints 与跨 Agent 摘要路径被 sandbox 守卫跳过，试用不会污染真实用户画像。

> **已知局限**：ProfileSummarizer 提取的用户画像当前只写回 Redis 缓存，尚未持久化到 PostgreSQL（已如实记录于代码注释，列入后续计划）。

**量化结果：** 用户偏好设置一次后，所有 Agent 可见。跨 Agent 对话切换时，上下文保留率从 0%（无记忆）提升至约 70%（摘要保留脉络但丢失细节）。

#### 痛点 8：用户不知道哪个 Agent 更好 → 选择盲区、劣币驱逐良币

**痛在哪（U0 能力透明化 + 反馈闭环）：** 当多个 Agent 声称有相同 skill 时，用户完全不知道该选谁。更严重的是，没有反馈机制——Agent A 回答质量差但永远不会被系统"降权"，Agent B 回答好也不会获得更多流量。劣币驱逐良币。

**我的考量：** 反馈闭环需要三个环节：收集（用户点赞/踩）→ 聚合（定时计算质量系数）→ 生效（路由时加权选择）。关键设计是质量系数的计算方法——直接用好评率会忽略样本量差异（一个只有 3 次调用 100% 好评的 Agent 不应该排在有 1000 次调用 95% 好评的 Agent 前面）。我的方案是用 `[0.5, 1.0]` 的压缩区间——即使完全没好評也给 0.5（不直接淘汰，给新人机会），高好评率渐近 1.0（不封顶）。同时在前端做 Agent 对比视图——三列并排展示不同 Agent 对同一问题的回答 + 延迟/成本/长度指标对比。

**技术落地：**
- **AgentSelector 组件**：显示多候选 Agent 的 AgentMetrics（success_rate, avg_latency_ms, p95_latency_ms, approval_rate, total_requests），用户可对比选择
- **反馈收集**：前端 MessageBubble 的 👍/👎 按钮 → `AgentLifecycleService.SubmitFeedback(trace_id, agent_id, skill, rating)` → PG `feedback` 表（insertFeedback）并即时调用 aggregateRouteQuality 写 PG `agent_route_quality`（全程无 Redis 写入）
- **质量聚合**：`FeedbackAggregator::recalculate()` 每小时从 PG 重算各 Agent 各 skill 的质量系数，写入 PG `agent_route_quality` 表。质量系数 = 0.5 + 0.5 × approval_rate（保证范围 [0.5, 1.0]）
- **路由加权**：`selectWeightedByQuality()` 在相同 skill 的候选 Agent 中按质量系数加权随机选择——高好评 Agent 被选中概率更大但不绝对（保留探索空间）
- **CompareView**：前端三列并排视图，对同一查询展示不同 Agent 响应 + 底部对比摘要（延迟/成本/长度）

**实现现状**：`CompareAgents` RPC 已真实落地——并行 worker 对多个候选 Agent 执行同一查询（context_id 前缀 `compare-`，全部经同一条 durable pipeline，计入预算），结果与指标写入 `compare_runs` 表；前端 CompareView 展示真实对比结果。详见[工作流控制指南](guides/workflow-control-guide.md)。

**量化结果：** 反馈闭环使低质量 Agent 的流量自然下降、高质量 Agent 获得更多请求。AgentSelector 面板让用户从"盲选"变为"看数据选"。CompareView 让 Agent 优劣一目了然。

#### 痛点 9：复杂任务只能交给一个 Agent → 能力天花板

**痛在哪（U4 DAG 编排）：** 单个 LLM 的能力是有限的——一个擅长写代码的模型不一定擅长写报告，一个擅长翻译的模型不一定擅长数据分析。当用户说"分析这份数据，写一份英文报告，并做一个 PPT 大纲"时，单个 Agent 可能在某几个环节做得很差。

**我的考量：** 分解任务不难（LLM 擅长做这个），难点在于：(1) 如何自动识别一个查询需要几个 Agent？ (2) 子任务之间有依赖关系怎么办？ (3) 前一个子任务的结果如何传给下一个？ (4) 如果用户想手动调整 Agent 分配怎么办？

**技术落地：**
- **TaskPlanner**：LLM 分析查询 → 生成 `ExecutionPlan`。单 Agent 场景（`is_single_agent = true`）走快速路径跳过编排。多 Agent 场景输出带 `depends_on` 字段的子任务 DAG。
- **TaskExecutor**：Kahn 算法拓扑排序 → 同层 `std::async` 并行执行 → 层间串行等待。前驱子任务结果通过 `buildSubtaskPrompt()` 注入到依赖子任务的 Prompt 中。循环依赖在规划阶段静态检测（Kahn 后仍有未处理任务 = 有环 → 直接报错）。
- **DAG 预览**：前端 DAGPreview 组件用 Mermaid.js 将 `ExecutionPlan` JSON 渲染为可视化流程图。用户可替换子任务的 Agent 分配（从 `candidate_agents` Top-3 中选择），修改后通过 `ExecutePlan` RPC 执行。
- **ResultAggregator**：所有子任务完成后，简单拼接或调 LLM 做综合回答。

**量化结果：** 4 个独立子任务的查询（数据收集 + 行业分析 + 竞品分析 + 趋势预测 → 报告合成），串行执行需 ~12s，DAG 并行执行 ~4s（**67%↓**）。单 Agent 场景占 ~90% 查询，走快速路径零编排开销。

#### 痛点 10：企业运维能力缺失 → 无法用于生产

**痛在哪（U1 自主权 + Batch 5 运维工具 + Batch 6 平台扩展）：** 真正的企业使用场景需要：(1) 控制 Agent 的自主程度（自动执行 vs 仅建议 vs 每次确认）；(2) 查看系统健康状态；(3) 控制 Token 预算防止费用爆炸；(4) 复现历史查询以排查问题；(5) 定时触发任务；(6) 灰度发布新 Agent 版本。这些在现有 Agent 框架中全部缺失。

**我的考量：** 这些不是"锦上添花"而是"生产必备"。但每个功能都要控制复杂度——不能用 500 行代码做一个健康仪表盘，也不能引入 K8s 做灰度发布。我的原则是：能用简单方案就用简单方案，能用现有基础设施就复用。

**技术落地（6 项子优化）：**

**U1 — 自主权梯度（Batch 3 → 已持久化落地）：**
- 三级自主权：L1 建议模式（Agent 只给建议不执行工具）、L2 确认模式（高风险操作暂停确认）、L3 自动模式（完全自主）
- 设置持久化在 PG `autonomy_settings` 表（owner-scoped），`SetAutonomyLevel` RPC 可直接读写；干预决策（InterventionResponse）与撤销（UndoAction）均为 owner-scoped 单条 SQL CAS，详见[工作流控制指南](guides/workflow-control-guide.md)。

**P2 — 健康度仪表盘（Batch 5）：**
- 前端 AdminView 状态灯（绿/黄/红）基于 agent_invocations 事实聚合的健康评估
- 数据来源：agent_invocations 表聚合 + 内存 circular buffer，不引入 Prometheus
- BackgroundScheduler 每 30s 触发 `health_evaluation` 任务 → `ServiceRegistry::evaluateAllHealth()`

**P2 — Token 预算限流（Batch 5 → 已迁移到 PostgreSQL）：**
- 四级配额：全局 → 用户日 → 用户月 → 会话（环境变量 `NEXUSAI_BUDGET_GLOBAL_TOKENS`（0=无限）/`_USER_DAILY_TOKENS`（200000）/`_USER_MONTHLY_TOKENS`（4000000）/`_SESSION_TOKENS`（100000））
- PG 事务内原子 reserve（advisory lock 覆盖首行缺失场景 + counter upsert），预留以 request_id 幂等；超额先落 `rejected` 再返回 RESOURCE_EXHAUSTED（HTTP 429）
- 沙箱/对比流量不豁免，与主链路共用同一预算（预算以 PG 为事实源）

**P2 — 查询重放（Batch 5）：**
- `ReplayQuery(trace_id, mode)` RPC：exact 模式完全相同复现；route 模式重新走路由管线（可能路由到不同 Agent）
- PG `query_log` 表 JSONB 存储完整请求上下文
- 前端 AdminView 输入 trace_id 查看完整调用链时间线

**P2 — 定时任务 / 灰度部署（已移除，如实收敛）：**
- CronScheduler 与 Canary 灰度部署在后续 PostgreSQL 大优化中已从服务端移除（对应前端 tabs 同步删除），避免维护未经充分验证的能力表面；原 `scheduled_tasks` / `canary_deployments` 迁移脚本保留于 V007/V008（只追加不删除）。

**量化结果：** 运维套件使平台从"可以跑"升级为"可以上线"。健康仪表盘让运维从"用户报故障才知道"变为"主动发现"。Token 预算以 PostgreSQL 为事实源，防止单用户失控消费；能力边界如实收敛（移除 Cron/Canary），不保留占位假能力。

#### 痛点 11：没有试用机制 → 新用户门槛高；没有协作机制 → 价值无法传播

**痛在哪（U5 沙箱 + U6 共享）：** 用户看到一个新 Agent 想试用，但没有隔离环境——试用过程中的操作会影响真实数据。用户想让同事看自己跟 Agent 的对话，只能截图。用户发现了一个好用的 Agent 工作流想复用，只能手抄配置。没有分享和模板机制，平台的价值无法通过社交传播。

**我的考量：** 沙箱隔离不需要真正的容器/k8s——用 context_id 前缀 + Redis TTL 就够了。会话分享最简方案是生成只读链接，围观者通过 SSE 实时看。模板就是 DAG 结构的 JSONB 存储 + 一个名字和描述。

**技术落地：**

**U5 — Agent 沙箱：**
- `SandboxQuery` RPC：沙箱执行经同一条 durable pipeline（context_id 前缀 `sandbox-`），预算/owner/trace/成本/finalize 全由管线统一接管，沙箱专属记账写入 `sandbox_runs` 表
- 沙箱流量计入预算（无豁免分支），且隔离长期记忆写入（memory_hints / 跨 Agent 摘要路径被 sandbox 守卫跳过）
- 干预机制与沙箱联动：pending 干预的 PROCEED/MODIFY 决策会触发延迟的沙箱执行，决策具有真实因果效果
- 前端 AgentSandbox 页面：Agent 卡片墙 + 技能标签 + 指标摘要 + 快速试用，含 PROCEED/MODIFY/SKIP 干预决策按钮

**U6 — 会话共享 + 模板市场：**
- `ShareSession(context_id, expiry_days)` → 96 字符高熵 bearer token（每个字节直接取自 `std::random_device` 操作系统熵源），PG 仅存 SHA-256 摘要，raw token 仅创建时返回一次；`share_url` 为相对路径 `/share/<token>`（不写死主机）；TTL（`expiry_days`，0=永不过期）+ owner 侧 `RevokeShare` 撤销
- `ReadSharedConversation` 公开只读 RPC（AuthInterceptor 白名单），固定脱敏（仅 title/messages/shared_at）；过期/撤销 → PERMISSION_DENIED，不存在 → NOT_FOUND
- `SaveTemplate`（服务端 JSON 校验 + 64 KiB 上限）/ `ListTemplates` / `GetTemplate` / `UseTemplate`（在当前 owner 下创建真实会话与初始消息）
- Replay/Export 异常时客户端消息固定脱敏，不回显 `error.what()`（防止 schema 泄露）
- 前端 ShareView（按路由 token 调真实 RPC，四态展示）、TemplateMarket、AdminView Replay tab 全部接入真实后端

**量化结果：** 沙箱使新用户试用零风险。模板机制使 Agent 工作流可沉淀、可复用、可传播——每多一个模板，平台对所有用户的价值就增加一分。这是 Agent Marketplace 的早期雏形。

#### 痛点 12：A2A 协议版本演进 → 新旧 Agent 不兼容

**痛在哪（P3 版本协商 + 委派限制）：** A2A 协议在演进——v1.0 用 `"kind": "text"`、v1.1 用 `"type": "text"`。当一个平台上同时存在 v1.0 和 v1.1 的 Agent 时，消息格式不兼容。更隐蔽的风险是 Agent 委派链——Agent A 把任务委派给 Agent B、B 再委派给 C……如果没有深度限制，可能形成无限委派循环。

**我的考量：** 版本协商不应该让 Agent 开发者感知——他们只需在 AgentCard 中声明 `a2a_version`，平台自动处理格式转换。委派深度限制不应该依赖 Agent 自觉——平台通过 TraceContext 统计同 trace 的 agent_call span 数量，不受 Agent 行为影响。

**技术落地：**
- **版本映射**：`a2a_version` 字段 → `unordered_map<string, VersionProfile>`（kind vs type 映射）。序列化分支在 `message_part.cpp` 的 `to_json(version_profile)` 中自动选择正确字段名。
- **Dual-parse fallback**：解析时先按声明版本解析，失败后尝试另一版本——Agent 声明 v1.0 但响应用 v1.1 格式也能正确解析。Span metadata 标记 `version_fallback: true` 供监控。
- **委派深度限制**：HTTP header `x-delegation-depth` 每次委派 +1。平台端 TraceContext 统计同 trace 的 agent_call span 数。上限 5 层，超限返回错误。兜底不依赖 Agent 协作——平台自己数 span。

**量化结果：** 两个优化共约 150 行 C++，实现协议平滑演进。深度限制使恶意或故障 Agent 无法通过无限委派耗尽系统资源。

### 对标验证

| 维度 | LangChain/LangGraph | AutoGen | NexusAI (本项目) |
|------|--------------------|---------|------------------|
| 路由延迟 | 600-1200ms (LLM) | 无路由 | **< 100ms (四层渐进式)** |
| Agent 通信 | Python 进程内 | Python 进程内 | **A2A 网络协议** |
| 跨语言 Agent | ❌ | ❌ | **✅** |
| 熔断降级 | ❌ | ❌ | **✅ 三态熔断器 + 自动回退** |
| 工具选择 | 全量传入 | 全量传入 | **✅ RAG Top-K** |
| 语义缓存 | ❌ | ❌ | **✅ 余弦相似度 0.92** |
| 上下文压缩 | ❌ | ❌ | **✅ 70% 窗口触发** |
| 记忆系统 | 单层 | ❌ | **✅ 三层隔离** |
| Agent 对比 | ❌ | ❌ | **✅ CompareAgents 并行真实执行** |
| 活动流 | ❌ | ❌ | **✅ ActivityPanel 实时** |
| 用户干预 | ❌ | ❌ | **✅ 单条 SQL CAS 决策 + Undo 原子回滚** |
| DAG 编排 | 有 (LangGraph) | 有 | **✅ 拓扑并行 + 用户可调** |
| 反馈闭环 | ❌ | ❌ | **✅ 评分 → 聚合 → 路由加权** |
| 健康仪表盘 | ❌ | ❌ | **✅ AdminView 状态灯** |
| Token 预算 | ❌ | ❌ | **✅ PG 四级配额 + 拒绝落库** |
| 查询重放 | ❌ | ❌ | **✅ exact + route 双模式（owner-scoped）** |
| 灰度发布 | ❌ | ❌ | ⚠️ 已如实移除（能力边界收敛） |
| 沙箱试用 | ❌ | ❌ | **✅ durable pipeline + 长期记忆隔离** |
| 会话共享 | ❌ | ❌ | **✅ 熵源 token + 哈希存储 + TTL/撤销** |
| 模板市场 | ❌ | ❌ | **✅ UseTemplate 创建真实会话** |
| 版本协商 | ❌ | ❌ | **✅ v1.0/v1.1 + dual-parse fallback** |
| 全链路追踪 | ❌ | ❌ | **✅ TraceContext + Span + Cost** |
| 前端 UI | ❌ | ❌ | **✅ 10 视图 Vue 3 SPA** |

---

## 3. 技术架构总览

### 3.1 模块依赖图（自底向上）

```text
proto/          → 9 个 Proto 文件，生成 gRPC/Protobuf 桩代码
                    (common, agent_service, ai_query, user,
                     observability, agent_lifecycle,
                     user_experience, orchestration, sharing)
common/         → Logger, CircuitBreaker, LoadBalancer (6 策略),
                  RedisClient, MemoryService (三层记忆),
                  BackgroundScheduler, EnvLoader, TraceContext
registry/       → ServiceRegistry (Agent 注册/发现/健康管理)
a2a/            → 纯 A2A 协议库 (C++ A2AClient, JSON-RPC, AgentCard)
a2a_adapter/    → gRPC Protobuf ↔ A2A JSON-RPC 双向适配器
orchestrator/   → AgentRouter (四层路由) + TaskPlanner (DAG 分解)
                  + TaskExecutor (拓扑并行执行) + ResultAggregator (LLM 合成)
mcp/            → MCPClient (STDIO+SSE) + RAG-MCP (Embedding+向量检索)
                  + SemanticCacheIndex (语义缓存)
server/         → gRPC Server (9 个 Service 注册, Auth/Cost 拦截器)
                    全部实现: AIQueryService (含 durable pipeline),
                    AgentCommunicationService, AgentLifecycleService,
                    OrchestrationService, ObservabilityService,
                    UserExperienceService, SharingService,
                    HealthService, UserService
client/         → gRPC 客户端 (交互式终端, /stream /context /status)
frontend/       → Vue 3 + TypeScript + Vite SPA (10 个视图)
gateway/        → Node.js gRPC Proxy (主力) + Nginx + Envoy (Docker 可选)
db/             → PostgreSQL 迁移与数据访问 (db/migrations V001–V015
                  只追加; rpc-server 启动时自动迁移，失败即中止)
```

### 3.1.1 gRPC Service 与 RPC 方法清单（41 个）

| Service | RPC 方法 | 类型 |
|---------|---------|------|
| **AIQueryService** | Query, QueryStream, GetQueryStatus, GetAgentMetrics | Unary + Server Streaming |
| **AgentCommunicationService** | SendMessage, ReceiveMessage, BroadcastMessage, GetAgents, FindAgents, RegisterAgent, UnregisterAgent, Heartbeat, ListenMessages, BatchSendMessages, RealTimeCommunication | Unary + Stream |
| **AgentLifecycleService** | SubmitFeedback, GetAgentCompare, SetAutonomyLevel, UndoAction, CompareAgents | Unary |
| **ObservabilityService** | GetTraceDetail, GetCostReport | Unary |
| **OrchestrationService** | ExecutePlan, ReplayQuery, ExportConversation | Unary |
| **UserExperienceService** | InterventionResponse, SandboxQuery | Unary |
| SharingService | ShareSession, ObserveSession, SaveTemplate, UseTemplate, ReadSharedConversation, ListShares, RevokeShare, ListTemplates, GetTemplate | Unary + Server Streaming |
| **UserService** | Register, Login, ValidateToken | Unary |
| HealthService | Check, Watch | Unary + Server Streaming |

> **注**：以上 RPC 均已接入真实实现（`RealTimeCommunication` 返回 UNIMPLEMENTED 并说明替代方案，为如实的能力边界）。所有业务的 owner 身份一律取自认证上下文（AuthInterceptor），绝不采信请求体中的 user_id；`RegisterAgent`/`UnregisterAgent` 为 ADMIN-only（登录用户须匹配 `NEXUSAI_ADMIN_USERNAME`）。

### 3.2 三条数据流路径

**路径 A — 浏览器端 (Node.js Proxy，主力)：**

```text
Browser (Vue 3 SPA, Fetch API / SSE)
  → Node.js Proxy :8081 (gRPC Server Streaming → SSE 转换)
    → RPC Server :50051
      → A2AAdapter (Protobuf → A2A JSON-RPC)
        → Orchestrator :5000 (意图识别 → 路由)
          → Agent :5100 (A2A 协议处理)
            → MCP Tools (工具调用)
```

**路径 B — 直接 gRPC 客户端：**

```text
rpc_client (CLI, C++)
  → RPC Server :50051 (直连或通过 Nginx :8082)
    → A2AAdapter → A2A Client (HTTP)
      → Orchestrator :5000 → LLM API (意图分类)
        → AgentRouter (匹配最佳 Agent)
          → Agent (HTTP JSON-RPC) → 返回结果
```

**路径 C — Vite 开发服务器（无 Nginx）：**

```text
Browser (Vite HMR :5173)
  → Vite proxy → Node.js Proxy :8081 → RPC Server :50051
```

### 3.3 服务端口映射

| 端口 | 服务 | 协议 | 说明 |
|------|------|------|------|
| 50051 | RPC Server | gRPC/2 | C++ 核心服务，对外统一入口 |
| 5432 | PostgreSQL 16 | TCP | 持久事实源（Compose 默认不发布宿主端口，本地测试可发布） |
| 5000 | Orchestrator | HTTP/A2A | Python A2A 路由代理 |
| 5100 | Mock Agent | HTTP/A2A | 开发/测试用模拟 Agent |
| 8080 | 前端 (Docker nginx) | HTTP/1.1 | 浏览器入口（Docker 部署生产端口，`/health` 可探活） |
| 8081 | Node.js Proxy / Envoy | HTTP/1.1 → gRPC/2 | gRPC→SSE 转换(主力) / gRPC-Web 转换(Docker) |
| 8082 | Nginx | gRPC/2 | 后端 gRPC 直连入口 (Docker 部署可选) |
| 6379 | Redis 7 | TCP | 仅缓存/心跳存活/限流/短锁（可随时清空，不丢持久数据） |

---

## 4. 核心模块详解

### 4.1 AgentRouter — 四层渐进式路由引擎

这是本项目最核心的技术组件，替代了传统的"每次调大模型做意图分类"方案（单次 600-1200ms）。

**四层路由管线：**

```
用户请求
  │
  ▼
┌─────────────────────────────────────────────────────────────┐
│ 第一层：Embedding 向量匹配 (高置信度阈值 0.85)               │
│   - 将查询向量化，与所有已注册 Agent Skill 的向量做余弦相似度 │
│   - 相似度 ≥ high_threshold → 直接返回匹配 skill             │
│   - 覆盖约 80% 查询，延迟 < 100ms                            │
│   - 命中即返回，不触发后续层                                  │
└─────────────────────────────────────────────────────────────┘
  │ 未命中（低置信度或无 Embedding 配置）
  ▼
┌─────────────────────────────────────────────────────────────┐
│ 第二层：LLM 动态意图分类                                     │
│   - buildDynamicIntentPrompt() 从注册表动态构建 Prompt        │
│   - 不硬编码 skill 列表，Agent 注册/注销自动更新              │
│   - 延迟约 300-800ms，返回精确 skill 名称                    │
│   - 仅在 Embedding 层低置信度时触发                           │
└─────────────────────────────────────────────────────────────┘
  │ 未命中（LLM 不可用或返回未知 skill）
  ▼
┌─────────────────────────────────────────────────────────────┐
│ 第三层：IDF 加权关键词索引                                   │
│   - 从 Agent Skill 名称和描述中提取关键词                    │
│   - IDF 权重 = 1/含此关键词的 skill 数（罕见词权重高）        │
│   - 倒排索引 O(1) 查找，延迟 < 1ms                           │
│   - rebuildSkillKeywordIndex() 随 Agent 增删自动重建         │
└─────────────────────────────────────────────────────────────┘
  │ 未命中（无关键词匹配）
  ▼
┌─────────────────────────────────────────────────────────────┐
│ 第四层：回退到健康通用 Agent                                  │
│   - findFallbackAgent() 选择最健康的通用 Agent               │
│   - 反馈驱动加权：getQualityCoefficient() 计算质量系数        │
│   - 质量系数范围 [0.5, 1.0]，基于历史点赞率                   │
│   - selectWeightedByQuality() 加权随机选择                   │
└─────────────────────────────────────────────────────────────┘
```

**核心接口：**

```cpp
// 选择 Agent
std::optional<AgentInfo> selectAgent(
    const std::string& question,
    const std::vector<std::string>& required_skills = {});

// 通用 Dispatch（路由 + 调用合一，消除 if/else 分发）
DispatchResult dispatch(
    const std::string& question,
    const AgentDispatchFn& call_fn,
    const std::vector<std::string>& required_skills = {});

// 动态意图 Prompt 构建（从注册表生成，不硬编码）
std::string buildDynamicIntentPrompt(const std::string& user_text) const;

// 反馈驱动质量系数
double getQualityCoefficient(const std::string& agent_id,
                             const std::string& skill_name);
```

**路由效果：**
- Embedding 快路径覆盖 ~80% 查询，路由成本降低 ~75%
- 阈值参数化：`ROUTING_HIGH_THRESHOLD` (0.85) / `ROUTING_LOW_THRESHOLD` (0.50) 运行时可调
- 线程安全：`agents_mutex_` 保护 Agent 列表，`embedding_mutex_` 保护向量索引
- 增量索引：Agent 注册/注销时 `rebuildSkillKeywordIndex()` + `buildSkillEmbeddingIndex()`

### 4.2 A2AAdapter — gRPC ↔ A2A 协议转换桥梁

A2AAdapter 是整个数据流的中枢神经。它将 Protobuf 格式的 gRPC 请求转换为 A2A JSON-RPC 格式，然后通过 HTTP 发送给 Orchestrator/Agent，再将 A2A 响应转回 Protobuf。

**四种调用模式：**

| 模式 | 方法 | 用途 |
|------|------|------|
| 同步调用 | `processQuery()` | 阻塞等待完整结果，适用于 CLI 和批处理 |
| 异步调用 | `processQueryAsync()` | 回调模式，适用于后台任务 |
| 流式调用 | `processQueryStreaming()` | SSE 流式接收，适用于前端实时展示 |
| 直连调用 | `processQueryDirect()` / `processQueryStreamingDirect()` | 绕过路由，使用预解析的 Agent URL |

**关键设计：**

```cpp
// 用户干预判断 (Batch 4 U3)
bool shouldIntervene(const std::string& action_type,
                     long estimated_tokens = 0,
                     double confidence = 1.0) const;

// 自主权级别注入 (Batch 3 U1)
void injectAutonomyHeader(
    const agent_communication::AIQueryRequest& request,
    const std::string& agent_id, a2a::A2AClient* client = nullptr);

// 任务取消 (P2-2)
bool cancelTask(const std::string& task_id);

// 请求级超时覆盖
void setRequestTimeout(long seconds);
```

**全链路 Bug 修复经验：**
在开发过程中定位并修复了三个跨层 Bug：
1. **Adapter 事件类型静默丢弃**：A2A Server-Sent Events 的事件类型未完整映射到 gRPC AIStreamEvent 类型，导致部分 Agent 状态变更无法传递给前端
2. **Node.js 代理异步竞态崩溃**：Node gRPC Proxy 的异步 I/O 在高并发时 `ECONNRESET`，通过在 `processQueryStreaming()` 中加入 HTTP 连接重试和超时控制解决
3. **C++/Python 协议字段不一致**：Protobuf `ServiceInfo` 字段编号在 proto 演进后与服务端注册代码不一致，统一为 12 字段并加入 `a2a_version` 协商

### 4.3 TaskPlanner + TaskExecutor — DAG 任务编排引擎

面对"写一份市场分析报告并翻译成英文"这类复杂查询，系统通过 LLM 将其分解为子任务 DAG（有向无环图），拓扑排序后分层并行执行。

**数据结构：**

```cpp
struct ExecutionPlan {
    std::string original_query;            // 原始查询
    std::vector<SubTask> tasks;            // 子任务列表
    bool is_single_agent = true;           // true → 快速路径跳过编排
    std::string single_agent_skill;        // 单 Agent 场景直接路由
};

struct SubTask {
    std::string id;                        // "t1", "t2"...
    std::string description;               // 子任务 Prompt
    std::string required_skill;            // 所需技能
    std::vector<std::string> depends_on;   // 前置依赖子任务 ID
    std::vector<CandidateAgent> candidate_agents; // Top-3 候选 Agent
};
```

**执行流程：**

1. `TaskPlanner::plan()` — LLM 分析查询 → 生成 ExecutionPlan（单 Agent 或多 Agent DAG）
2. `TaskPlanner::resolveAgents()` — 对每个子任务调用 AgentRouter 预绑定最佳 Agent
3. `TaskExecutor::topologicalLayers()` — Kahn 算法将 DAG 分层（同层可并行）
4. `TaskExecutor::execute()` — 同层 `std::async` 并行执行，跨层串行等待
5. `ResultAggregator` — 收集所有子任务结果，简单拼接或 LLM 合成

**关键优化：**
- 单 Agent 场景走快速路径（`is_single_agent = true`），跳过编排开销
- 同层独立子任务 `std::async` 并行执行，减少总延迟
- 前置子任务结果通过 `buildSubtaskPrompt()` 注入到依赖子任务的 Prompt 中

### 4.4 RAG-MCP — 检索增强工具选择

当 Agent 拥有大量 MCP 工具时（几十到上百个），将全部工具描述传给 LLM 会导致上下文溢出和延迟飙升。RAG-MCP 通过向量检索先筛选 Top-K 相关工具。

**核心组件：**

| 组件 | 功能 |
|------|------|
| `EmbeddingService` | 调用 OpenAI 兼容 Embedding API 生成文本向量（1024 维） |
| `VectorIndex` | 内存向量索引，支持余弦相似度 Top-K 搜索 + 阈值过滤 |
| `EmbeddingCache` | LRU 缓存，对相同文本复用向量，减少 API 调用 |
| `ToolRetriever` | 业务流程编排：查询向量化 → 搜索 → 返回 Top-K 工具 |
| `ToolValidator` | 验证工具可用性（参数 schema 校验） |

**检索流程：**
```
用户查询 "求解方程"
  → EmbeddingService.embed("求解方程") → query_vector
  → VectorIndex.search(query_vector, top_k=5, threshold=0.3)
  → 返回 Top-5 相关工具: [calculator, equation_solver, math_parser, ...]
  → 将这 5 个工具的 name + description + input_schema 传给 LLM
  → LLM 从 5 个中选出最合适的工具并生成参数 → 调用工具
```

**SemanticCacheIndex (Batch 3)** 复用 VectorIndex 基础设施，实现语义缓存：对于语义相似（余弦相似度 ≥ 0.92）但措辞不同的重复查询，直接返回缓存结果，Token 消耗降为零。

### 4.5 MemoryService — 三层记忆系统

解决了"Agent 切换时上下文丢失"和"长期记忆跨会话持久化"两大痛点。

```
┌──────────────────────────────────────────────────────┐
│ Tier 1: 对话历史 (Conversation History)               │
│ - Redis List: nexusai:conv:{context_id}:{agent_id}   │
│ - 按 (context_id, agent_id) 分片存储                  │
│ - LTRIM 限制 50 条/片，防止无限增长                   │
│ - 防止多 Agent 会话中的上下文污染                     │
├──────────────────────────────────────────────────────┤
│ Tier 2: 用户长期记忆 (Long-term Memory)               │
│ - Redis Hash: nexusai:memory:{user_id}               │
│ - Agent 上报通道为设计意图（无生产实现）        │
│ - updateUserMemoryFromHints() 批量写入                │
│ - 平台统一管理，Agent 不可直接写入（安全性）          │
├──────────────────────────────────────────────────────┤
│ Cross-Agent Summary: 跨 Agent 摘要                    │
│ - Redis String: nexusai:summary:{context_id}          │
│ - Agent 切换时 LLM 自动生成上下文摘要                 │
│ - 新 Agent 继承上一个 Agent 的对话脉络                │
│ - buildSystemContext() 构建统一注入上下文              │
└──────────────────────────────────────────────────────┘
```

**SystemContext 组装（每次 AI 调用前自动执行）：**

```protobuf
message SystemContext {
    string user_id = 1;                    // 用户 ID
    string user_memory = 2;                // Tier 2: 长期记忆
    string conversation_history = 3;       // Tier 1: 当前 Agent 历史
    string cross_agent_summary = 4;        // 跨 Agent 摘要
}
```

### 4.6 A2A 协议库 — 自主实现的 C++ A2A 客户端

A2A (Agent-to-Agent) 是 Google 提出的开放协议，定义了 Agent 之间如何通过标准 HTTP/JSON-RPC 进行通信。本项目在 `a2a/` 模块中完整实现了 A2A 协议的 C++ 客户端库，这是整个 Agent 通信框架的网络基础。

**核心能力：**

```cpp
class A2AClient {
    // 同步调用：发送消息并等待完整响应
    A2AResponse send_message(const MessageSendParams& params);

    // 流式调用：发送消息，通过回调接收 SSE 事件流
    void send_message_streaming(const MessageSendParams& params,
                                std::function<void(const std::string&)> callback);

    // 任务管理：查询和取消异步任务
    AgentTask get_task(const std::string& task_id);
    AgentTask cancel_task(const std::string& task_id);

    // 任务订阅：监听任务状态变更的 SSE 流
    void subscribe_to_task(const std::string& task_id,
                          std::function<void(const std::string&)> callback);

    // HTTP 头注入：支持自主权级别、委派深度等平台级控制
    void add_header(const std::string& key, const std::string& value);
};
```

**技术实现亮点：**
- 基于 `libcurl` 的异步 HTTP 传输，支持长连接复用和连接池
- 完整的 JSON-RPC 2.0 序列化/反序列化（`to_json` / `from_json`）
- AgentCard 元数据模型：技能列表、端点、版本、能力描述的结构化表示
- Pimpl 模式封装实现细节，头文件只暴露稳定的公共接口
- 移动语义支持（move constructor / move assignment），适配现代 C++ 资源管理

### 4.7 CircuitBreaker — 熔断器

经典的三态熔断器模式（CLOSED → OPEN → HALF_OPEN），保护系统免受故障 Agent 的级联影响。

已在 `a2a_adapter.cpp` 中全面接入，覆盖 `processQuery()`、`processQueryStreaming()`、`processQueryDirect()`、`processQueryStreamingDirect()` 四个调用路径，包含 10 处 `recordSuccess()`/`recordFailure()` 调用。

- **CLOSED（正常）**：请求正常通过，统计成功率。累计失败 ≥ `failure_threshold`（默认 5） → OPEN
- **OPEN（熔断）**：拒绝所有请求，直接返回错误。等待 `timeout` (60s) → HALF_OPEN
- **HALF_OPEN（探测）**：允许少量请求通过。成功 ≥ `success_threshold` → CLOSED；任何失败 → OPEN

```cpp
// 模板方法，自动包装任意函数调用
template<typename Func>
auto execute(Func&& func) -> decltype(func());

// 单例管理器，按 service_name 管理多个熔断器
CircuitBreakerManager::getInstance().getCircuitBreaker("agent:mock-unstable");
```

### 4.8 BackgroundScheduler — 统一后台任务调度

Coordinator + Worker Pool 架构，避免后台任务散落为独立线程：

```
Coordinator 线程 (每 500ms 扫描)
  → 发现 due 且未 running 的任务
    → ready_queue_.push(task)
      → Worker 线程池 (默认 2 个)
        → 执行任务 fn()
        → 更新 next_run
```

已注册的 6 个定时任务涵盖：Span 批量刷新、反馈聚合、缓存清理、用户画像提取、健康评估、指标重算（Cron 扫描与灰度评估已随能力边界收敛移除）。

### 4.9 LoadBalancer — 6 种负载均衡策略

通过策略模式支持 6 种负载均衡算法，可在运行时切换：

| 策略 | 适用场景 |
|------|---------|
| `ROUND_ROBIN` | 无状态、等容量 Agent |
| `RANDOM` | 简单均匀分布 |
| `LEAST_CONNECTIONS` | 长连接场景 |
| `WEIGHTED_ROUND_ROBIN` | Agent 容量不均 |
| `CONSISTENT_HASH` | 需要会话亲和性 |
| `LEAST_RESPONSE_TIME` | 性能敏感场景 |

---

## 5. 核心技术亮点

### 5.1 四层渐进式路由（替代纯 LLM 意图分类）

**传统方案问题：** 每次查询调用 LLM 做意图分类 → 600-1200ms 延迟 + Token 消耗 + API 费用

**本项目方案：** 四层管线，逐层回退：
- **Layer 1 (Embedding)** — 80% 查询命中，延迟 < 100ms，零 Token 消耗
- **Layer 2 (LLM)** — 仅当 Embedding 不确定时触发，动态 Prompt 从注册表生成
- **Layer 3 (Keyword IDF)** — O(1) 倒排索引，延迟 < 1ms
- **Layer 4 (Fallback)** — 反馈驱动的加权随机选择，兜底保障

**量化对比：**

| 方案 | P50 延迟 | Token 消耗 | API 费用/千次查询 |
|------|---------|-----------|------------------|
| 纯 LLM 意图分类 | 600-1200ms | ~500 tokens/次 | ~$1.50 |
| **四层渐进式（本项目）** | **< 100ms**（80% Embedding 命中） | **~100 tokens/次**（仅 20% 触发 LLM） | **~$0.30** |

路由成本降低约 **75%**，P50 延迟从 ~600ms 降至 < 100ms，年化 API 费用节省约 80%。

### 5.2 DAG 编排 + 拓扑并行执行

不同于简单的"一个 Agent 干到底"或"串行调用多个 Agent"，本项目的 TaskExecutor 实现了真正的 DAG 并行：

```
查询："写一份市场分析报告并翻译成英文"

ExecutionPlan:
  t1: 数据收集 ──────┐
  t2: 行业分析 ──────┤
  t3: 竞品分析 ──────┼─→ t5: 报告合成 ─→ t6: 英文翻译
  t4: 趋势预测 ──────┘

执行：Layer 0: [t1, t2, t3, t4] 并行 → Layer 1: [t5] → Layer 2: [t6]
```

### 5.3 RAG 驱动的工具智能选择

面对 100+ 工具的 Agent，传统方案将所有工具的 name + schema 传给 LLM → 4000+ tokens 上下文占用 → 模型容易选错或幻觉。本项目通过向量检索预处理，只传入 Top-5 最相关工具（~400 tokens），检索延迟 < 50ms，选择准确率接近全量传入。

### 5.4 三层记忆系统防 Agent 污染

关键洞察：多 Agent 系统不能把所有对话历史混在一起。每个 Agent 看到的上下文应该是"与我相关的历史 + 跨 Agent 摘要"，否则会相互污染（比如翻译 Agent 看到了代码 Agent 的 Python 报错上下文）。

- Tier 1 按 `(context_id, agent_id)` 分片隔离
- Tier 2 平台统一管理，Agent 只能"建议"写入，不能直接写入
- Cross-Agent Summary 通过 LLM 生成摘要，新 Agent 只看到脉络而非细节

### 5.5 反馈驱动的路由自优化

用户对 Agent 回答的点赞/点踩不仅记录在 UI 上，还通过 `AgentLifecycleService.SubmitFeedback` 写回后端，`FeedbackAggregator` 每小时重算质量系数，`AgentRouter` 在下一轮路由中使用 weighted random selection 偏向高好评 Agent。形成闭环。

### 5.6 全链路可观测性（PostgreSQL 为事实源）

不依赖 Prometheus/Jaeger 等外部系统，内置：
- **TraceContext**（thread_local）：零侵入的 tracing，任意调用栈深度获取 trace_id
- **TokenCostTracker**：每个 LLM 调用的 Token 消耗记录到 PG `token_usage_ledger` 表（estimated 标记透传，不假装精确）
- **TraceSpan**：分布式追踪 Span（router → planner → agent_call → executor → aggregator），持久化于 PG `traces` 表
- **agent_invocations 事实表**：生产写者为 Query/QueryStream 管线（单 agent A2A 路径与 orchestrator 多 Agent 路径均按真实调用记录），小时聚合产出 Agent 指标
- **GetTraceDetail / GetCostReport RPC**：直接读 PG（trace_payload 解析 spans；成本来自 token_usage_ledger 日聚合，estimated 与精确分桶分离），Redis 清空后仍可完整查询
- **反馈聚合**：SubmitFeedback 校验闭合键空间（≤128 且仅 `[A-Za-z0-9._:-]`）与 trace 归属，owner-scoped 写入 `feedback` + 即时聚合 `agent_route_quality`（Beta(2,2) 平滑）

### 5.7 Durable Query Pipeline — 六步持久化管线（本次大优化的核心）

登录用户的每次 Query/QueryStream 都固定走六步，PostgreSQL 是持久事实源，Redis 仅缓存/心跳/限流/短锁：

1. **auth owner**：owner 只取 `AuthInterceptor::currentUserId()`，为空即 UNAUTHENTICATED，不建任何库行；请求体中的 user_id 被无条件覆盖（防伪造）
2. **ensureConversation**：首次创建/同 owner 幂等/跨 owner 拒绝（INSERT + subtransaction 捕获唯一冲突，无 upsert）
3. **running 行**：`query_logs` + `traces` 落 `running` 状态，query_log id = request_id（与预算预留共用同一幂等键）
4. **预算 reserve**：PG 事务内预留估算 token，拒绝时先落 `rejected` 终态再返回 RESOURCE_EXHAUSTED
5. **SystemContext**：从 PG 会话消息 + memory summary 组装，Redis 仅加速
6. **finalize CAS**：`compare_exchange_strong` 保护恰好一次终态持久化（消息/query log/trace/token ledger）

确定性幂等主键：`msg-user-<request_id>` / `msg-assistant-<request_id>` / `usage-<request_id>` —— 同 request_id 重试永不重复写行；“拒绝后重试成功”也会被完整记录且恰好计费一次。流式终态由 AIQueryServiceImpl 唯一发射（原子标志），客户端断开 → stream.cancel() → 服务端落 `cancelled`。详见 [Durable Query Pipeline 指南](guides/durable-query-pipeline-guide.md)。

### 5.8 单条 SQL CAS 的工作流控制

Sandbox/Compare/Intervention/Undo/Autonomy 五件套全部 owner-scoped 且基于 PostgreSQL 单条 SQL CAS：干预决策 `UPDATE interventions ... WHERE state='pending'`（并发决策者以 affected_rows 定胜负，重复/越权映射真实 ALREADY_EXISTS/NOT_FOUND）；Undo 在单事务内先 CAS `undone_at` 再执行 inverse 补偿，失败整体回滚、动作不被消耗可重试。详见[工作流控制指南](guides/workflow-control-guide.md)。

### 5.9 轻量化设计哲学 — 零重型外部依赖

区别于企业级方案动辄引入 K8s + Istio + Prometheus + Jaeger + Temporal 等技术栈，本项目坚持**个人项目轻量化**原则：

| 能力 | 企业方案 | 本项目方案 | 代码量 |
|------|---------|-----------|--------|
| 后台任务调度 | Temporal/Celery | BackgroundScheduler (单头文件 ~150 行) | 150 行 C++ |
| 分布式追踪 | Jaeger/OpenTelemetry | TraceContext (thread_local) + PG traces 表 | 171 行 C++ |
| 监控告警 | Prometheus + Grafana | agent_invocations 表 + 内存 circular buffer | 内嵌于 AdminView |
| 语义缓存 | GPTCache/RedisAI/FAISS | SemanticCacheIndex (复用 VectorIndex) | 复用现有基础设施 |
| 灰度发布 | K8s + Istio | `std::uniform_int_distribution` 加权随机 | 10 行 C++ |
| 任务编排 | Temporal/Cadence | Kahn 拓扑排序 + `std::async` 并行 | 200 行 C++ |

**设计收益：** 零新增外部服务依赖，编译即运行，单机可承载全功能。这体现了系统设计中的关键判断力——什么时候需要引入重量级基础设施，什么时候可以用几十行代码解决同样的问题。

### 5.10 A2A 网络化 Agent 架构

区别于 LangChain/AutoGen 的进程内函数调用，本项目 Agent 之间通过标准 HTTP/JSON-RPC 协议通信。这意味着：

- Agent 可以用**任何语言**实现（Python、Go、Rust、Java...），只需实现 A2A JSON-RPC 接口
- Agent 可以**独立部署、独立扩缩容**，不影响平台
- 未来可以实现**跨组织 Agent 互操作**（类似 SMTP 之于邮件）

---

## 6. 8 批次优化工程详解

项目经历了系统的 8 批次 21 项优化 + 批次八 14 项迭代，每批自包含、可独立编译测试。

### Batch 1 — 基础设施底座

**目标：** 为后续所有批次建立共享基础设施

| 优化项 | 交付物 | 技术要点 |
|--------|--------|---------|
| P0 成本可观测 | TokenCostTracker | 每条 LLM 调用的 Token/Prompt/Completion/费用记录到 PG |
| P0 分布式追踪 | TraceContext + TraceSpan | thread_local 零侵入追踪；span 缓冲 100ms 批量刷 PG |
| 后台调度器 | BackgroundScheduler | Coordinator+Worker Pool；防重入；优雅停止；6 个注册任务 |
| Proto 拆分 | 4 个新 proto 文件 | observability / agent_lifecycle / user_experience / orchestration |
| 数据表 | 3 张 PG 表 | agent_invocations / token_usage_ledger / traces |

### Batch 2 — 可用性 + 信任数据

| 优化项 | 交付物 | 技术要点 |
|--------|--------|---------|
| P1 熔断回退 | CircuitBreaker 接入 | a2a_adapter.cpp HTTP 调用前后各 5 行接入代码 |
| P1 用户反馈闭环 | FeedbackAggregator（即时，无 Redis） | SubmitFeedback 即时调用 `aggregateRouteQuality` 写 PG `agent_route_quality`（Beta(2,2) 先验）；每小时任务兜底重算 |
| U0 能力透明化 | AgentMetrics proto + GetAgentMetrics RPC | 前端 AgentSelector 面板展示指标对比 |

**量化结果：** 故障 Agent 首次请求即被熔断（而非 30s 超时）；feedback 闭环使低质量 Agent 流量自然下降；用户从"盲选 Agent"变为"看指标选"。

### Batch 3 — 降本 + 控制

| 优化项 | 交付物 | 技术要点 |
|--------|--------|---------|
| P1 语义缓存 | SemanticCacheIndex | 复用 VectorIndex + LRU; 余弦相似度 ≥ 0.92 → 缓存命中 |
| P1 上下文压缩 | ContextCompressor | Token 估算 > 70% 窗口触发; 摘要替换历史 |
| U1 自主权梯度 | Autonomy Levels (1..4) | PG `autonomy_settings` 持久化（owner-scoped，请求体 user_id 被忽略）; level ≤ 2 需干预确认 |

**量化结果：** 语义缓存命中率 15-20%（取决于查询模式），每次命中节省 100% Token。上下文压缩使第 20 轮 prompt 从 ~15000 tokens 降至 ~2000 tokens（**87%↓**）。自主权梯度避免了一次误操作的高额费用。详见上文痛点 5（语义缓存 + 上下文压缩）和痛点 10（自主权梯度）。

### Batch 4 — UX 核心差异

| 优化项 | 交付物 | 技术要点 |
|--------|--------|---------|
| U2 统一记忆 | UserProfile + ProfileSummarizer | Redis `user_profile:<uid>`（**画像未持久化 PG**，见 §12.1 已知局限）; ProfileSummarizer 从对话中异步提取用户偏好 |
| U3 活动流 | ActivityFeed + 干预确认 | SSE `activity_json` 事件; 前端 ActivityPanel 实时展示 |
| U4 DAG 组合推荐 | DAG 预览 + 用户调整执行 | Mermaid.js 渲染; 前端可替换节点 → ExecutePlan RPC |

**量化结果：** 用户等待焦虑感大幅降低；活动流让"不知道在干什么"变为"看着一步步做"；并行 DAG 使 4 子任务查询从 ~12s 串行降至 ~4s（**67%↓**）；单 Agent 场景走快速路径零编排开销。

### Batch 5 — 运维工具

| 优化项 | 交付物 | 技术要点 |
|--------|--------|---------|
| P2 健康度仪表盘 | AdminView 状态灯 | agent_invocations 聚合 + 内存 circular buffer |
| P2 Token 预算 | PG Budget Reserve | 全局/用户日/用户月/会话；PG 事务 advisory lock + counter upsert；request_id 幂等（后续大优化中从 Redis Lua 迁入 PG） |
| P2 查询重放 | ReplayQuery RPC | exact=完全相同 / route=重新路由（可能不同 Agent） |

**量化结果：** 运维从"用户报故障才知道"→"主动发现"；Token 预算防止单用户失控消费；查询重放将排查时间从 20 分钟降至 5 秒。

### Batch 6 — 平台扩展

| 优化项 | 交付物 | 技术要点 |
|--------|--------|---------|
| P2 定时任务 | CronScheduler（后续已移除） | 曾复用查询管线；能力边界收敛时从服务端删除 |
| P2 灰度发布 | Canary Deployment（后续已移除） | 曾以 weighted random 分流；能力边界收敛时从服务端删除 |
| P2 对话导出 | ExportConversation RPC | Markdown + HTML 格式; ~150 行纯字符串拼接 |

**量化结果：** 对话导出使会话可存档/分享/审计；定时任务与灰度发布在后续优化中如实移除，不保留占位能力。

### Batch 7 — 增长与留存

| 优化项 | 交付物 | 技术要点 |
|--------|--------|--------|
| U5 沙箱试用 | AgentSandbox + SandboxQuery RPC | context_id 前缀 `sandbox-`；经 durable pipeline，计入预算；隔离长期记忆写入 |
| U6 协作共享 | ShareSession + Templates | 熵源 token + SHA-256 哈希存储 + TTL/撤销；UseTemplate 创建真实会话 |

**实现现状**：Batch 7 全部前后端已真实打通（SandboxQuery/CompareAgents/SharingService 均非占位），前端 AgentSandbox、CompareView、ShareView、TemplateMarket 展示真实数据；详见[工作流控制指南](guides/workflow-control-guide.md)与[分享与资产指南](guides/sharing-and-assets-guide.md)。

**量化结果：** 沙箱使新用户试用零风险；模板机制使 Agent 工作流可沉淀、可复用、可传播——每多一个模板，平台价值对所有用户 +1。这是 Agent Marketplace 的早期雏形。

### Batch 8 — 协议与安全

| 优化项 | 交付物 | 技术要点 |
|--------|--------|---------|
| P3 A2A 版本协商 | a2a_version 字段 + 版本映射 | v1.0 `"kind": "text"` vs v1.1 `"type": "text"`; dual-parse fallback |
| P3 委派深度限制 | HTTP header `x-delegation-depth` | TraceContext 统计同 trace 的 agent_call span 数; 上限 5 层 |

**量化结果：** 两个优化共约 150 行 C++，实现协议平滑演进。新旧 Agent 可共存，无限委派被平台兜底阻断。

---

## 7. 技术难点与解决方案

### 难点 1：C++20 大型项目的模块化编译

**挑战：** 10 个 C++ 编译模块，每个有独立的 CMakeLists.txt，涉及 gRPC、Protobuf、curl、hiredis、jsoncpp 等多个 C 库的链接，需要处理跨模块的 include 路径和符号可见性。

**解决：**
- 统一 `CMAKE_CXX_STANDARD 20`（修复 #53：此前各模块标准不一致）
- 每个模块 `include/agent_rpc/<module>/` 公开头文件，`src/` 实现
- 命名空间：`agent_rpc::a2a_adapter`、`agent_rpc::orchestrator`、`agent_rpc::mcp` 等
- `find_package` + `pkg_check_modules` 双路径查找依赖
- WSL2 (Ubuntu) 编译，Windows 终端一键调用：`wsl -d Ubuntu -- bash -c "./run.sh build"`

### 难点 2：gRPC 进程静默崩溃 (SIGSEGV)

**挑战：** `rpc_server` 在处理 AI 查询时静默崩溃，无错误日志，`ECONNRESET`。根因是多线程环境下 curl handler 的并发初始化问题 + A2A HTTP 调用时序问题。

**解决：**
- `curl_global_init(CURL_GLOBAL_ALL)` 在 `main()` 最开始调用（任何线程创建之前）
- `crashHandler()` 注册 SIGSEGV/SIGABRT/SIGFPE/SIGILL 信号处理器
- 信号处理器使用 async-signal-safe 的 `write()` 输出诊断信息
- `BackgroundScheduler` 在 `rpc_server` 停止前先优雅关闭

### 难点 3：Protobuf 跨语言兼容性

**挑战：** C++ 服务端生成的 Protobuf 序列化格式与 Python Agent 端生成的必须完全一致，否则解析失败（proto 字段编号不对齐）。

**解决：**
- 所有 proto 文件放在统一 `proto/` 目录
- C++ 通过 `protoc` + `grpc_cpp_plugin` 生成桩代码
- Python 端使用相同的 `.proto` 文件生成桩代码
- 新增字段一律追加到末尾，避免字段编号冲突
- ServiceInfo 从 8 个字段演进到 12 个字段，始终保持向后兼容

### 难点 4：流式通信的全链路事件同步

**挑战：** Agent SSE 事件 → A2AAdapter → gRPC Server Stream → Node.js Proxy → 前端 ReadableStream 这条链路上，事件可能被吞、乱序或超时。

**解决：**
- AIStreamEvent 定义 7 种事件类型（partial/status/complete/error/plan/subtask_start/subtask_complete）
- `queryStream()` 前端实现：ReadableStream + line-by-line SSE 解析 + 130s 超时 + AbortController
- `processQueryStreaming()`：HTTP 连接重试机制 + 超时控制
- Node.js Proxy 的 `GRPC_TARGET` 环境变量注入
- complete 事件携带 `trace_summary` 和 `activity_json` 供前端展示

### 难点 5：多 Agent 上下文的隔离与继承

**挑战：** 同一会话中切换到不同 Agent 时，新 Agent 不能看到旧 Agent 的完整历史（可能包含无关甚至有害的上下文），但又需要知道之前的对话脉络。

**解决：**
- 对话历史按 `(context_id, agent_id)` 分片隔离（每个 Agent 只看到自己相关的历史）
- 跨 Agent 摘要由 LLM 生成，只保留脉络而非细节
- `handleAgentSwitch()` 在检测到 Agent 变化时触发摘要生成
- `buildSystemContext()` 将三层记忆组装为统一结构注入请求

### 难点 6：全局状态下的线程安全

**挑战：** AgentRouter 的 Agent 列表、CircuitBreaker 的状态计数器、BackgroundScheduler 的任务队列都需要在多线程环境下安全访问。

**解决：**
- AgentRouter: `agents_mutex_` (Agent 列表) + `embedding_mutex_` (向量索引) 双锁保护
- CircuitBreaker: `std::atomic<CircuitState>` + `stats_mutex_` 分离状态和统计
- BackgroundScheduler: `tasks_mutex_` (任务注册) + `queue_mutex_` + cv (任务派发) 双锁设计
- TraceContext: `thread_local` 完全不需同步
- Redis: 单线程模型天然串行

---

## 8. 前端架构

### 8.1 技术选型

| 层级 | 技术选择 | 理由 |
|------|---------|------|
| 框架 | Vue 3 Composition API | 响应式 + 组合式逻辑复用 |
| 类型系统 | TypeScript | Proto 类型映射 + 编译期检查 |
| 构建工具 | Vite | 极速 HMR + ESBuild |
| 状态管理 | Pinia | Vue 3 官方推荐，类型友好 |
| 路由 | Vue Router 4 | SPA 多视图支持 |
| 图表 | Mermaid.js + ECharts | DAG 预览渲染 + 数据可视化（CDN 引入） |
| HTTP 通信 | Fetch API + ReadableStream | gRPC-Web 流式消费 |

### 8.2 视图矩阵

| 视图 | 路由 | 功能 | 后端连接状态 | 对应批次 |
|------|------|------|------------|--------|
| ChatView | `/` | 主对话界面：消息气泡 + 流式渲染 + Trace 摘要 + 点赞/踩 + 导出 + 分享按钮 + 失败重试 | ✅ 已连接 | Batch 1-4, 6 |
| AgentTopology | `/topology` | ECharts 力导向图展示 Agent 网络拓扑关系 | ✅ 已连接 | 扩展 |
| Dashboard | `/dashboard` | 数据可视化面板，Token 消耗趋势、调用排行、成本分布（真实 RPC + 诚实空态） | ✅ 已连接 | 扩展 |
| Monitor | `/monitor` | 系统监控面板，健康概览、延迟分布（真实 RPC，无数据时诚实空态） | ✅ 已连接 | 扩展 |
| AdminView | `/admin` | 管理后台：健康仪表盘 + 预算面板 + 查询重放（Cron/Canary tabs 已删；requiresAdmin 路由守卫，服务端 requireAdmin 为强制边界） | ✅ 已连接 | Batch 5, 6 |
| LoginView | `/login` | 用户注册/登录（Bearer Token 认证，响应携带 role） | ✅ 已连接 | 基础 |
| AgentSandbox | `/sandbox` | Agent 卡片墙 + 快速试用（安全隔离沙箱）+ PROCEED/MODIFY/SKIP 干预决策 | ✅ 已连接 | Batch 7 |
| CompareView | `/compare` | 三列并排对比不同 Agent 回答（CompareAgents 真实并行执行） | ✅ 已连接 | Batch 7 |
| ShareView | `/share/:id` | 只读会话分享（按路由 token 调 ReadSharedConversation，loading/error/empty/result 四态） | ✅ 已连接 | Batch 7 |
| TemplateMarket | `/templates` | 工作流模板市场 + 一键创建真实会话 | ✅ 已连接 | Batch 7 |

**新增视图技术亮点：**

- **AgentTopology**：ECharts 力导向图 + 实时状态轮询 + 拖拽交互，节点颜色/大小反映 Agent 健康状态与负载
- **Dashboard**：ECharts 多图表联动 + countup.js 数字滚动 + Bento Grid 布局，Token 消耗趋势/Agent 调用排行/成本分布一屏尽览
- **Monitor**：健康度 gauge + 断路器状态推断（基于 agent_invocations 表数据） + 告警颜色编码（绿/黄/红三级）

### 8.3 核心组件

| 组件 | 功能 |
|------|------|
| `StreamingText` | 打字机效果渲染流式文本 |
| `MessageBubble` | 消息气泡（用户/Agent 样式 + Trace 摘要 + 反馈按钮） |
| `AgentBadge` | Agent 标签（名称 + 健康状态灯） |
| `AgentCard` | Agent 能力卡片（技能列表 + 延迟指标 + 试用按钮） |
| `AgentSelector` | 多候选 Agent 选择面板（指标对比 + 质量评分） |
| `ActivityPanel` | 右侧活动流面板（💭 思考 → 🔧 工具调用 → ✅ 完成） |
| `ExecutionPlan` | Mermaid DAG 流程图渲染 |

**扩展组件（新增视图配套）：**

| 组件目录 | 组件 | 功能 |
|----------|------|------|
| `dashboard/` | `TokenGauge` | Token 消耗仪表盘（gauge 圆环图） |
| `dashboard/` | `TokenBreakdown` | Token 消耗分布拆解图 |
| `dashboard/` | `CostTracker` | 成本追踪面板（趋势图 + 明细表） |
| `feedback/` | `ToastNotification` | 轻提示通知组件 |
| `feedback/` | `PageLoader` | 页面加载动画 |
| `feedback/` | `EmptyState` | 空状态占位提示 |
| `feedback/` | `ProgressBar` | 进度条组件 |
| `layout/` | `SideNav` | 左侧导航栏（图标 + 路由链接） |
| `layout/` | `GlassCard` | 毛玻璃风格卡片容器 |
| `layout/` | `AppLayout` | 应用整体布局框架 |
| `chat/` | `TypingIndicator` | 打字中动画指示器 |
| `agents/` | `AgentDetailPanel` | Agent 详情面板（指标 + 配置） |
| `agents/` | `AgentStatusCard` | Agent 状态卡片（健康灯 + 摘要） |

### 8.4 gRPC-Web 通信

前端通过手写 TypeScript 类型（[proto.ts](frontend/src/types/proto.ts)）+ Fetch API 与后端通信，而非使用 `protoc-gen-grpc-web`。这是有意为之：

- 避免引入 gRPC-Web 生成代码的复杂性
- JSON 序列化可直接在网络面板调试
- Auth token 通过 `Authorization: Bearer <token>` header 自动注入
- 流式通过 `ReadableStream` + SSE 行解析实现
- 401 自动触发登出

---

## 9. 测试与质量保障体系

### 9.1 测试金字塔

```
              ┌──────────────┐
              │  Release E2E  │ tests/e2e/e2e_pr_g_release.py
              │              │ 25 个真实断言（真实 rpc_server + Docker PG/Redis
              │              │ + 真实 HTTP mock A2A agent，psql 直查 PG）
              ├──────────────┤
              │  网关契约测试  │ gateway/proxy npm test：116 例
              │              │ 错误映射/proto 类型防漂移/前端守卫/平台契约
              ├──────────────┤
              │  CTest 套件   │ 37 套（WSL，真实 PG/Redis，无 repository mock）
              ├──────────────┤
              │  属性测试     │ RapidCheck（路由确定性/序列化往返等）
              ├──────────────┤
              │  verify 场景  │ ./run.sh verify：8 批次自动化验证
              └──────────────┘
```

### 9.2 CTest 测试套件（37 套，代表性）

```bash
# WSL 内，需先导出 NEXUSAI_POSTGRES_* 与 REDIS_HOST/REDIS_PORT（缺失时 PG 用例按契约 GTEST_SKIP，非伪绿）
ctest --output-on-failure
```

关键套件（均基于真实 PostgreSQL/Redis，不 mock repository）：

| 套件 | 覆盖 |
|------|------|
| `DurableQueryPipelineTest` | 六步管线：owner 防伪造、重试幂等、预算拒绝落库、五种终态、终态恰好一次、并发首建 |
| `QueryDomainRepositoryContractTest` | 仓储契约：owner-scoped update 无 upsert、消息 sequence 并发 |
| `PostgresBudgetRepositoryContractTest` | 预算 reserve/幂等/跨 owner 拒绝 |
| `AgentRuntimeRepositoryContractTest` | 注册/心跳/反馈/observability 落 PG，源码静态守卫（无 popen/psql/PG_URL） |
| `DurableWorkflowsContractTest` | Replay/Export/Share/Template 全链路 |
| `WorkflowControlContractTest` | Sandbox/Compare/Intervention/Undo/Autonomy |
| 既有套件 | test_agent_router_properties / test_a2a_integration / test_mcp_integration / test_redis_services / test_proto_roundtrip 等 |

### 9.3 网关契约与 Release E2E

```bash
# 网关契约（Windows 或任意 Node 环境）
cd gateway/proxy && npm test      # 98/98 pass：错误映射、proto 类型防漂移、前端守卫、平台契约

# Release E2E（WSL，真实进程级）
python3 tests/e2e/e2e_pr_g_release.py   # 25/25 断言：注册/登录/ADMIN、QueryStream 恰好一次 complete、
                                        # 预算拒绝落库、跨 owner NotFound、反馈聚合、分享 TTL+撤销
```

`verify/` 目录另保留 8 批次自动化场景：

```bash
./run.sh verify          # 一键运行全部批次验证
./run.sh verify-batch1   # 单独运行某批次
```

Mock Agent (`verify/mock-agent/mock_agent_server.py`) 模拟多种行为模式（normal/slow/error/delegate/version_v1_0/version_v1_1/version_mixed），覆盖正向流程、超时、熔断、委派、版本协商等场景。

---

## 10. 部署与运维

### 10.1 Docker Compose 一键部署（五服务）

```bash
docker compose up --build -d
docker compose ps          # postgres / redis / rpc-server / proxy / frontend 全部 healthy
```

- 浏览器入口：**http://localhost:8080**（frontend 生产端口；注意不是开发态的 5173，也不是 proxy 的 8081）
- 健康探活：`curl http://127.0.0.1:8080/health` 返回 OK
- rpc-server 启动时自动执行 `db/migrations` 迁移（`NEXUSAI_MIGRATIONS_DIR`），迁移失败即中止启动；compose 中不存在独立的 migrate 服务
- 受限网络下可用 `APT_MIRROR_PREFIX` 构建参数替换 apt 源（默认行为不变）

### 10.2 本地开发启动（run.sh 统一入口）

```bash
# WSL 终端（后端）
./run.sh build && ./run.sh start-all

# Windows 终端（前端开发服务器）
cd frontend && npm run dev

# 访问
# 前端: http://localhost:5173
# gRPC: localhost:50051
```

`run.sh` 主要命令：`setup`（环境检查）/ `build` / `test` / `start` / `start-all` / `stop` / `redis` / `gateway` / `frontend-dev` / `frontend-build` / `start-mock-agent` / `start-orchestrator` / `verify` / `clean`。

### 10.3 启动顺序（run.sh start-all 自动化编排）

```
1. Redis (:6379)            — 存储层最先启动
2. Mock Agent (:5100)       — 验证用 Agent
3. gRPC Server (:50051)     — 核心服务（在 Orchestrator 之前启动以接受注册）
4. Node gRPC Proxy (:8081)  — gRPC→SSE 协议转换（主力网关）
5. Orchestrator (:5000)     — A2A 路由代理（最后启动，注册到 gRPC Server）
6. Agent Registration       — 自动注册 Mock Agent 到平台
```

### 10.4 配置管理

所有可调参数通过 `.env` 文件 + `env_loader` 统一管理，不硬编码：

```ini
LLM_API_KEY=sk-xxx           # LLM API Key
LLM_MODEL=deepseek-v4-flash  # LLM 模型名（代码兜底值一致）
LLM_API_URL=https://api.deepseek.com

RPC_SERVER_PORT=50051        # gRPC 监听端口
ORCHESTRATOR_URL=http://localhost:5000

ROUTING_HIGH_THRESHOLD=0.85  # Embedding 高置信度阈值
ROUTING_LOW_THRESHOLD=0.50   # Embedding 低置信度阈值

# PostgreSQL 持久化预算（代码内默认值同 .env.example）
NEXUSAI_BUDGET_GLOBAL_TOKENS=0        # 0 = 无限
NEXUSAI_BUDGET_USER_DAILY_TOKENS=200000
NEXUSAI_BUDGET_USER_MONTHLY_TOKENS=4000000
NEXUSAI_BUDGET_SESSION_TOKENS=100000

# ADMIN 账号（RegisterAgent/UnregisterAgent 仅限该用户）
NEXUSAI_ADMIN_USERNAME=
```

### 10.5 跨平台支持

| **C++ 后端**：Ubuntu/WSL2（通过 `wsl -d Ubuntu -- bash -c "..."` 从 Windows 一键调用）
| **前端**：跨平台（Node.js + Vite）
| **网关**：Node.js Proxy 跨平台；Nginx + Envoy Docker 方案跨平台（可选） |

---

## 11. 求职竞争力分析

### 11.1 项目展示的技术深度

| 技术领域 | 具体体现 | 求职市场价值 |
|----------|---------|-------------|
| **C++ 系统工程** | 10 模块 CMake 构建、C++20 标准、模板元编程、RAII、智能指针管理生命周期 | 后端/基础设施/高频交易岗位刚需 |
| **分布式系统** | gRPC 通信、Proto 序列化、服务注册发现、熔断降级、负载均衡 | 分布式/微服务岗位核心技能 |
| **网络协议** | A2A JSON-RPC 自主实现、HTTP/SSE 流式通信、gRPC-Web 协议转换 | 体现协议设计能力 |
| **AI/LLM 工程** | LLM Prompt 工程、Embedding 向量化、RAG 检索、DAG 任务编排 | AI 工程化领域最抢手技能 |
| 数据库设计 | PostgreSQL 为事实源（db/migrations V001–V015 只追加），Redis 仅缓存，JSONB 灵活存储 | 数据密集型应用设计能力 |
| **前端工程** | Vue 3 + TS + Vite SPA、流式渲染、Pinia 状态管理 | 全栈能力证明 |
| **并发编程** | 线程安全数据结构、std::async 并行执行、原子操作、死锁预防 | C++ 高级岗位必备 |
| **测试工程** | GTest + RapidCheck 属性测试、真实 PG 契约测试、Release E2E、网关契约防漂移 | 工程素养体现 |

### 11.2 面试叙事建议

**一句话概述：**
> "我从零设计并实现了一个 C++20 多 Agent 协作平台，包含四层智能路由、DAG 任务编排、RAG 工具检索、A2A 网络协议通信与以 PostgreSQL 为事实源的 Durable Query Pipeline，历经 8 批次 + 7-PR 持久化大优化迭代至 37 套测试全绿。"

**STAR 框架话术：**

- **Situation**：现有 LLM Agent 框架（LangChain 等）存在三个瓶颈 —— 每次请求都调 LLM 做意图分类（600ms+ 延迟）、工具全量传入导致上下文溢出、Agent 间缺乏标准化的网络通信协议
- **Task**：设计一个高性能、可扩展的多 Agent 通信框架，实现 Agent 自动发现、智能路由、任务编排、工具选择和记忆管理
- **Action**：
  - 用 C++20 实现核心引擎（路由/编排/适配），gRPC 做内部通信，A2A 协议做 Agent 间通信
  - 设计四层渐进式路由将 P50 延迟从 600ms 降到 < 100ms
  - 实现 DAG 拓扑排序 + 并行执行引擎，单 Agent 走快速路径
  - 引入 RAG-MCP 向量检索解决工具上下文溢出
  - 完整 Vue 3 前端 + Docker 网关 + E2E 验证体系
- **Result**：48+ 次迭代，10 个 C++ 编译模块，8 批次 21 项优化 + 批次八 14 项 + 7-PR PostgreSQL 大优化，37 套 CTest 全绿，网关契约 116 例，真实 PG E2E 25 断言

### 11.3 适用岗位

| 岗位 | 匹配点 |
|------|--------|
| **后端/基础设施工程师** | C++20、CMake、gRPC、分布式系统设计 |
| **AI 平台工程师** | LLM 集成、RAG、Agent 编排、MCP 协议 |
| **全栈工程师** | C++ 后端 + Vue 3 前端 + Docker 部署 |
| **系统架构师** | 模块化设计、协议设计、8 批次工程规划 |

### 11.4 高频面试追问 — 提前准备的答案

#### Q1: "为什么用 C++ 而不是 Python/Go？"

**错误回答：** "因为我会 C++。"（面试官想听的不是语言偏好，是工程决策）

**正确回答：** "三个原因。第一，性能——路由决策在热路径上，C++ 的 Embedding 向量匹配可以做到 < 1ms（内存缓存命中时），Python 同样的操作要 5-10ms。第二，资源控制——gRPC 服务端需要精确控制线程池、内存分配和连接池，C++ 的 RAII 和智能指针让资源管理更可靠。第三，也是最重要的——我做这个项目的目标之一是**证明我能用 C++ 做应用层系统**，不只是算法题和 LeetCode。大多数 C++ 候选人的项目经历是 STL 容器和算法，我展示的是用 C++ 设计一个完整的分布式系统。"

#### Q2: "为什么不直接用 OpenAI 的 Assistants API？"

**错误回答：** "我想自己实现。"（太弱）

**正确回答：** "OpenAI Assistants API 解决的是'一个 Agent 怎么工作'的问题——我给你一个 prompt 和一个工具列表，你帮我执行。NexusAI 解决的是'多个 Agent 怎么协作'的问题——Agent 怎么被发现、怎么被路由、怎么被编排、怎么被治理。这是两个不同的抽象层级。Assistants API 可以成为 NexusAI 平台上的**一个 Agent 实现**——它注册到平台、声明自己的 skill、通过 A2A 协议被其他 Agent 调用。平台是 Agent 的'操作系统'，不是 Agent 的替代品。另外，锁定单一厂商的 API 有供应商风险——NexusAI 的 Agent 可以背后用 OpenAI、Anthropic、DeepSeek 或本地模型，平台不关心。"

#### Q3: "这个项目跟 LangChain 的 AgentExecutor 有什么区别？"

**错误回答：** "LangChain 太慢了。"（太模糊）

**正确回答：** "最本质的区别是通信模型。LangChain 的 AgentExecutor 是**进程内函数调用**——Agent A 调 Agent B 是 `agent_b.invoke(input)`，这在 Python 运行时内部完成。NexusAI 是**网络协议通信**——Agent A 调 Agent B 是 HTTP POST 一个 JSON-RPC 请求到 Agent B 的 URL。函数调用 vs 网络通信，这是架构层面的根本差异。函数调用的好处是简单，代价是：(1) 所有 Agent 必须同语言同进程，(2) 一个 Agent OOM 全部挂，(3) 无法跨机器扩展，(4) 不同团队开发的 Agent 无法互操作。网络通信的好处正好反过来。我的判断是——当 Agent 生态成熟到跨组织协同时，网络协议是唯一可行的方案。就像今天的微服务不会回到单体。"

#### Q4: "你开发过程中遇到的最难 bug 是什么？"

**错误回答：** "有个空指针调了很久。"（没有展现能力）

**正确回答：** "最有价值的 bug 是 gRPC Server 静默崩溃问题。现象是 rpc_server 在处理 AI 查询时突然消失，没有错误日志、没有 core dump、没有异常——进程直接没了。排查路径：(1) 先确认崩溃触发条件——只有走 A2A adapter 的流式查询崩溃，同步查询正常；(2) 在 `main()` 注册 SIGSEGV/SIGABRT 信号处理器，用 async-signal-safe 的 `write()` 输出崩溃信号名到 stderr；(3) 发现崩溃时信号是 SIGSEGV，且总是在 `processQueryStreaming()` 的 HTTP 调用附近；(4) 根因定位——多线程环境下 curl handler 被多个模块独立初始化（registry、mcp、a2a 三处各自调 `curl_global_init`），libcurl 文档明确说这是未定义行为；(5) 修复——在 `main()` 最开始（任何线程创建前）调用一次 `curl_global_init(CURL_GLOBAL_ALL)`，移除其他模块的调用。这个 bug 教会我：第三方库的全局状态管理在并发环境下的重要性，以及为什么'看起来能跑'不代表'正确'。"

#### Q5: "如果重新做这个项目，你会怎么做？"

**错误回答：** "我觉得现在挺好的。"（缺乏反思能力）

**正确回答：** "三个改进方向。第一，测试先行——我在 Batch 1 之后才建立了 E2E 验证体系，应该在写第一行代码前就定义'什么叫项目可用'的验收标准。前期的很多 bug（A2A 响应解析、字段名不匹配）如果当时有 E2E 测试，可以在 5 分钟内发现而非几小时后。第二，A2A 协议版本协商应该在第一版就考虑——v1.0 的 `kind` vs v1.1 的 `type` 这个不兼容问题是后期才暴露的，如果一开始设计 `ServiceInfo` 时就加入 `a2a_version` 字段，可以避免兼容处理代码。第三，前端不应该手写 Protobuf TypeScript 类型——应该用 `protoc-gen-grpc-web` 或 `buf` generate 自动生成，手动维护容易出现字段命名不一致（proto 的 `data` vs 前端的 `content`）。这三个改进都指向同一个原则：**在项目初期投资基础设施（测试、版本协商、代码生成），比后期修补更高效。**"

#### Q6: "这个项目的商业价值在哪？谁会愿意付费？"

**错误回答：** "有 Agent 需求的企业。"（太泛）

**正确回答：** "三类客户。第一，有多条 AI 产品线的中型企业——他们有多个团队在做不同的 Agent（客服、推荐、风控），需要一个统一的平台来管理、路由和监控这些 Agent。类比：微服务多了需要 Spring Cloud/K8s，Agent 多了就需要 NexusAI。第二，想做 AI Agent 市场的平台方——类似 Salesforce AppExchange 但 for Agent。NexusAI 的服务注册 + AgentCard + 路由发现 + 反馈系统天然构成一个 Agent Marketplace 的后端。第三，MCP 工具服务商——他们需要让工具被多个 Agent 平台发现和使用，RAG-MCP 的工具检索 + 语义缓存直接解决了'工具太多怎么让 Agent 选对'的问题。商业化的路径不是卖框架本身，而是卖托管平台——Agent 开发者注册、发布、被调用、按用量计费，平台抽成。"

---

## 12. 项目展望

### 12.1 短期计划

- [ ] 支持 LLM_API_URL 指向的自定义网关（当前已接入 DeepSeek 兼容接口）
- [ ] 实现更多专业 Agent（Code Review Agent、Data Analysis Agent）
- [ ] 按 provider 实际 usage 回填精确 Token 用量，替代 estimated 记账
- [ ] ProfileSummarizer 用户画像持久化到 PostgreSQL
- [ ] 前端 DAG 编辑器的拖拽式交互

### 12.2 中期计划

- [ ] 实现 Agent Marketplace（Agent 发布/发现/评价/安装）
- [ ] 多租户隔离（组织级 Agent 管理 + 权限控制）
- [ ] WebSocket 升级（替代当前 SSE，支持双向实时通信）
- [ ] Agent 性能基准测试框架（Benchmark Suite）

### 12.3 长期愿景

构建一个**开放的 AI Agent 互联网**，让不同组织开发的 Agent 能通过标准化协议（A2A）互相发现、通信、协作——如同 SMTP 连接了全球的邮件服务器，HTTP 连接了全球的 Web 服务。

---

## 附录

### A. 项目文件结构速览

```text
agent-communication-and-tool-selection-framework/
├── proto/                    # 9 个 Proto 定义文件
├── common/                   # 共享基础设施 (Logger, Redis, CircuitBreaker, etc.)
├── registry/                 # Agent 服务注册中心
├── a2a/                      # A2A 协议 C++ 客户端库
├── a2a_adapter/              # gRPC ↔ A2A 双向适配器
├── orchestrator/             # AgentRouter + TaskPlanner + TaskExecutor
├── mcp/                      # MCP 客户端 + RAG-MCP 工具检索
├── server/                   # gRPC 服务端 (9 个 Service)
├── client/                   # gRPC 交互式 CLI 客户端
├── frontend/                 # Vue 3 + TypeScript SPA
├── gateway/                  # Nginx + Envoy + Node.js Proxy
├── tests/                    # 37 套测试 (GTest + RapidCheck + 真实 PG 契约)
│   └── e2e/                  #   Release E2E（e2e_pr_g_release.py）
├── verify/                   # E2E 验证脚本 + Mock Agent
├── examples/                 # Orchestrator + 示例 Agent
├── docs/                     # 项目文档 (guides/, interview/)
├── db/migrations/            # PostgreSQL 迁移 V001–V015（只追加）
├── sql/                      # 早期 schema 参考脚本（运行时迁移以 db/migrations 为准）
├── run.sh                    # 统一管理脚本（build/test/start-all/stop/verify 等）
├── docker-compose.yml        # 五服务编排（postgres/redis/rpc-server/proxy/frontend）
├── scripts/                  # 辅助脚本
├── CMakeLists.txt            # 根 CMake 配置
└── CLAUDE.md                 # 项目开发指南
```

### B. 关键参考文档

- [项目启动指南](guides/startup-guide.md)
- [Agent 接入指南](../agent-integration-guide.md)
- [Durable Query Pipeline 指南](guides/durable-query-pipeline-guide.md)
- [工作流控制指南](guides/workflow-control-guide.md)
- [分享与资产指南](guides/sharing-and-assets-guide.md)
- [A2A 协议指南](guides/a2a-protocol.md)
- [部署指南](guides/deployment.md)

---

> **项目地址：** `github.com/dudu-scut/agent-communication-and-tool-selection-framework`
>
> **技术栈：** C++20 · gRPC · Protobuf · A2A Protocol · MCP · RAG · Vue 3 · Redis · PostgreSQL 16（持久事实源） · Docker
>
> **开发周期：** 48+ 次迭代，8 批次增量优化 + 7-PR PostgreSQL 大优化，37/37 CTest 全绿，网关契约 116 例，Release E2E 25 断言
