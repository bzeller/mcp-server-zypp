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

# ─── Target: zypp-mcp-tool ────────────────────────────────────────────────────
set(WORKER_HEADERS
  src/transport.h
  src/callbacks.h
  src/system.h
  src/tools/registry.h
  src/tools/tools.h
  src/tools/validate.h
  src/tools/transaction.h
)


set(WORKER_SOURCES
  src/main.cc
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

add_executable(zypp-mcp-tool ${WORKER_HEADERS} ${WORKER_SOURCES})
target_link_libraries(zypp-mcp-tool PRIVATE ${ZYPP_LIBRARY})
target_compile_features(zypp-mcp-tool PRIVATE cxx_std_17)

set_target_properties(zypp-mcp-tool PROPERTIES
  LINK_FLAGS "-pie -Wl,-z,relro,-z,now"
  COMPILE_FLAGS "-fpie -fPIE"
)

install(TARGETS zypp-mcp-tool RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/mcp-server-zypp)
