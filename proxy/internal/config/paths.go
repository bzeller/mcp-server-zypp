package config

// DefaultWorkerDir is injected at build time by CMake via -ldflags -X.
// It points to the directory containing zypp-mcp-* worker binaries.
// When building standalone (go build without CMake), falls back to the
// directory of the running executable.
var DefaultWorkerDir = ""
