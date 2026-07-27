package config

// DefaultWorkerPath is injected at build time by CMake via -ldflags -X.
// When building standalone (go build without CMake), falls back to PATH lookup.
var DefaultWorkerPath = "zypp-mcp-tool"
