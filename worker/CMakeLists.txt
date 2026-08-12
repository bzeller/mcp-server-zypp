cmake_minimum_required(VERSION 3.17)

# ─── Resolve libzypp ──────────────────────────────────────────────────────────
# Mirrors zypper/CMakeLists.txt:76-83 — use in-tree target when building inside
# zypp-stack, otherwise locate an installed libzypp via ZyppConfig.cmake.
if(ZYPP_STACK_BUILD)
  set(ZYPP_FOUND 1)
  set(ZYPP_LIBRARY zypp)
else()
  if(DEFINED ZYPP_PREFIX)
    list(APPEND CMAKE_MODULE_PATH
      "${ZYPP_PREFIX}/${LIB}/cmake/Zypp/"
      "${ZYPP_PREFIX}/lib64/cmake/Zypp/"
      "${ZYPP_PREFIX}/lib/cmake/Zypp/"
    )
  else()
    list(APPEND CMAKE_MODULE_PATH
      "${CMAKE_INSTALL_PREFIX}/${LIB}/cmake/Zypp/"
      "${CMAKE_INSTALL_PREFIX}/lib64/cmake/Zypp/"
      "${CMAKE_INSTALL_PREFIX}/lib/cmake/Zypp/"
    )
  endif()
  find_package(Zypp REQUIRED)
  include_directories(${ZYPP_INCLUDE_DIR})
endif()

if(ZYPP_STACK_BUILD)
  include_directories(${ZYPP_STACK_INCLUDE_DIRS})
endif()

# ─── Shared object library ────────────────────────────────────────────────────
# Everything except main.cc — compiled once, linked into both zypp-mcp-tool
# and zypp-mcp-tool-tests, so adding/using the test target never triggers a
# second compilation of the same translation units.
set(WORKER_HEADERS
  src/transport.h
  src/callbacks.h
  src/system.h
  src/tools/registry.h
  src/tools/tools.h
  src/tools/validate.h
  src/tools/transaction.h
)

set(WORKER_LIB_SOURCES
  src/transport.cc
  src/callbacks.cc
  src/system.cc
  src/tools/transaction.cc
  src/tools/search_packages.cc
  src/tools/find_providers.cc
  src/tools/find_dependents.cc
  src/tools/updates.cc
  src/tools/plan_install.cc
  src/tools/plan_remove.cc
  src/tools/confirm_install.cc
  src/tools/confirm_remove.cc
)

add_library(zypp-mcp-tool-objlib OBJECT ${WORKER_HEADERS} ${WORKER_LIB_SOURCES})
target_link_libraries(zypp-mcp-tool-objlib PUBLIC ${ZYPP_LIBRARY})
target_compile_features(zypp-mcp-tool-objlib PUBLIC cxx_std_17)
set_target_properties(zypp-mcp-tool-objlib PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ─── Target: zypp-mcp-tool ────────────────────────────────────────────────────
add_executable(zypp-mcp-tool src/main.cc)
target_link_libraries(zypp-mcp-tool PRIVATE zypp-mcp-tool-objlib)

set_target_properties(zypp-mcp-tool PROPERTIES
  LINK_FLAGS "-pie -Wl,-z,relro,-z,now"
  COMPILE_FLAGS "-fpie -fPIE"
)

install(TARGETS zypp-mcp-tool RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/mcp-server-zypp)

# ─── C++ unit tests ────────────────────────────────────────────────────────────
# Covers logic that needs no root and no real commit() — e.g. the license
# confirmation gate — by calling it directly, bypassing tool_confirm_install()'s
# entry point (which starts with an unconditional geteuid() check that must
# never be reordered/weakened for testability; see worker/tests/transaction_test.cc
# for the full rationale). Actual commit()/RPM-transaction execution genuinely
# needs root and is out of scope for this test suite — RPM/OBS %check build
# roots are unprivileged by design, and namespace tricks to fake geteuid()==0
# wouldn't make the underlying rpm db/filesystem writes behave reliably anyway.
find_package(Boost REQUIRED COMPONENTS unit_test_framework)

add_executable(zypp-mcp-tool-tests tests/transaction_test.cc)
target_link_libraries(zypp-mcp-tool-tests PRIVATE zypp-mcp-tool-objlib Boost::unit_test_framework)
target_compile_definitions(zypp-mcp-tool-tests PRIVATE
  MCP_TEST_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tests/testdata")

add_test(NAME mcp_worker_cpp_tests COMMAND zypp-mcp-tool-tests --catch_system_errors=no)
