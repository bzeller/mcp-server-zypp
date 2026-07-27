// Package tools registers all MCP tool definitions with the server.
// Tool metadata (name, description, input schema) is discovered at startup
// by running zypp-mcp-tool --list-tools — the C++ binary is the single source
// of truth. No tool definitions live in Go.
package tools

import (
	"context"
	"encoding/json"
	"fmt"

	"github.com/modelcontextprotocol/go-sdk/mcp"

	"github.com/openSUSE/mcp-server-zypp/internal/worker"
)

// Register discovers tools from the worker binary and registers them with the
// MCP server. Returns an error if discovery fails — the proxy must not start
// with an empty or stale tool set.
func Register(server *mcp.Server, workerPath string) error {
	descriptors, err := worker.ListTools(context.Background(), workerPath)
	if err != nil {
		return fmt.Errorf("discover tools from worker: %w", err)
	}
	if len(descriptors) == 0 {
		return fmt.Errorf("worker returned no tools")
	}

	for _, td := range descriptors {
		td := td // capture for closure
		server.AddTool(
			&mcp.Tool{
				Name:        td.Name,
				Description: td.Description,
				InputSchema: td.InputSchema, // json.RawMessage passes straight through
			},
			func(ctx context.Context, req *mcp.CallToolRequest) (*mcp.CallToolResult, error) {
				result, err := worker.Invoke(
					ctx,
					workerPath,
					td.Name,
					string(req.Params.Arguments), // forward raw JSON arg verbatim
					makeElicitationHandler(ctx, req),
				)
				if err != nil {
					return errorResult(err), nil
				}
				if result.Type == "error" {
					return errorResult(fmt.Errorf("%s: %s", result.Code, result.Detail)), nil
				}
				return jsonResult(result.Raw), nil
			},
		)
	}
	return nil
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

// makeElicitationHandler bridges MCP elicitation/create ↔ worker stdin.
// For elicitation frames the worker blocks on stdin waiting for an answer;
// we use the MCP session's Elicit() to ask the human and return their answer.
func makeElicitationHandler(ctx context.Context, req *mcp.CallToolRequest) func(json.RawMessage) []byte {
	return func(frame json.RawMessage) []byte {
		var envelope struct {
			Type   string          `json:"type"`
			Method string          `json:"method"`
			Data   json.RawMessage `json:"data"`
		}
		if err := json.Unmarshal(frame, &envelope); err != nil || envelope.Type != "elicitation" {
			return nil // progress or malformed — no stdin response
		}

		session := req.Session
		if session == nil {
			return []byte(`{"answer":"decline"}`) // fail closed
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
}
