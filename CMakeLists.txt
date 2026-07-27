cmake_minimum_required(VERSION 3.17)
project(mcp-server-zypp VERSION 0.1.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)

find_package(Go REQUIRED)

# ─── C++ worker (zypp-mcp-tool) ────────────────────────────────────────────────
add_subdirectory(worker)

# ─── Go proxy (mcp-server-zypp) ───────────────────────────────────────────────
add_subdirectory(proxy)
