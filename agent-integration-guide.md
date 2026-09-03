# NexusAI Agent 接入指南（gRPC 路径）

本文档面向需要将自有 Agent 接入 NexusAI 平台的开发者。接入后，平台的路由引擎会自动发现你的 Agent，并根据用户请求的技能匹配将其分派给你的 Agent 处理。

整体接入分三步理解：**对外暴露 A2A HTTP 接口**（平台调用你的 Agent），**登录平台取得管理员令牌**（注册是 ADMIN-only 能力），**通过 gRPC 向平台注册**（平台发现你的 Agent）。

> 本文与 7-PR 大优化后的现状一致：注册信息持久化在 PostgreSQL 的 `agent_registry` 表，Redis 仅承担短时存活缓存（`agent:liveness:<agent_id>`）；平台事实源是 PostgreSQL，而非内存或 Redis。

---

## 架构总览

```
                          ┌─────────────────────────────────────────┐
                          │            NexusAI 平台                  │
                          │                                         │
  用户请求 ──► RPC Server ──► AgentRouter ──► A2AAdapter ──HTTP──►  你的 Agent
               ▲              (路由/匹配)     (协议转换)             │
               │                                                    │
               └── gRPC ◄── RegisterAgent / Heartbeat ◄──────────────┘
                          │                                         │
                          └─────────────────────────────────────────┘
```

你的 Agent 需要承担两个角色：

1. **A2A HTTP Server**：监听一个 HTTP 端口，接受平台发来的 JSON-RPC 请求（`message/send` 和 `message/stream`），执行任务并返回结果。
2. **gRPC Client**：主动连接 NexusAI 的 gRPC Server（默认端口 50051），携带管理员令牌完成注册、心跳维持和注销。

注册成功后，AgentRouter 会将你的 Agent 的 skills 纳入路由匹配范围。当用户请求命中你的 Agent 的技能时，平台通过 A2A 协议调用你。

### 认证要求（重要变化）

`RegisterAgent` 与 `UnregisterAgent` 是 **ADMIN-only** 能力：服务端通过 `AuthInterceptor::requireAdmin()` 拦截，调用方必须携带角色为 `ADMIN` 的登录令牌，否则返回 `UNAUTHENTICATED`（未登录）或 `PERMISSION_DENIED`（角色不足）。ADMIN 身份由服务端环境变量 `NEXUSAI_ADMIN_USERNAME` 指定——注册用户名与之匹配时，`agent_communication.auth.UserService/Register` 会颁发 `ADMIN` 角色。

因此接入流程先要经过认证服务：

| RPC（服务 `agent_communication.auth.UserService`） | 用途 |
|------|------|
| `Register` | 注册平台账号（用户名匹配 `NEXUSAI_ADMIN_USERNAME` 时获得 ADMIN 角色） |
| `Login` | 登录换取 token |
| `ValidateToken` | 校验 token 有效性 |

取得 token 后，将其放入 gRPC metadata：`authorization: Bearer <token>`（经 HTTP 网关调用时则是请求头 `Authorization: Bearer <token>`）。仓库中的 `scripts/register_agents.sh` 就是这套流程的参考实现：先调 `agent_communication.auth.UserService/Login` 取 token，再带 `Authorization: Bearer` 调 `RegisterAgent`。

---

## 前置条件

### 网络要求

- 你的 Agent 能被 NexusAI 平台通过 HTTP 访问到（平台 → Agent 方向）。
- 你的 Agent 能访问 NexusAI 的 gRPC Server（Agent → 平台方向，默认端口 50051）。
- 你持有匹配 `NEXUSAI_ADMIN_USERNAME` 的平台账号（注册/注销为 ADMIN-only）。

### Proto 文件

从 NexusAI 仓库获取以下两个 proto 文件，用于生成 gRPC 客户端代码：

```
proto/
├── common.proto          # Status、ServiceInfo 等公共消息
└── agent_service.proto   # AgentCommunicationService 服务定义
```

使用 protoc 生成 Python 桩代码：

```bash
python -m grpc_tools.protoc -I proto/ \
    --python_out=. --grpc_python_out=. \
    proto/common.proto proto/agent_service.proto
```

---

## 第一步：实现 A2A HTTP 接口

NexusAI 平台通过 A2A（Agent-to-Agent）协议调用你的 Agent。你需要暴露一个 HTTP 端点，处理以下两个 JSON-RPC 方法。

### 端点规范

所有请求发往同一个 URL（例如 `http://your-agent:8080/`），Content-Type 为 `application/json`。

### message/send（同步调用）

**请求体：**

```json
{
  "jsonrpc": "2.0",
  "method": "message/send",
  "id": "req-uuid",
  "params": {
    "message": {
      "message_id": "msg-uuid",
      "context_id": "conversation-uuid",
      "role": "user",
      "parts": [
        { "type": "text", "text": "帮我解一道方程" }
      ]
    },
    "history_length": 10,
    "context_id": "conversation-uuid"
  }
}
```

**响应体（成功）：**

```json
{
  "jsonrpc": "2.0",
  "id": "req-uuid",
  "result": {
    "type": "message",
    "message": {
      "message_id": "resp-uuid",
      "context_id": "conversation-uuid",
      "role": "agent",
      "parts": [
        { "type": "text", "text": "方程的解是 x = 42" }
      ]
    }
  }
}
```

响应中的 `result.type` 可以是 `"message"`（直接回复）或 `"task"`（异步任务）。对于大多数同步 Agent，返回 `"message"` 即可。

### message/stream（流式调用）

**请求体**与 `message/send` 相同，method 为 `"message/stream"`。

**响应格式**为 Server-Sent Events（SSE），每行以 `data: ` 开头：

```
data: {"jsonrpc":"2.0","id":"req-uuid","result":{"type":"status","status":{"state":"working","message":{"message_id":"m1","role":"agent","parts":[{"type":"text","text":"正在计算"}]}}}}

data: {"jsonrpc":"2.0","id":"req-uuid","result":{"type":"status","status":{"state":"completed","message":{"message_id":"m2","role":"agent","parts":[{"type":"text","text":"x = 42"}]}}}}
```

SSE 事件类型通过 `result.type` 区分：`status`（状态更新）、`artifact`（中间产物）、`message`（最终回复）。流结束时关闭 HTTP 连接。

### 错误响应

```json
{
  "jsonrpc": "2.0",
  "id": "req-uuid",
  "error": {
    "code": -32603,
    "message": "内部处理失败",
    "data": { "detail": "调用 LLM 超时" }
  }
}
```

常用错误码：`-32600`（请求格式错误）、`-32601`（方法不存在）、`-32602`（参数无效）、`-32603`（内部错误）。

---

## 第二步：提供 AgentCard

AgentCard 是一段 JSON 元数据，描述你的 Agent 的能力，供平台路由引擎匹配。你需要在注册时通过 `ServiceInfo.agent_card` 字段传递它，同时在 HTTP 端点上暴露 `/.well-known/agent-card.json` 供平台拉取（可选，但推荐）。

### AgentCard Schema

```json
{
  "name": "math-agent",
  "description": "数学计算与方程求解 Agent",
  "url": "http://your-agent:8080/",
  "version": "1.0.0",
  "skills": [
    {
      "name": "math",
      "description": "通用数学计算，包括算术、代数、微积分"
    },
    {
      "name": "equation",
      "description": "方程求解，支持一元/多元方程"
    }
  ]
}
```

### 路由匹配机制

AgentRouter 使用四级路由策略，你的 Agent 能否被选中取决于 skills 字段：

1. **Embedding 匹配**：将用户请求与所有 Agent 的 skills 做向量相似度对比，高置信度直接选中。
2. **LLM 意图分类**：调用大模型判断用户意图属于哪个 skill。
3. **关键词 IDF 匹配**：提取请求中的关键词，与 skill 名称做 IDF 加权匹配。
4. **兜底**：以上均未命中时，选择健康状态的通用 Agent。

因此，**skills 的 name 和 description 写得越精确，路由命中率越高**。避免使用过于宽泛的描述（如"处理各种任务"），应当具体到实际能力域。

**反馈驱动路由（Batch 2，已随 7-PR 优化迁入 PostgreSQL）：** 在上述四级路由基础上，当同一 skill 有多个候选 Agent 时，平台会根据历史用户反馈（点赞/点踩）计算质量系数，对 Agent 做加权随机选择。质量系数范围 [0.5, 1.0]，点赞率越高的 Agent 被选中概率越大。这意味着：
- 持续提供高质量响应的 Agent 会获得更多流量
- 点踩率高的 Agent 会被自动降权
- 你可以在 `AgentMetrics.approval_rate` 中查看自己的质量评分

优化后的两个关键事实：
- **质量数据按用户维度隔离**：反馈聚合写入 PostgreSQL 的 `agent_route_quality` 表，唯一键为 `(owner_id, agent_id, skill_name)`——即每个用户对每个 Agent 每个技能都有独立的质量画像，不同用户的路由加权互不干扰。
- **小样本保护**：样本量不足时采用 Beta(2,2) 先验平滑（好评率按 `(positive + 2) / (total + 4)` 估计），避免低样本 Agent 被推到 0 或 1 的极端权重。

---

## 第三步：通过 gRPC 注册

注册使用 `AgentCommunicationService.RegisterAgent` RPC，调用必须携带 ADMIN 令牌（见上文“认证要求”）。注册成功后，你的 Agent 会出现在 AgentRouter 的候选列表中，同时平台会把注册信息持久化到 PostgreSQL 的 `agent_registry` 表（注册表行在注销时不删除，仅标记 offline，历史与指标保持可查询）。

### RegisterAgent RPC

**请求：**

```protobuf
message RegisterAgentRequest {
    common.ServiceInfo agent_info = 1;   // Agent 元信息
    int32 heartbeat_interval = 2;        // 心跳间隔（秒），建议 15
}
```

**ServiceInfo 字段说明：**

| 字段 | 必填 | 说明 |
|------|------|------|
| `service_name` | 是 | Agent 唯一名称，如 `"math-agent"` |
| `version` | 是 | 版本号，如 `"1.0.0"` |
| `host` | 是 | Agent 的 HTTP 监听地址，如 `"192.168.1.100"` |
| `port` | 是 | Agent 的 HTTP 监听端口，如 `8080` |
| `skills` | 是 | 技能名称列表，与 AgentCard 中的 skills.name 一致 |
| `tags` | 否 | 标签列表，用于分类筛选 |
| `metadata` | 否 | 键值对扩展信息 |
| `agent_card` | 否 | 完整的 AgentCard JSON 字符串 |
| `agent_metrics` | 否 | 运行时指标（成功率、延迟、token 估算等），心跳时可更新 |
| `cacheable` | 否 | 响应是否可被语义缓存复用（Batch 3） |
| `deployment_stage` | 否 | 部署阶段：`STABLE` / `CANARY` / `DEPRECATED`（Batch 6） |
| `a2a_version` | 否 | A2A 协议版本，如 `"1.0"`、`"1.1"`（Batch 8） |

**响应：**

```protobuf
message RegisterAgentResponse {
    common.Status status = 1;          // code=0 表示成功
    string agent_id = 2;               // 平台分配的唯一 Agent ID
    int64 registration_time = 3;       // 注册时间戳
}
```

注册成功后请保存 `agent_id`，后续心跳和注销都需要用到它。`agent_id` 的生成规则是 `<service_name>-<host>-<port>` 拼接，因此同一三元组的重复注册会更新已有记录而非报错。

---

## 第四步：维持心跳

注册成功后，你需要每隔 `heartbeat_interval` 秒向平台发送一次心跳，告知平台你的 Agent 仍然存活。平台按 `max(3 × heartbeat_interval, 300秒)` 计算存活窗口（即至少容忍连续 3 次心跳丢失，且下限 5 分钟）；窗口内未收到心跳，你的 Agent 会被移出路由候选。

心跳在服务端做两件事：

1. **刷新 PostgreSQL 的 `agent_registry` 记录**（upsert 语义）：如果注册时恰逢 PostgreSQL 短暂不可用导致注册表行丢失，心跳会自愈补写；
2. **刷新 Redis 存活缓存**（`SETEx agent:liveness:<agent_id>`）：仅作为短 TTL 的存活判断，不承担事实存储。

### Heartbeat RPC

```protobuf
message HeartbeatRequest {
    string agent_id = 1;               // 注册时获得的 agent_id
    common.ServiceInfo agent_info = 2; // 可选，用于心跳时同步更新 Agent 元数据
}

message HeartbeatResponse {
    common.Status status = 1;          // code=0 表示成功
    int64 server_time = 2;             // 服务器时间戳
}
```

**实现建议：** 用一个独立的后台线程/协程执行心跳循环，避免阻塞业务逻辑。心跳间隔建议 15 秒（存活窗口 = max(45, 300) = 300 秒，容错余量充足）。心跳时可以附带 `agent_info` 字段来动态更新运行时指标（成功率、延迟等），让平台路由引擎获取最新数据。

---

## 第五步：优雅注销

Agent 下线前应主动调用 `UnregisterAgent`（同样需要 ADMIN 令牌），让平台及时更新路由表，避免将请求分发到已下线的 Agent。注销时平台会将 `agent_registry` 行标记为 offline（保留历史），并删除 Redis 存活缓存。

### UnregisterAgent RPC

```protobuf
message UnregisterAgentRequest {
    string agent_id = 1;               // 注册时获得的 agent_id
    string reason = 2;                 // 注销原因，如 "shutdown"、"maintenance"
}

message UnregisterAgentResponse {
    common.Status status = 1;
    int64 unregistration_time = 2;
}
```

---

## 完整接入示例（Python）

以下是一个完整的 Python Agent 接入示例，包含 A2A HTTP Server、gRPC 注册、心跳维持和优雅注销。

### 安装依赖

```bash
pip install grpcio grpcio-tools flask requests
```

### 生成 Proto 桩代码

```bash
# 在项目根目录执行
python -m grpc_tools.protoc -I proto/ \
    --python_out=. --grpc_python_out=. \
    proto/common.proto proto/agent_service.proto
```

执行后会在当前目录生成 `common_pb2.py`、`agent_service_pb2.py` 和 `agent_service_pb2_grpc.py`。

### 完整代码

```python
"""
math_agent.py — 接入 NexusAI 平台的数学 Agent 示例

功能:
  1. 启动 HTTP Server 处理 A2A 请求（message/send 和 message/stream）
  2. 通过 gRPC 向平台注册，维持心跳，退出时注销
"""

import json
import signal
import sys
import threading
import time
import uuid

import grpc
from flask import Flask, request, jsonify, Response

# Proto 生成的桩代码
import common_pb2
import agent_service_pb2
import agent_service_pb2_grpc

# ============================================================================
# 配置
# ============================================================================

AGENT_NAME = "math-agent"
AGENT_VERSION = "1.0.0"
AGENT_HOST = "192.168.1.100"       # 你的 Agent IP，平台通过此地址访问你
AGENT_PORT = 8080                   # HTTP 监听端口
GRPC_SERVER = "nexusai-platform:50051"  # NexusAI 平台 gRPC 地址
HTTP_GATEWAY = "http://nexusai-platform:8081"  # HTTP 网关（用于登录取 token）
HEARTBEAT_INTERVAL = 15             # 心跳间隔（秒）

# 管理员账号（用户名须与服务端 NEXUSAI_ADMIN_USERNAME 一致，注册/注销是 ADMIN-only）
ADMIN_USERNAME = "admin"
ADMIN_PASSWORD = "change-me"

AGENT_CARD = {
    "name": AGENT_NAME,
    "description": "数学计算与方程求解 Agent",
    "url": f"http://{AGENT_HOST}:{AGENT_PORT}/",
    "version": AGENT_VERSION,
    "skills": [
        {"name": "math", "description": "通用数学计算，包括算术、代数、微积分"},
        {"name": "equation", "description": "方程求解，支持一元/多元方程"},
    ],
}

# ============================================================================
# A2A HTTP Server（Flask）
# ============================================================================

app = Flask(__name__)

@app.route("/.well-known/agent-card.json")
def agent_card():
    """暴露 AgentCard，供平台拉取元数据（可选但推荐）"""
    return jsonify(AGENT_CARD)

@app.route("/", methods=["POST"])
def handle_a2a():
    """处理 A2A JSON-RPC 请求"""
    body = request.get_json()
    method = body.get("method", "")
    req_id = body.get("id", "")
    params = body.get("params", {})

    if method == "message/send":
        return handle_message_send(req_id, params)
    elif method == "message/stream":
        return handle_message_stream(req_id, params)
    else:
        return jsonify({
            "jsonrpc": "2.0",
            "id": req_id,
            "error": {"code": -32601, "message": f"未知方法: {method}"},
        }), 400

def handle_message_send(req_id, params):
    """同步处理消息并返回结果"""
    user_text = extract_user_text(params)

    # ---- 你的 Agent 业务逻辑 ----
    answer = process_query(user_text)
    # ------------------------------

    return jsonify({
        "jsonrpc": "2.0",
        "id": req_id,
        "result": {
            "type": "message",
            "message": {
                "message_id": str(uuid.uuid4()),
                "context_id": params.get("context_id", ""),
                "role": "agent",
                "parts": [{"type": "text", "text": answer}],
            },
        },
    })

def handle_message_stream(req_id, params):
    """流式处理消息，返回 SSE"""
    user_text = extract_user_text(params)

    def generate():
        # 发送 working 状态
        working_event = {
            "jsonrpc": "2.0", "id": req_id,
            "result": {
                "type": "status",
                "status": {
                    "state": "working",
                    "message": {
                        "message_id": str(uuid.uuid4()),
                        "role": "agent",
                        "parts": [{"type": "text", "text": "正在处理..."}],
                    },
                },
            },
        }
        yield f"data: {json.dumps(working_event)}\n\n"

        # 执行实际处理
        answer = process_query(user_text)

        # 发送 completed 状态
        completed_event = {
            "jsonrpc": "2.0", "id": req_id,
            "result": {
                "type": "status",
                "status": {
                    "state": "completed",
                    "message": {
                        "message_id": str(uuid.uuid4()),
                        "role": "agent",
                        "parts": [{"type": "text", "text": answer}],
                    },
                },
            },
        }
        yield f"data: {json.dumps(completed_event)}\n\n"

    return Response(generate(), mimetype="text/event-stream")

def extract_user_text(params):
    """从 MessageSendParams 中提取用户文本"""
    message = params.get("message", {})
    parts = message.get("parts", [])
    for part in parts:
        if part.get("type") == "text":
            return part.get("text", "")
    return ""

def process_query(text):
    """
    你的 Agent 核心逻辑。
    这里用占位实现，替换为实际的 LLM 调用、工具调用等。
    """
    return f"收到请求: {text}（此处替换为你的实际处理逻辑）"

# ============================================================================
# 登录取 ADMIN 令牌（经 HTTP 网关调 agent_communication.auth.UserService/Login）
# ============================================================================

def get_admin_token():
    """登录平台获取 token；RegisterAgent/UnregisterAgent 为 ADMIN-only"""
    import requests
    resp = requests.post(
        f"{HTTP_GATEWAY}/agent_communication.auth.UserService/Login",
        json={"username": ADMIN_USERNAME, "password": ADMIN_PASSWORD},
        timeout=10,
    )
    resp.raise_for_status()
    return resp.json()["token"]

# ============================================================================
# gRPC 注册 / 心跳 / 注销
# ============================================================================

class PlatformRegistrar:
    """封装与 NexusAI 平台的 gRPC 注册生命周期"""

    def __init__(self, server_address, heartbeat_sec=15):
        self._server_address = server_address
        self._heartbeat_sec = heartbeat_sec
        self._channel = None
        self._stub = None
        self._agent_id = None
        self._running = False
        self._heartbeat_thread = None
        self._metadata = None        # authorization: Bearer <token>

    def register(self):
        """向平台注册 Agent（需 ADMIN 令牌）"""
        token = get_admin_token()
        self._metadata = (("authorization", f"Bearer {token}"),)
        self._channel = grpc.insecure_channel(self._server_address)
        self._stub = agent_service_pb2_grpc.AgentCommunicationServiceStub(self._channel)

        request = agent_service_pb2.RegisterAgentRequest(
            agent_info=common_pb2.ServiceInfo(
                service_name=AGENT_NAME,
                version=AGENT_VERSION,
                host=AGENT_HOST,
                port=AGENT_PORT,
                skills=[s["name"] for s in AGENT_CARD["skills"]],
                agent_card=json.dumps(AGENT_CARD, ensure_ascii=False),
                a2a_version="1.0",                       # A2A 协议版本
                deployment_stage="STABLE",                # 部署阶段: STABLE / CANARY / DEPRECATED
            ),
            heartbeat_interval=self._heartbeat_sec,
        )

        try:
            response = self._stub.RegisterAgent(request, timeout=10, metadata=self._metadata)
            if response.status.code != 0:
                print(f"注册失败: {response.status.message}")
                return False
            self._agent_id = response.agent_id
            print(f"注册成功，agent_id: {self._agent_id}")
            return True
        except grpc.RpcError as e:
            print(f"注册 RPC 失败: {e.details()}")
            return False

    def start_heartbeat(self):
        """启动后台心跳线程"""
        self._running = True
        self._heartbeat_thread = threading.Thread(target=self._heartbeat_loop, daemon=True)
        self._heartbeat_thread.start()

    def _heartbeat_loop(self):
        while self._running:
            time.sleep(self._heartbeat_sec)
            if not self._running:
                break
            try:
                self._stub.Heartbeat(
                    agent_service_pb2.HeartbeatRequest(
                        agent_id=self._agent_id,
                        # 可选：附带 agent_info 以同步更新元数据/指标
                        # agent_info=common_pb2.ServiceInfo(...)
                    ),
                    timeout=5,
                )
            except grpc.RpcError as e:
                print(f"心跳失败: {e.details()}")

    def unregister(self):
        """优雅注销"""
        self._running = False
        if self._heartbeat_thread:
            self._heartbeat_thread.join(timeout=5)
        if self._stub and self._agent_id:
            try:
                self._stub.UnregisterAgent(
                    agent_service_pb2.UnregisterAgentRequest(
                        agent_id=self._agent_id, reason="shutdown"
                    ),
                    timeout=5,
                    metadata=self._metadata,
                )
                print("已注销")
            except grpc.RpcError as e:
                print(f"注销失败: {e.details()}")
        if self._channel:
            self._channel.close()

# ============================================================================
# 主程序
# ============================================================================

def main():
    registrar = PlatformRegistrar(GRPC_SERVER, HEARTBEAT_INTERVAL)

    # 注册到平台
    if not registrar.register():
        sys.exit(1)

    # 启动心跳
    registrar.start_heartbeat()

    # 优雅退出
    def shutdown(signum, frame):
        print("\n正在关闭...")
        registrar.unregister()
        sys.exit(0)

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    # 启动 HTTP Server
    print(f"A2A HTTP Server 监听 http://0.0.0.0:{AGENT_PORT}")
    app.run(host="0.0.0.0", port=AGENT_PORT)

if __name__ == "__main__":
    main()
```

### 运行

```bash
python math_agent.py
```

启动后会看到：

```
注册成功，agent_id: agent-xxxx-xxxx
A2A HTTP Server 监听 http://0.0.0.0:8080
```

此时你的 Agent 已注册到平台，路由引擎可以将匹配 math/equation 技能的请求分发给你。

---

## 附录 A：Proto 参考

### common.proto — ServiceInfo

```protobuf
message ServiceInfo {
    string service_name = 1;          // Agent 名称（必填）
    string version = 2;               // 版本号（必填）
    string host = 3;                  // HTTP 监听地址（必填）
    int32 port = 4;                   // HTTP 监听端口（必填）
    repeated string tags = 5;
    map<string, string> metadata = 6;
    repeated string skills = 7;       // 技能名称列表（路由匹配的关键依据）
    string agent_card = 8;            // AgentCard JSON
    AgentMetrics agent_metrics = 9;   // 运行时指标（Batch 2）
    bool cacheable = 10;              // 响应是否可被语义缓存复用（Batch 3）
    string deployment_stage = 11;     // 部署阶段：STABLE / CANARY / DEPRECATED（Batch 6）
    string a2a_version = 12;          // A2A 协议版本号（Batch 8）
}
```

### common.proto — Status

```protobuf
message Status {
    int32 code = 1;      // 0 = 成功，非零 = 失败
    string message = 2;  // 人类可读的状态描述
    string details = 3;  // 详细信息
}
```

### common.proto — AgentMetrics

```protobuf
message AgentMetrics {
    string agent_id = 1;
    double success_rate = 2;          // 成功率 (0.0–1.0)
    double avg_latency_ms = 3;        // 平均延迟（毫秒）
    double p95_latency_ms = 4;        // P95 延迟（毫秒）
    int32 total_requests = 5;         // 累计请求数
    double approval_rate = 6;         // 用户点赞率 (0.0–1.0)
    int32 estimated_token_low = 7;    // 预估 token 消耗下限
    int32 estimated_token_high = 8;   // 预估 token 消耗上限
}
```

### agent_lifecycle.proto — 反馈与生命周期管理

```protobuf
service AgentLifecycleService {
    // 提交对 Agent 响应的评分（点赞/点踩）
    rpc SubmitFeedback(SubmitFeedbackRequest) returns (SubmitFeedbackResponse);

    // 获取同类 Agent 的指标对比
    rpc GetAgentCompare(GetAgentCompareRequest) returns (GetAgentCompareResponse);

    // 多维度对比 Agent（成功率/延迟/好评率，7-PR 优化新增）
    rpc CompareAgents(CompareAgentsRequest) returns (CompareAgentsResponse);

    // 设置用户对 Agent 的自主程度偏好
    rpc SetAutonomyLevel(SetAutonomyLevelRequest) returns (SetAutonomyLevelResponse);

    // 撤销 Agent 的某次操作
    rpc UndoAction(UndoActionRequest) returns (UndoActionResponse);
}
```

### agent_service.proto — 接入相关的 RPC

```protobuf
service AgentCommunicationService {
    // Agent 注册与生命周期
    rpc RegisterAgent(RegisterAgentRequest) returns (RegisterAgentResponse);
    rpc Heartbeat(HeartbeatRequest) returns (HeartbeatResponse);
    rpc UnregisterAgent(UnregisterAgentRequest) returns (UnregisterAgentResponse);

    // Agent 发现
    rpc FindAgents(FindAgentsRequest) returns (FindAgentsResponse);
    rpc GetAgents(GetAgentsRequest) returns (GetAgentsResponse);

    // 消息传递
    rpc SendMessage(SendMessageRequest) returns (SendMessageResponse);
    rpc ReceiveMessage(ReceiveMessageRequest) returns (ReceiveMessageResponse);
    rpc BroadcastMessage(BroadcastMessageRequest) returns (BroadcastMessageResponse);

    // 流式 RPC
    rpc ListenMessages(ReceiveMessageRequest) returns (stream Message);        // 服务端流式
    rpc BatchSendMessages(stream SendMessageRequest) returns (SendMessageResponse); // 客户端流式
    rpc RealTimeCommunication(stream Message) returns (stream Message);        // 双向流式
}
```

---

## 附录 B：常见问题

### 注册后路由不到我的 Agent

检查以下几点：

- `skills` 字段是否填写了与 AgentCard 一致的技能名称？空 skills 会导致 Embedding 和关键词路由都无法命中。
- `host:port` 是否是平台能访问到的地址？`127.0.0.1` 只在同一台机器上有效。
- 心跳是否正常？调用 `GetAgents` 或 `FindAgents` 确认你的 Agent 状态是健康的。

### 注册被拒绝：Administrator role required

说明你携带的 token 角色不是 ADMIN。确认两点：

- 注册账号的用户名是否与服务端环境变量 `NEXUSAI_ADMIN_USERNAME` 一致？只有匹配的用户才会被颁发 ADMIN 角色。
- token 是否过期？重新调用 `agent_communication.auth.UserService/Login` 获取新 token。未携带 token 时会收到 `Valid authentication token required`（UNAUTHENTICATED）。

### 心跳失败但 Agent 仍在运行

单次心跳失败不会导致立即下线，平台存活窗口为 `max(3 × heartbeat_interval, 300秒)`。以 15 秒间隔为例，窗口 300 秒内可容忍多次失败；连续超过窗口未收到心跳后才会移出路由候选。建议在心跳失败时打印日志并排查网络连通性；由于心跳具备 upsert 自愈语义，短暂中断后恢复心跳即可重新进入路由。

### Agent 被重复注册

`agent_id` 由 `service_name + host + port` 拼接生成。如果这三项相同，重复注册会更新已有记录而非报错。确保不同 Agent 实例的 service_name 或 host:port 不冲突。

### 如何实现长时任务

对于需要较长时间处理的任务，推荐使用 `message/stream` 端点返回 SSE 流。平台会在收到 `state=completed` 事件或连接关闭时结束等待。注意平台的默认请求超时为 30 秒（`A2AConfig.request_timeout_seconds`，gRPC deadline 会传播覆盖该值），如果需要更长时间，需要在 A2AAdapter 配置中调整。
