package main

import (
	"context"
	"flag"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"

	"github.com/modelcontextprotocol/go-sdk/mcp"

	"github.com/openSUSE/mcp-server-zypp/internal/config"
	"github.com/openSUSE/mcp-server-zypp/internal/tools"
)

func main() {
	var (
		transport = flag.String("transport", "stdio", "Transport mode: stdio or http")
		addr      = flag.String("addr", ":8080", "HTTP listen address (only used with -transport=http)")
		workerDir = flag.String("worker-dir", config.DefaultWorkerDir, "Directory containing zypp-mcp-* worker binaries")
	)
	flag.Parse()

	// Fallback: if no worker dir was injected at build time or passed on the
	// command line, use the directory of the running executable.
	if *workerDir == "" {
		exe, err := os.Executable()
		if err != nil {
			slog.Error("cannot determine executable path", "err", err)
			os.Exit(1)
		}
		*workerDir = filepath.Dir(exe)
	}

	slog.SetDefault(slog.New(slog.NewJSONHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelInfo})))

	server := mcp.NewServer(&mcp.Implementation{
		Name:    "mcp-server-zypp",
		Version: "0.1.0",
	}, nil)

	if err := tools.Register(server, *workerDir); err != nil {
		slog.Error("tool discovery failed", "err", err)
		os.Exit(1)
	}

	ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer cancel()

	switch *transport {
	case "stdio":
		slog.Info("starting mcp-server-zypp", "transport", "stdio")
		if err := server.Run(ctx, &mcp.StdioTransport{}); err != nil {
			slog.Error("server exited", "err", err)
			os.Exit(1)
		}

	case "http":
		handler := mcp.NewStreamableHTTPHandler(
			func(_ *http.Request) *mcp.Server { return server },
			nil,
		)
		srv := &http.Server{Addr: *addr, Handler: handler}

		go func() {
			<-ctx.Done()
			_ = srv.Close()
		}()

		slog.Info("starting mcp-server-zypp", "transport", "http", "addr", *addr)
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			slog.Error("http server error", "err", err)
			os.Exit(1)
		}

	default:
		slog.Error("unknown transport", "transport", *transport)
		os.Exit(2)
	}
}
