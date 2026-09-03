package main

import (
        "context"
	"flag"
	"log/slog"
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
	        transport = flag.String("transport", "stdio", transportUsage)
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
		Version: "0.1.2",
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
	        // runHTTP is implemented in http_enabled.go or http_disabled.go,
		// selected at build time via the enable_http build tag (see
		// CMakeLists.txt: ENABLE_HTTP option). HTTP is for local debugging
		// only and is not compiled into the binary by default.
		slog.Info("starting mcp-server-zypp", "transport", "http")
		if err := runHTTP(ctx, server); err != nil {
		        slog.Error("http server error", "err", err)
			os.Exit(1)
		}

        default:
	        slog.Error("unknown transport", "transport", *transport)
		os.Exit(2)
	}
}
