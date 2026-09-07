# Orchestrator module CMakeLists.txt

cmake_minimum_required(VERSION 3.15)

# Source files
set(ORCHESTRATOR_SOURCES
    src/agent_router.cpp
    src/agent_info.cpp
    src/task_planner.cpp
    src/task_executor.cpp
    src/result_aggregator.cpp
    src/feedback_aggregator.cpp
    src/replay_service.cpp
    src/export_service.cpp           # Batch 6
)

# Header files
set(ORCHESTRATOR_HEADERS
    include/agent_rpc/orchestrator/agent_info.h
    include/agent_rpc/orchestrator/agent_router.h
    include/agent_rpc/orchestrator/task_planner.h
    include/agent_rpc/orchestrator/task_executor.h
    include/agent_rpc/orchestrator/result_aggregator.h
    include/agent_rpc/orchestrator/feedback_aggregator.h
    include/agent_rpc/orchestrator/export_service.h    # Batch 6
)

# Create library
add_library(orchestrator ${ORCHESTRATOR_SOURCES} ${ORCHESTRATOR_HEADERS})

target_include_directories(orchestrator
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../a2a/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../a2a/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/../a2a_adapter/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../common/include
        ${CMAKE_BINARY_DIR}/proto
)

target_link_libraries(orchestrator
    PUBLIC
        agent_rpc_common
        a2a
        a2a_adapter
        ${GRPC_LIBRARIES}
        ${PROTOBUF_LIBRARIES}
        CURL::libcurl
)


if(ENABLE_MCP)
    # P7 (批次十一): AgentRouter's vector-routing tier moved to
    # agent_rpc_common, so orchestrator no longer depends on agent_rpc_mcp
    # or the AGENT_RPC_ENABLE_MCP layout macro (the old PRIVATE/PUBLIC ODR
    # hazard class is gone with the dual implementations).
endif()

# Embedding routing requires C++20 (MCP module uses C++20)
target_compile_features(orchestrator PUBLIC cxx_std_20)

# Compiler options
if(NOT MSVC)
    target_compile_options(orchestrator PRIVATE -Wall -Wextra)
endif()

# Export target
set_target_properties(orchestrator PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
