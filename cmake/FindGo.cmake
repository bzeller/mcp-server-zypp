# FindGo.cmake — locate the Go toolchain
# Sets: GO_EXECUTABLE, GO_VERSION, Go_FOUND

find_program(GO_EXECUTABLE NAMES go DOC "Go compiler")

if(GO_EXECUTABLE)
  execute_process(
    COMMAND "${GO_EXECUTABLE}" version
    OUTPUT_VARIABLE _GO_VERSION_RAW
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  string(REGEX MATCH "go[0-9]+\\.[0-9]+(\\.[0-9]+)?" GO_VERSION "${_GO_VERSION_RAW}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Go
  REQUIRED_VARS GO_EXECUTABLE
  VERSION_VAR   GO_VERSION
)
mark_as_advanced(GO_EXECUTABLE)
