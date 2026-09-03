cmake_minimum_required(VERSION 3.17)
project(mcp-server-zypp VERSION 0.1.2 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)

# ─── C++ worker (zypp-mcp-tool) ───────────────────────────────────────────────
add_subdirectory(worker)

# ─── Go proxy (mcp-server-zypp) ───────────────────────────────────────────────
# Set BUILD_GO_PROXY=OFF to skip the Go build — useful when the proxy binary
# is built separately (e.g. via 'go build -mod=vendor' in an OBS spec file).
option(BUILD_GO_PROXY "Build the Go proxy binary via cmake" ON)
if(BUILD_GO_PROXY)
  find_package(Go REQUIRED)
  add_subdirectory(proxy)
endif()
