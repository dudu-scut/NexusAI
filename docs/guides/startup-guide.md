# NexusAI 项目启动指南

> 从零到全链路可用的完整开发环境搭建流程。

## 一、环境准备

### 系统要求

| 环境 | 用途 | 说明 |
|------|------|------|
| **WSL2 (Ubuntu 20.04+)** | C++ 编译、后端服务运行 | 所有 `./run.sh` 命令在 WSL 内执行 |
| **Windows** | 前端开发、gRPC Proxy | PowerShell 终端 |

### 必要工具

| 工具 | 最低版本 | 安装方式 |
|------|---------|---------|
| CMake | 3.20+ | `sudo apt install cmake` |
| GCC | 10+（C++20） | `sudo apt install build-essential` |
| gRPC + Protobuf | 1.51.1+ | `sudo apt install libgrpc++-dev protobuf-compiler-grpc` |
| Redis | 7.0+ | `sudo apt install redis-server` |
| PostgreSQL 16 + libpq | 16+ | `sudo apt install postgresql libpq-dev`（Docker 方式见第五节） |
| libpqxx | 8.0.1（pin） | `./scripts/bootstrap-wsl.sh` 自动安装 |
| hiredis | - | `sudo apt install libhiredis-dev` |
| nlohmann-json | 3.x | 项目自带（`a2a/third_party/json.hpp`） |
| Node.js | 18+ | Windows 安装或 `nvm` |
| grpcurl | 最新版 | `go install github.com/fullstorydev/grpcurl/cmd/grpcurl@latest`（安装到 `~/.local/bin/`） |

### 一键安装依赖

```bash
sudo apt-get update
sudo apt-get install -y \
    cmake build-essential pkg-config \
    libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc \
    libcurl4-openssl-dev libjsoncpp-dev uuid-dev \
    libgtest-dev libhiredis-dev redis-server \
    nlohmann-json3-dev
```

### 环境变量

项目根目录创建 `.env` 文件：

```bash
LLM_API_KEY=your-api-key-here
LLM_MODEL=deepseek-v4-flash
LLM_API_URL=https://api.deepseek.com
```

| 变量 | 必填 | 说明 |
|------|------|------|
| `LLM_API_KEY` | ✅ | LLM API 密钥（OpenAI 兼容格式） |
| `LLM_MODEL` | 否 | 模型名称，默认 `deepseek-v4-flash` |
| `LLM_API_URL` | 否 | API 端点 |

> ⚠️ 不配置 `LLM_API_KEY` 会导致 Orchestrator 的路由 Tier 2（LLM 意图分类）和 DAG 任务分解不可用，但服务本身可以启动，基础查询走 Tier 1 Embedding 路由。

---

## 二、编译

### 推荐方式

```bash
# WSL 内，项目根目录
./run.sh build
```

等价于 `mkdir -p build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)`。

### 增量编译

```bash
cd build && make -j$(nproc)
```

### 编译产物

`build/` 目录下关键二进制：

| 产物 | 路径 | 说明 |
|------|------|------|
| gRPC Server | `build/server/rpc_server` | 核心服务端，监听 :50051 |
| 测试套件 | `build/tests/test_*` | 37 套 GTest + RapidCheck 测试 |

---

## 三、启动后端服务

NexusAI 运行时依赖 6 个服务：

| 服务 | 端口 | 类型 | 路径 |
|------|------|------|------|
| PostgreSQL | 5432 | Docker（推荐） | `docker compose up -d postgres redis` |
| Redis | 6379 | Docker / 系统服务 | `redis-server` |
| Mock Agent | 5100 | Python A2A | `verify/mock-agent/mock_agent_server.py` |
| gRPC Server | 50051 | C++ 二进制 | `build/server/rpc_server` |
| Orchestrator | 5000 | Python A2A | `examples/orchestrator_agent.py` |
| Node.js Proxy | 8081 | Node.js | `gateway/proxy/server.mjs`（Windows） |

### 方式一：`run.sh start-all`（推荐，一键全包，等同 `run.sh gateway`，以 Docker 启动后端栈）

```bash
# WSL 内
./run.sh start-all
```

启动 Redis + Mock Agent + Orchestrator + gRPC Server + Node Proxy，全部后台运行，PID 写入 `pids/`。

### 方式二：`scripts/start_backend.sh`（仅后端 4 服务）

```bash
# WSL 内
./scripts/start_backend.sh
```

启动 Redis → Mock Agent → gRPC Server → Orchestrator（不含 Node Proxy）。每步检查存活，失败即退出。

### 方式三：分步启动

```bash
./run.sh redis              # 仅 Redis
./run.sh start-mock-agent   # 仅 Mock Agent (:5100)
./run.sh start-orchestrator # 仅 Orchestrator (:5000)
./run.sh start              # 仅 gRPC Server (:50051)
```

### 验证服务状态

```bash
ss -tlnp | grep -E '(6379|5100|50051|5000)'
```

预期 4 行 LISTEN 状态。或用：

```bash
cat pids/*.pid    # 查看各服务 PID
```

---

## 四、启动 gRPC Proxy（Windows）

gRPC Proxy 将前端 HTTP JSON 请求转为 gRPC 调用，并实现 gRPC Server Streaming → SSE 协议转换。

```powershell
# Windows PowerShell，项目根目录
$env:GRPC_TARGET="localhost:50051"
node gateway\proxy\server.mjs
```

预期输出：

```
gRPC clients initialized → localhost:50051
NexusAI gRPC-JSON proxy listening on :8081
```

| 环境变量 | 默认值 | 说明 |
|------|--------|------|
| `PROXY_PORT` | `8081` | 代理监听端口 |
| `GRPC_TARGET` | `localhost:50051` | gRPC 后端地址 |

---

## 五、启动前端

```powershell
# Windows PowerShell，新窗口
cd frontend
npm ci         # 首次（与 README/CI 一致）
npm run dev    # Vite :5173
```

浏览器访问 **http://localhost:5173**。Vite 自动将 `/agent_communication.*` 请求代理转发到 Proxy `:8081`。

---

## 六、注册 Mock Agent

```bash
# WSL 内
./scripts/register_agents.sh
```

注册 4 个测试 Agent：

| Agent | 技能 | A2A 端点 |
|------|------|---------|
| mock-general | general | `http://127.0.0.1:5100/` |
| math-agent | math, calculation | `http://127.0.0.1:5100/` |
| translator-agent | translation, language | `http://127.0.0.1:5100/` |
| echo-agent | echo, test | `http://127.0.0.1:5100/` |

注册流程：Login（`smoke3` / `pass1234`）→ RegisterAgent × 4 → GetAgents 确认。

> ⚠️ 依赖 `~/.local/bin/grpcurl`，确保已安装。

---

## 七、验证全链路

### 快速冒烟

在浏览器中打开 http://localhost:5173，登录后发送"hello"——应收到 Mock Agent 流式回复。

### E2E 自动化测试

```bash
./run.sh verify          # 运行全部 8 批次 32 个场景
./run.sh verify-batch1   # 单独运行某批次
```

### 手动验证清单



---

## 八、停止服务

```bash
# WSL 内
./run.sh stop
```

停止所有通过 `run.sh` 和 `scripts/start_backend.sh` 启动的进程（读取 `pids/*.pid`），并停止 Docker 网关容器（如有）。

Windows 侧手动 `Ctrl+C` 停止 Proxy 和 Vite。

---

## 九、故障排查

### 前端卡在 "Analyzing request..." 不动

1. 确认 Mock Agent :5100 已启动
2. 查看 Mock Agent 终端日志
3. 确认 Orchestrator :5000 已启动且可访问

### gRPC Server 启动后立即退出

1. 确认 `.env` 文件存在且含 `LLM_API_KEY`
2. 确认 Redis :6379 已启动

### Agent 注册失败

1. `which grpcurl` 或检查 `~/.local/bin/grpcurl`
2. 确认 gRPC Server :50051 已监听

### 端口被占用

```bash
ss -tlnp | grep :6379    # Redis
ss -tlnp | grep :5100    # Mock Agent
ss -tlnp | grep :50051   # gRPC Server
ss -tlnp | grep :5000    # Orchestrator
```

### WSL 进程在关闭窗口后消失

始终使用 `./run.sh start-all` 或 `./scripts/start_backend.sh`（已使用 `nohup + disown`）。

---

## 十、架构与端口总览

```
┌────────────────────────────────────────────┐
│  Browser  http://localhost:5173             │
│  Vue 3 SPA — 10 views, SSE streaming       │
└──────────────┬─────────────────────────────┘
               │ HTTP (Vite proxy)
┌──────────────▼─────────────────────────────┐
│  Node.js gRPC Proxy  :8081  (Windows)      │
│  JSON → gRPC / gRPC Streaming → SSE        │
└──────────────┬─────────────────────────────┘
               │ gRPC/Protobuf
┌──────────────▼─────────────────────────────┐
│  gRPC Server  :50051  (WSL)                │
│  C++ — 9 Services, 41 RPCs                 │
│  AuthInterceptor | CostInterceptor          │
└─────┬──────────┬──────────┬────────────────┘
      │          │          │
┌─────▼──┐ ┌─────▼──┐ ┌─────▼──────────────┐
│ Redis  │ │Orchest-│ │ Mock Agent :5100    │
│ :6379  │ │rator   │ │ Python A2A Server   │
│ cache/ │ │:5000   │ │                      │
│ state  │ │Python  │ │                      │
└────────┘ └────────┘ └─────────────────────┘
```

| 端口 | 服务 | 环境 | 协议 |
|------|------|------|------|
| 5173 | Vite Dev Server | Windows | HTTP |
| 8081 | Node.js gRPC Proxy | Windows | HTTP/SSE |
| 50051 | gRPC Server | WSL | gRPC/HTTP2 |
| 5000 | Orchestrator | WSL | HTTP/A2A |
| 5100 | Mock Agent | WSL | HTTP/A2A |
| 6379 | Redis | WSL | TCP |
