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
  src/context.h
  src/gpgkeygate.h
  src/commitfailurelog.h
  src/callbacks.h
  src/cancellation.h
  src/system.h
  src/tools/registry.h
  src/tools/tools.h
  src/tools/validate.h
  src/tools/transaction.h
  src/solver/packagespec.h
  src/solver/feedback.h
  src/solver/patchfilter.h
  src/solver/options.h
  src/solver/selection.h
  src/solver/packageargs.h
  src/solver/requester.h
  src/solver/apply.h
)

set(WORKER_LIB_SOURCES
  src/transport.cc
  src/context.cc
  src/gpgkeygate.cc
  src/commitfailurelog.cc
  src/callbacks.cc
  src/cancellation.cc
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
  src/solver/options.cc
  src/solver/feedback.cc
  src/solver/packageargs.cc
  src/solver/requester.cc
  src/solver/apply.cc
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

add_executable(zypp-mcp-tool-tests
  tests/transaction_test.cc
  tests/preloadprogress_test.cc
)
target_link_libraries(zypp-mcp-tool-tests PRIVATE zypp-mcp-tool-objlib Boost::unit_test_framework)
target_compile_definitions(zypp-mcp-tool-tests PRIVATE
  BOOST_TEST_DYN_LINK
  MCP_TEST_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tests/testdata")

add_test(NAME mcp_worker_cpp_tests COMMAND zypp-mcp-tool-tests --catch_system_errors=no)

# GpgKeyGate is deliberately free of libzypp types (see gpgkeygate.h) — built
# as its own small executable, independent of zypp-mcp-tool-objlib, so this
# test stays fast and buildable without the zypp toolchain.
add_executable(zypp-mcp-gpgkeygate-tests
  tests/gpgkeygate_test.cc
  src/gpgkeygate.cc
)
target_link_libraries(zypp-mcp-gpgkeygate-tests PRIVATE Boost::unit_test_framework)
target_compile_features(zypp-mcp-gpgkeygate-tests PRIVATE cxx_std_17)
target_compile_definitions(zypp-mcp-gpgkeygate-tests PRIVATE BOOST_TEST_DYN_LINK)

add_test(NAME mcp_gpgkeygate_tests COMMAND zypp-mcp-gpgkeygate-tests --catch_system_errors=no)

# CommitFailureLog is deliberately free of libzypp types (see
# commitfailurelog.h), for the same reason and with the same shape as
# GpgKeyGate above.
add_executable(zypp-mcp-commitfailurelog-tests
  tests/commitfailurelog_test.cc
  src/commitfailurelog.cc
)
target_link_libraries(zypp-mcp-commitfailurelog-tests PRIVATE Boost::unit_test_framework)
target_compile_features(zypp-mcp-commitfailurelog-tests PRIVATE cxx_std_17)
target_compile_definitions(zypp-mcp-commitfailurelog-tests PRIVATE BOOST_TEST_DYN_LINK)

add_test(NAME mcp_commitfailurelog_tests COMMAND zypp-mcp-commitfailurelog-tests --catch_system_errors=no)

# ─── Ported SolverRequester tests ──────────────────────────────────────────────
# Ported from zypper/tests/SolverRequester_test.cc — see
# worker/tests/solver/solverrequester_test.cc's header comment for exactly
# what changed vs. upstream (nothing in the test bodies themselves). Needs
# the full libzypp toolchain and a real RepoManager against real rpm-md/
# yast2 repo metadata (worker/tests/solverrequester-data/, copied from
# zypper/tests/data/) — no root, no network, no live system. Its own
# target rather than reusing zypp-mcp-tool-objlib: unrelated translation
# units there would only slow down this test's rebuild.
#
# BOOST_TEST_DYN_LINK + BOOST_TEST_MAIN mirror zypper's own
# tests/CMakeLists.txt: ADD_TESTS() macro exactly — the ported .cc file
# relies on Boost.Test's auto-generated main() via these defines rather
# than declaring its own BOOST_TEST_MODULE, matching upstream.
add_executable(zypp-mcp-solverrequester-tests
  tests/solver/solverrequester_test.cc
  src/solver/options.cc
  src/solver/feedback.cc
  src/solver/packageargs.cc
  src/solver/requester.cc
  src/solver/apply.cc
)
target_link_libraries(zypp-mcp-solverrequester-tests PRIVATE ${ZYPP_LIBRARY} Boost::unit_test_framework)
target_compile_features(zypp-mcp-solverrequester-tests PRIVATE cxx_std_17)
target_compile_definitions(zypp-mcp-solverrequester-tests PRIVATE
  BOOST_TEST_DYN_LINK
  BOOST_TEST_MAIN
  TESTS_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/solverrequester-data")

add_test(NAME mcp_solverrequester_tests COMMAND zypp-mcp-solverrequester-tests --catch_system_errors=no)
