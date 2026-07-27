cmake_minimum_required(VERSION 3.17)

# ─── Resolve worker binary path for the Go proxy ─────────────────────────────
if(ZYPP_STACK_BUILD)
  set(WORKER_BIN_PATH "$<TARGET_FILE:zypp-mcp-tool>")
else()
  set(WORKER_BIN_PATH "${CMAKE_INSTALL_FULL_LIBEXECDIR}/mcp-server-zypp/zypp-mcp-tool")
endif()

# ─── Redirect Go caches into the CMake build directory ────────────────────────
# GOCACHE:    intermediate build objects → <builddir>/go-cache
# GOMODCACHE: downloaded module zips     → <builddir>/go-mod
# Both are inside the CMake build tree — no source-dir taint, no ~/.cache leakage.
set(GO_CACHE    "${CMAKE_CURRENT_BINARY_DIR}/go-cache")
set(GO_MODCACHE "${CMAKE_CURRENT_BINARY_DIR}/go-mod")

# Shared environment block reused by every go invocation below.
# Note: GOFLAGS is intentionally unset (not passed) rather than set to ""
# because cmake -E env treats "" as a literal empty-string flag argument,
# which Go rejects with "non-flag". Unset means the variable is simply
# absent from the child environment, which is what we want.
set(GO_ENV
  CGO_ENABLED=0
  GOCACHE=${GO_CACHE}
  GOMODCACHE=${GO_MODCACHE}
)

# ─── go mod download ──────────────────────────────────────────────────────────
# Populates GOMODCACHE from go.sum. Must run before go build on a clean machine.
# Stamp file marks completion so CMake only re-runs when go.sum changes.
set(MOD_STAMP "${CMAKE_CURRENT_BINARY_DIR}/go-mod-download.stamp")

add_custom_command(
  OUTPUT  "${MOD_STAMP}"
  COMMAND "${CMAKE_COMMAND}" -E env ${GO_ENV}
          "${GO_EXECUTABLE}" mod download -x
  COMMAND "${CMAKE_COMMAND}" -E touch "${MOD_STAMP}"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/go.mod"
          "${CMAKE_CURRENT_SOURCE_DIR}/go.sum"
  COMMENT "go mod download"
  VERBATIM
)
add_custom_target(go-mod-download DEPENDS "${MOD_STAMP}")

# ─── go mod tidy ─────────────────────────────────────────────────────────────
# Updates go.mod and go.sum to match the source. Not part of ALL — run explicitly:
#   cmake --build <builddir> --target go-mod-tidy
# go.sum is written back into the source tree (correct — it is a committed file).
add_custom_target(go-mod-tidy
  COMMAND "${CMAKE_COMMAND}" -E env ${GO_ENV}
          "${GO_EXECUTABLE}" mod tidy
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  COMMENT "go mod tidy"
  VERBATIM
)

# ─── Build mcp-server-zypp via go build ───────────────────────────────────────
# -ldflags "-X ..." injects the worker path at link time — no generated source files.
# All Go artifacts stay under CMAKE_CURRENT_BINARY_DIR.
set(PROXY_BIN "${CMAKE_CURRENT_BINARY_DIR}/mcp-server-zypp")
set(PROXY_LDFLAGS "-X 'github.com/openSUSE/mcp-server-zypp/internal/config.DefaultWorkerPath=${WORKER_BIN_PATH}'")

file(GLOB_RECURSE PROXY_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.go")

# ── Type-check test sources at build time ─────────────────────────────────────
# `go build ./tests/...` compiles the test package without running it, catching
# syntax and type errors at cmake --build time rather than at ctest time.
set(TEST_TYPECHECK_STAMP "${CMAKE_CURRENT_BINARY_DIR}/go-test-typecheck.stamp")
add_custom_command(
  OUTPUT  "${TEST_TYPECHECK_STAMP}"
  COMMAND "${CMAKE_COMMAND}" -E env ${GO_ENV}
          "${GO_EXECUTABLE}" build ./tests/...
  COMMAND "${CMAKE_COMMAND}" -E touch "${TEST_TYPECHECK_STAMP}"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  DEPENDS ${PROXY_SOURCES}
          "${CMAKE_CURRENT_SOURCE_DIR}/go.sum"
          "${MOD_STAMP}"
  COMMENT "go build ./tests/... (type-check)"
  VERBATIM
)

add_custom_command(
  OUTPUT  "${PROXY_BIN}"
  COMMAND "${CMAKE_COMMAND}" -E env ${GO_ENV}
          "${GO_EXECUTABLE}" build
            -ldflags "${PROXY_LDFLAGS}"
            -o "${PROXY_BIN}"
            ./cmd/mcp-server-zypp
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  DEPENDS ${PROXY_SOURCES}
          "${CMAKE_CURRENT_SOURCE_DIR}/go.sum"
          "${MOD_STAMP}"
          "${TEST_TYPECHECK_STAMP}"
  COMMENT "go build mcp-server-zypp"
  VERBATIM
)

add_custom_target(mcp-server-zypp-proxy ALL DEPENDS "${PROXY_BIN}"
  # Listing Go sources here makes Qt Creator show them in the project tree.
  # CMake does not attempt to compile them — SOURCES on a custom target is
  # purely informational for IDEs that consume the CMake file API.
  SOURCES
    ${PROXY_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/go.mod"
    "${CMAKE_CURRENT_SOURCE_DIR}/go.sum"
)
add_dependencies(mcp-server-zypp-proxy zypp-mcp-tool)

# ─── Integration tests ────────────────────────────────────────────────────────
# Run with: cmake --build <builddir> --target test
# or:       ctest --test-dir <builddir> -R mcp_worker_tests -V
file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.go"
)
file(GLOB_RECURSE TEST_DATA CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/../tests/testdata/*"
)

# Expose test sources and testdata to Qt Creator via a dedicated target.
add_custom_target(mcp-server-zypp-tests
  SOURCES
    ${TEST_SOURCES}
    ${TEST_DATA}
)

add_test(NAME mcp_worker_tests
  COMMAND "${CMAKE_COMMAND}" -E env
          ${GO_ENV}
          MCP_WORKER_BINARY=$<TARGET_FILE:zypp-mcp-tool>
          MCP_TESTDATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../tests/testdata
          "${GO_EXECUTABLE}" test -v ./tests/...
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

install(PROGRAMS "${PROXY_BIN}" DESTINATION "${CMAKE_INSTALL_BINDIR}")
