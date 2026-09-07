# Server module
cmake_minimum_required(VERSION 3.15)

# Source files (library)
set(SERVER_SOURCES
    src/rpc_server.cpp
    src/agent_service.cpp
    src/ai_query_service.cpp
    src/orchestration_service_impl.cpp
    src/multi_agent_handler.cpp
    src/query_helpers.cpp
    src/auth_service.cpp
    src/auth_interceptor.cpp
    src/sharing_service.cpp
    src/agent_lifecycle_service.cpp
    src/user_experience_service.cpp
    src/observability_service.cpp
)

# Build the static library

add_library(agent_rpc_server STATIC ${SERVER_SOURCES})

# RPC server executable
add_executable(rpc_server src/main.cpp)
set(NEXUSAI_MIGRATIONS_DEFAULT_DIR
    "${CMAKE_SOURCE_DIR}/db/migrations"
    CACHE PATH "Default migration directory for local rpc_server runs")
file(TO_CMAKE_PATH "${NEXUSAI_MIGRATIONS_DEFAULT_DIR}" NEXUSAI_MIGRATIONS_DEFAULT_DIR_FOR_COMPILE)
target_compile_definitions(rpc_server PRIVATE
    NEXUSAI_MIGRATIONS_DEFAULT_DIR="${NEXUSAI_MIGRATIONS_DEFAULT_DIR_FOR_COMPILE}"
)
if(ENABLE_MCP)
    # P7 (批次十一): AgentRouter and the server no longer reference MCP
    # headers or the AGENT_RPC_ENABLE_MCP layout macro (the vector tier now
    # lives in agent_rpc_common), so nothing MCP-specific is compiled here.
    # agent_rpc_mcp remains available for MCP-side tests/tools.
endif()

target_link_libraries(rpc_server
    agent_rpc_server
    agent_rpc_db
    agent_rpc_common
    agent_rpc_registry
    orchestrator
    a2a_adapter
    a2a
    proto_lib
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    pthread
    ${UUID_LIB}
    CURL::libcurl
    ${HIREDIS_LIBRARIES}
)

# Link dependencies
target_link_libraries(agent_rpc_server
    agent_rpc_common
    agent_rpc_registry
    orchestrator
    a2a_adapter
    a2a
    proto_lib
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES}
    grpc++_reflection  # Link directly (TODO: use find_package(gRPC CONFIG) for imported target gRPC::grpc++_reflection)
    pthread
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    ${HIREDIS_LIBRARIES}
)

# Include directories
target_include_directories(agent_rpc_server PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/../proto
)

# Compile options
target_compile_options(agent_rpc_server PRIVATE
    -Wall
    -Wextra
    -std=c++20  # Consistent with root CMakeLists.txt C++20 standard
)

# Install rules
install(TARGETS agent_rpc_server
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/agent_rpc
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)
