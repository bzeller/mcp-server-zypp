//go:build enable_http

package main

import (
	"context"
	"flag"
	"net/http"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

// -addr only exists in this build — it is meaningless without HTTP support,
// so it must not be registered (and therefore not shown in -help) when
// built without -tags enable_http.
var httpAddr = flag.String("addr", ":8080", "HTTP listen address (only used with -transport=http)")

// runHTTP serves the MCP Streamable HTTP transport until ctx is cancelled.
// Debugging only — no authentication, must never be bound to a non-loopback
// interface. Built only when compiled with -tags enable_http (see
// CMakeLists.txt: ENABLE_HTTP option).
func runHTTP(ctx context.Context, server *mcp.Server) error {
	handler := mcp.NewStreamableHTTPHandler(
		func(_ *http.Request) *mcp.Server { return server },
		nil,
	)
	srv := &http.Server{Addr: *httpAddr, Handler: handler}

	go func() {
		<-ctx.Done()
		_ = srv.Close()
	}()

	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		return err
	}
	return nil
}

// transportUsage is surfaced in -help — must reflect what this build
// actually supports, not what the binary could support in general.
const transportUsage = "Transport mode: stdio or http"
