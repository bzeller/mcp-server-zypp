//go:build !enable_http

package main

import (
	"context"
	"fmt"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

// runHTTP is a stub: this binary was not built with HTTP transport support.
// See http_enabled.go and CMakeLists.txt: ENABLE_HTTP.
func runHTTP(_ context.Context, _ *mcp.Server) error {
	return fmt.Errorf("HTTP transport not built into this binary (rebuild with -tags enable_http, or -DENABLE_HTTP=ON via CMake)")
}

// transportUsage is surfaced in -help — must reflect what this build
// actually supports, not what the binary could support in general.
const transportUsage = "Transport mode: stdio (http not built into this binary)"
