# 公共组件库
cmake_minimum_required(VERSION 3.15)

# 源文件列表
set(COMMON_SOURCES
    src/logger.cpp
    src/serializer.cpp
    src/message_converter.cpp
    src/metrics.cpp
    src/circuit_breaker.cpp
    src/load_balancer.cpp
    src/memory_service.cpp
    src/redis_client.cpp
    src/rpc_framework.cpp
    src/cost_tracker.cpp        # Batch 1
    src/postgres_store.cpp
    src/postgres_budget_repository.cpp
    src/query_domain_repository.cpp
    src/agent_runtime_repository.cpp
    src/auth_repository.cpp
    src/profile_summarizer.cpp  # Batch 4 U2
    # P7 (批次十一): RAG vector building blocks moved out of agent_rpc_mcp so
    # the default build (no -DENABLE_MCP) can host the embedding routing tier.
    src/rag/embedding_service.cpp
    src/rag/vector_index.cpp
    src/rag/embedding_cache.cpp
    src/rag/semantic_cache_index.cpp
)

# 创建静态库
add_library(agent_rpc_common STATIC ${COMMON_SOURCES})

# 链接依赖
target_link_libraries(agent_rpc_common
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    pthread
    ${UUID_LIB}
    curl
    jsoncpp
    ${HIREDIS_LIBRARIES}
    ${AGENT_RPC_PQXX_TARGET}
    proto_lib
)

# 包含目录
target_include_directories(agent_rpc_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/../proto
)

# 编译选项
target_compile_options(agent_rpc_common PRIVATE
    -Wall
    -Wextra
    -std=c++20  # Fix #53: consistent with root CMakeLists.txt
)

# 安装规则
install(TARGETS agent_rpc_common
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/agent_rpc
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)
