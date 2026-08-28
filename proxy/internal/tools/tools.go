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
		// No frame exists at all here (the worker itself failed to run,
		// or the frame stream was malformed) — the Go error string is
		// genuinely all there is to report.
		return errorResult(err), nil
	}
	if result.Type == "error" {
		// Pass the whole frame through, exactly as the success path
		// below does. The worker's error frames (see
		// worker/src/tools/transaction.cc: commitFailureToJson()) carry
		// the actual diagnosis in "details"/"failed_installs"/
		// "skipped_installs"/etc. - collapsing that down to just
		// code+detail (as this used to do) discards everything
		// CommitFailureLog exists to capture. The proxy is a protocol
		// bridge and makes no decisions about this content, so it must
		// not summarize it either.
		return errorFrameResult(result.Raw), nil
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

// errorResult is for transport-level failures only - cases where no worker
// frame exists at all (worker.Invoke itself returned a Go error), so the
// error string is genuinely all there is to report. For a worker-produced
// error frame, use errorFrameResult instead to preserve its full structure.
func errorResult(err error) *mcp.CallToolResult {
	return &mcp.CallToolResult{
		Content: []mcp.Content{&mcp.TextContent{Text: err.Error()}},
		IsError: true,
	}
}

// errorFrameResult passes a worker error frame through verbatim, marked as
// an error for the MCP client. See its call site in handle() for why this
// must not summarize the frame down to a shorter string.
func errorFrameResult(raw json.RawMessage) *mcp.CallToolResult {
	return &mcp.CallToolResult{
		Content: []mcp.Content{&mcp.TextContent{Text: string(raw)}},
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
	// err (a genuine transport/client failure) and "the client declined"
	// are deliberately indistinguishable here — both fail closed to
	// decline, which is the correct security posture (see GpgKeyGate: a
	// client that can't or won't answer must never be treated as having
	// approved). The cost is that err itself is silently discarded, so a
	// real transport problem looks identical to a human clicking
	// "decline" from this code's point of view. Revisit only if that
	// ambiguity becomes a real diagnostic problem in practice.
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

// progressFrame mirrors the JSON emitted by every progress-reporting
// receiver in callbacks.cc — the classic McpInstallReceive/McpRemoveReceive/
// McpDownloadReceive/McpCommitPreloadReceive, and the SingleTrans-backend
// counterparts (McpInstallSAReceive, McpRemoveSAReceive,
// McpCommitScriptSAReceive, McpTransactionSAReceive, McpCleanupSAReceive,
// McpSingleTransReceive). Not every receiver sets every field — see
// progressMessage for which fields apply to which "action".
type progressFrame struct {
	Action     string `json:"action"`
	Package    string `json:"package,omitempty"`
	Edition    string `json:"edition,omitempty"`
	Percent    *int   `json:"percent,omitempty"`
	Finished   bool   `json:"finished,omitempty"`
	Error      bool   `json:"error,omitempty"`
	Cached     bool   `json:"cached,omitempty"`
	RpmOutput  string `json:"rpm_output,omitempty"`
	Level      string `json:"level,omitempty"`
	Line       string `json:"line,omitempty"`
	ScriptType string `json:"script_type,omitempty"`
	Severity   string `json:"severity,omitempty"`
	Name       string `json:"name,omitempty"`

	// McpCommitPreloadReceive fields (worker/src/callbacks.cc) - the
	// overall concurrent preload of all commit downloads, distinct from
	// the per-package "download" action above.
	Started       bool     `json:"started,omitempty"`
	File          string   `json:"file,omitempty"`
	URL           string   `json:"url,omitempty"`
	DbpsAvg       *float64 `json:"dbps_avg,omitempty"`
	DbpsCurrent   *float64 `json:"dbps_current,omitempty"`
	BytesReceived *float64 `json:"bytes_received,omitempty"`
	BytesRequired *float64 `json:"bytes_required,omitempty"`
}

// rpmLoglinePrefix mirrors SingleTransReport::loglevelPrefix() (ZYppCallbacks.h)
// — reusing libzypp's own suggested rendering for a given level rather than
// inventing a separate one here.
var rpmLoglinePrefix = map[string]string{
	"critical": "fatal error: ",
	"error":    "error: ",
	"warning":  "warning: ",
	"info":     "",
	"debug":    "D: ",
}

// progressMessage builds the human-readable message and progress/total pair
// for a notifications/progress message from a worker progress frame, or
// returns ok=false if the frame carries nothing worth forwarding.
//
// Pure and side-effect free by design, unlike handleProgress itself, so it
// can be unit tested directly without a live MCP session.
//
// Log-line frames (rpm_output, rpm_log) carry no percent at all — they are
// pure informational text, not tied to a specific step's completion. Rather
// than tracking state across calls to "carry forward" a percent (this
// function, and the rest of the progress path, is deliberately stateless —
// see handleProgress's doc comment on best-effort semantics), they are
// reported with progress=0/total=0: a 0/0 ratio distinct from 0/100,
// signaling "no percent applicable" rather than a spurious regression to 0%
// on an otherwise-advancing progress bar.
func progressMessage(pf progressFrame) (message string, percent, total float64, ok bool) {
	// Raw output takes priority over action-specific formatting regardless
	// of which step it's attached to (install/remove/script/transaction) —
	// the line itself is the entire informational content of the frame.
	if pf.RpmOutput != "" {
		return pf.RpmOutput, 0, 0, true
	}

	switch pf.Action {
	case "rpm_log":
		if pf.Line == "" {
			return "", 0, 0, false
		}
		return rpmLoglinePrefix[pf.Level] + pf.Line, 0, 0, true

	case "script":
		switch {
		case pf.Finished && pf.Severity == "critical":
			message = fmt.Sprintf("%s script failed", pf.ScriptType)
		case pf.Finished && pf.Severity == "warning":
			message = fmt.Sprintf("%s script warning", pf.ScriptType)
		case pf.Finished:
			message = fmt.Sprintf("%s script finished", pf.ScriptType)
		default:
			message = fmt.Sprintf("Executing %s script", pf.ScriptType)
		}
		if pf.Package != "" {
			message += ": " + pf.Package
		}
		return message, percentOf(pf), 100, true

	case "transaction":
		name := pf.Name
		if name == "" {
			name = pf.Action
		}
		switch {
		case pf.Finished && pf.Error:
			message = fmt.Sprintf("%s failed", name)
		case pf.Finished:
			message = fmt.Sprintf("%s finished", name)
		default:
			message = name
		}
		return message, percentOf(pf), 100, true

	case "preload":
		switch {
		case pf.Finished && pf.Error:
			message = "preload failed"
		case pf.Finished:
			message = "preload finished"
		case pf.Started:
			message = "preload started"
		case pf.File != "":
			message = fmt.Sprintf("preload: %s", pf.File)
		default:
			message = pf.Action
		}
		// Prefer the byte counters when both are present and meaningful —
		// a more accurate progress signal than the percent field for a
		// download that can span many packages at once. Falls back to the
		// same percentOf()/100 pair every other action uses otherwise, so
		// a bare "preload" frame with none of these fields set (e.g. the
		// zero-value case) behaves exactly as before this field was added.
		if pf.BytesReceived != nil && pf.BytesRequired != nil && *pf.BytesRequired > 0 {
			return message, *pf.BytesReceived, *pf.BytesRequired, true
		}
		return message, percentOf(pf), 100, true

	default:
		// Covers install/remove/download/cleanup — cleanup's Package
		// field holds an NVRA string, which the generic "%s %s" (action,
		// package) branch already renders sensibly.
		switch {
		case pf.Finished && pf.Error:
			message = fmt.Sprintf("%s failed", pf.Action)
		case pf.Finished:
			message = fmt.Sprintf("%s finished", pf.Action)
		case pf.Cached:
			message = fmt.Sprintf("%s %s (already cached)", pf.Action, pf.Package)
		case pf.Package != "":
			message = fmt.Sprintf("%s %s", pf.Action, pf.Package)
		default:
			message = pf.Action
		}
		return message, percentOf(pf), 100, true
	}
}

func percentOf(pf progressFrame) float64 {
	if pf.Percent != nil {
		return float64(*pf.Percent)
	}
	if pf.Finished {
		return 100
	}
	return 0
}

// handleProgress forwards a worker progress frame as an MCP
// notifications/progress message. No-op if the client did not request
// progress (no progressToken) or if the session is unavailable, or if the
// frame carries nothing worth forwarding (progressMessage's ok=false).
// Errors from the notification itself are ignored — progress is best-effort
// and must never fail the underlying tool call.
func handleProgress(ctx context.Context, req *mcp.CallToolRequest, progressToken any, frame json.RawMessage) {
	if progressToken == nil || req.Session == nil {
		return
	}

	var pf progressFrame
	if err := json.Unmarshal(frame, &pf); err != nil {
		return
	}

	message, percent, total, ok := progressMessage(pf)
	if !ok {
		return
	}

	_ = req.Session.NotifyProgress(ctx, &mcp.ProgressNotificationParams{
		ProgressToken: progressToken,
		Message:       message,
		Progress:      percent,
		Total:         total,
	})
}
