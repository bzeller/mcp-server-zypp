// Package tools registers all MCP tool definitions with the server.
// Tool metadata (name, description, input schema, requires_root) is discovered
// at startup by running each zypp-mcp-* binary in the worker directory with
// --list-tools. The C++ binaries are the single source of truth — no tool
// definitions live in Go.
package tools

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/modelcontextprotocol/go-sdk/mcp"

	"github.com/openSUSE/mcp-server-zypp/internal/worker"
)

// registeredTool composes an mcp.Tool with the binary path that implements it.
// The handle method is the single shared dispatch function — no per-tool closures.
type registeredTool struct {
	mcp.Tool
	binaryPath string
}

func (rt *registeredTool) handle(ctx context.Context, req *mcp.CallToolRequest) (*mcp.CallToolResult, error) {
	result, err := worker.Invoke(
		ctx,
		rt.binaryPath,
		rt.Tool.Name,
		string(req.Params.Arguments),
		makeFrameHandler(ctx, req),
	)
	if err != nil {
		return errorResult(err), nil
	}
	if result.Type == "error" {
		return errorResult(fmt.Errorf("%s: %s", result.Code, result.Detail)), nil
	}
	return jsonResult(result.Raw), nil
}

// Register discovers all zypp-mcp-* binaries in workerDir, queries their
// tool lists, applies the requires_root filter based on the current uid, and
// registers the surviving tools with the MCP server.
//
// Duplicate tool names across binaries are a fatal error — the LLM cannot
// disambiguate, and silent shadowing would be a correctness bug.
func Register(server *mcp.Server, workerDir string) error {
	isRoot := os.Getuid() == 0

	binaries, err := discoverWorkers(workerDir)
	if err != nil {
		return fmt.Errorf("discover worker binaries in %s: %w", workerDir, err)
	}
	if len(binaries) == 0 {
		return fmt.Errorf("no zypp-mcp-* binaries found in %s", workerDir)
	}

	// registered maps tool name → registeredTool for duplicate detection.
	registered := make(map[string]*registeredTool)

	for _, binaryPath := range binaries {
		descriptors, err := worker.ListTools(context.Background(), binaryPath)
		if err != nil {
			return fmt.Errorf("list-tools from %s: %w", binaryPath, err)
		}

		for _, td := range descriptors {
			// Root gate: skip root-only tools when not running as root.
			if td.RequiresRoot && !isRoot {
				continue
			}

			// Duplicate detection across all binaries.
			if existing, exists := registered[td.Name]; exists {
				return fmt.Errorf("duplicate tool %q registered by both %s and %s",
					td.Name, existing.binaryPath, binaryPath)
			}

			rt := &registeredTool{
				Tool: mcp.Tool{
					Name:        td.Name,
					Description: td.Description,
					InputSchema: td.InputSchema,
				},
				binaryPath: binaryPath,
			}
			registered[td.Name] = rt
			server.AddTool(&rt.Tool, rt.handle)
		}
	}

	if len(registered) == 0 {
		return fmt.Errorf("no tools registered (all filtered by root gate?)")
	}
	return nil
}

// discoverWorkers returns the paths of all zypp-mcp-* executables in dir,
// excluding *-tests binaries. A CMake build tree places the C++ unit test
// binaries (zypp-mcp-tool-tests, zypp-mcp-gpgkeygate-tests — see
// worker/CMakeLists.txt) in the same directory as zypp-mcp-tool itself; they
// match the zypp-mcp- prefix but don't implement --list-tools, which would
// otherwise make RegisterAll's tool discovery fail outright and prevent the
// proxy from starting when pointed at a build-tree worker dir.
func discoverWorkers(dir string) ([]string, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}
	var binaries []string
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		if !strings.HasPrefix(e.Name(), "zypp-mcp-") {
			continue
		}
		if strings.HasSuffix(e.Name(), "-tests") {
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		if info.Mode()&0111 == 0 {
			continue
		}
		binaries = append(binaries, filepath.Join(dir, e.Name()))
	}
	return binaries, nil
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

func jsonResult(raw json.RawMessage) *mcp.CallToolResult {
	return &mcp.CallToolResult{
		Content: []mcp.Content{&mcp.TextContent{Text: string(raw)}},
	}
}

func errorResult(err error) *mcp.CallToolResult {
	return &mcp.CallToolResult{
		Content: []mcp.Content{&mcp.TextContent{Text: err.Error()}},
		IsError: true,
	}
}

// makeFrameHandler bridges worker stdout frames to MCP.
//
// Two frame types are handled:
//   - "elicitation": blocks the worker via stdin, answered through the MCP
//     elicitation/create flow.
//   - "progress": forwarded as a real MCP notifications/progress message.
//     Per the MCP spec, progress notifications are only sent if the client
//     attached a progressToken to the original tools/call request — if it
//     didn't, there is nothing to correlate the notification with, so we
//     silently drop progress frames in that case (not an error).
//
// All other frame types are ignored.
func makeFrameHandler(ctx context.Context, req *mcp.CallToolRequest) func(json.RawMessage) []byte {
	progressToken := req.Params.GetProgressToken()

	return func(frame json.RawMessage) []byte {
		var envelope struct {
			Type string `json:"type"`
		}
		if err := json.Unmarshal(frame, &envelope); err != nil {
			return nil
		}

		switch envelope.Type {
		case "elicitation":
			return handleElicitation(ctx, req, frame)
		case "progress":
			handleProgress(ctx, req, progressToken, frame)
			return nil
		default:
			return nil
		}
	}
}

// handleElicitation bridges MCP elicitation/create ↔ worker stdin.
func handleElicitation(ctx context.Context, req *mcp.CallToolRequest, frame json.RawMessage) []byte {
	var envelope struct {
		Method string          `json:"method"`
		Data   json.RawMessage `json:"data"`
	}
	if err := json.Unmarshal(frame, &envelope); err != nil {
		return nil
	}

	session := req.Session
	if session == nil {
		return []byte(`{"answer":"decline"}`)
	}

	elicitResult, err := session.Elicit(ctx, &mcp.ElicitParams{
		Message: fmt.Sprintf("[%s] %s", envelope.Method, string(envelope.Data)),
		Mode:    "form",
		RequestedSchema: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"answer": map[string]any{"type": "string"},
			},
		},
	})
	if err != nil || elicitResult == nil || elicitResult.Action != "accept" {
		return []byte(`{"answer":"decline"}`)
	}

	answer, _ := elicitResult.Content["answer"].(string)
	if answer == "" {
		answer = "decline"
	}
	resp, _ := json.Marshal(map[string]string{"answer": answer})
	return resp
}

// progressFrame mirrors the JSON emitted by McpInstallReceive/McpRemoveReceive
// in callbacks.cc — see worker/src/callbacks.cc.
type progressFrame struct {
	Action   string `json:"action"`
	Package  string `json:"package,omitempty"`
	Edition  string `json:"edition,omitempty"`
	Percent  *int   `json:"percent,omitempty"`
	Finished bool   `json:"finished,omitempty"`
	Error    bool   `json:"error,omitempty"`
}

// handleProgress forwards a worker progress frame as an MCP
// notifications/progress message. No-op if the client did not request
// progress (no progressToken) or if the session is unavailable. Errors from
// the notification itself are logged, not surfaced — progress is best-effort
// and must never fail the underlying tool call.
func handleProgress(ctx context.Context, req *mcp.CallToolRequest, progressToken any, frame json.RawMessage) {
	if progressToken == nil || req.Session == nil {
		return
	}

	var pf progressFrame
	if err := json.Unmarshal(frame, &pf); err != nil {
		return
	}

	var message string
	switch {
	case pf.Finished && pf.Error:
		message = fmt.Sprintf("%s failed", pf.Action)
	case pf.Finished:
		message = fmt.Sprintf("%s finished", pf.Action)
	case pf.Package != "":
		message = fmt.Sprintf("%s %s", pf.Action, pf.Package)
	default:
		message = pf.Action
	}

	percent := 0.0
	if pf.Percent != nil {
		percent = float64(*pf.Percent)
	} else if pf.Finished {
		percent = 100
	}

	_ = req.Session.NotifyProgress(ctx, &mcp.ProgressNotificationParams{
		ProgressToken: progressToken,
		Message:       message,
		Progress:      percent,
		Total:         100,
	})
}
