cmake_minimum_required(VERSION 3.20)

# MCP module configuration
set(MCP_SOURCES
    src/mcp_client.cpp
    src/mcp_tool_manager.cpp
    src/mcp_agent_integration.cpp
    # P7 (批次十一): embedding_service/vector_index/embedding_cache/
    # semantic_cache_index moved to agent_rpc_common (common/src/rag/); the
    # RAG tool-integration layer stays MCP-side.
    src/rag/tool_retriever.cpp
    src/rag/tool_validator.cpp
)

# Build the MCP library
add_library(agent_rpc_mcp STATIC ${MCP_SOURCES})

# Include directories
target_include_directories(agent_rpc_mcp PUBLIC
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Find jsoncpp
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

# Link dependencies
target_link_libraries(agent_rpc_mcp PRIVATE
    agent_rpc_common
    ${JSONCPP_LIBRARIES}
    Threads::Threads
    CURL::libcurl
)

target_include_directories(agent_rpc_mcp PRIVATE ${JSONCPP_INCLUDE_DIRS})

# C++ standard
target_compile_features(agent_rpc_mcp PRIVATE cxx_std_20)

# Compile options
if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(agent_rpc_mcp PRIVATE -Wall -Wextra -Wpedantic)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(agent_rpc_mcp PRIVATE -g -O0)
    else()
        target_compile_options(agent_rpc_mcp PRIVATE -O2 -DNDEBUG)
    endif()
endif()

# Install rules
install(TARGETS agent_rpc_mcp
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/agent_rpc/mcp/
    DESTINATION include/agent_rpc/mcp
    FILES_MATCHING PATTERN "*.h"
)


