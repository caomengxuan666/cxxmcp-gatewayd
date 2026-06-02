cmake_minimum_required(VERSION 3.23)

project(cxxmcp-gatewayd
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(GNUInstallDirs)

find_package(Threads REQUIRED)

set(CXXMCP_GATEWAYD_GATEWAY_SOURCE_DIR
    ""
    CACHE PATH "Optional explicit local cxxmcp-gateway source tree; empty uses find_package(cxxmcp-gateway)")

if(CXXMCP_GATEWAYD_GATEWAY_SOURCE_DIR)
    set(CXXMCP_GATEWAY_BUILD_RUNTIME ON CACHE BOOL "" FORCE)
    set(CXXMCP_GATEWAY_BUILD_CONFIG_IO ON CACHE BOOL "" FORCE)
    set(CXXMCP_GATEWAY_BUILD_CLI OFF CACHE BOOL "" FORCE)
    set(CXXMCP_GATEWAY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CXXMCP_GATEWAY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CXXMCP_GATEWAY_BUILD_PERF OFF CACHE BOOL "" FORCE)
    add_subdirectory(
        "${CXXMCP_GATEWAYD_GATEWAY_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/_deps/cxxmcp-gateway"
        EXCLUDE_FROM_ALL)
else()
    find_package(cxxmcp-gateway CONFIG REQUIRED COMPONENTS runtime config_io)
endif()

add_executable(cxxmcp_gatewayd
    src/main.cpp
)
target_compile_features(cxxmcp_gatewayd PRIVATE cxx_std_23)
target_link_libraries(cxxmcp_gatewayd
    PRIVATE
        cxxmcp-gateway::runtime
        cxxmcp-gateway::config_io
        Threads::Threads
)
target_compile_definitions(cxxmcp_gatewayd PRIVATE
    CXXMCP_GATEWAYD_VERSION="${PROJECT_VERSION}"
)
set_target_properties(cxxmcp_gatewayd PROPERTIES
    OUTPUT_NAME cxxmcp-gatewayd
)

install(TARGETS cxxmcp_gatewayd
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(FILES examples/gatewayd.example.json
    DESTINATION "${CMAKE_INSTALL_DATADIR}/cxxmcp-gatewayd/examples"
)
install(FILES
        docs/audit_events.md
        docs/architecture.md
        docs/roadmap.md
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
)
install(DIRECTORY packaging/
    DESTINATION "${CMAKE_INSTALL_DATADIR}/cxxmcp-gatewayd/packaging"
)

if(BUILD_TESTING)
    add_test(
        NAME gatewayd_help
        COMMAND cxxmcp_gatewayd --help
    )
    add_test(
        NAME gatewayd_validate_example_config
        COMMAND cxxmcp_gatewayd validate --config "${CMAKE_CURRENT_SOURCE_DIR}/examples/gatewayd.example.json"
    )
endif()

set(CPACK_PACKAGE_NAME "cxxmcp-gatewayd")
set(CPACK_PACKAGE_VENDOR "caomengxuan666")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Local MCP middleware daemon built on cxxmcp-gateway")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "cxxmcp-gatewayd")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)
