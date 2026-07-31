// Package worker provides the bridge between the Go MCP server and the
// zypp-mcp-tool C++ binary. Each tool invocation spawns a fresh worker process.
package worker

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os/exec"
	"strconv"
	"strings"
)

// ToolDescriptor mirrors the JSON emitted by zypp-mcp-tool --list-tools.
type ToolDescriptor struct {
	Name         string          `json:"name"`
	Description  string          `json:"description"`
	InputSchema  json.RawMessage `json:"input_schema"`
	RequiresRoot bool            `json:"requires_root"`
}

// ListTools runs zypp-mcp-tool --list-tools.
// --list-tools writes plain JSON to stdout (no framing) and exits.
func ListTools(ctx context.Context, workerPath string) ([]ToolDescriptor, error) {
	out, err := exec.CommandContext(ctx, workerPath, "--list-tools").Output()
	if err != nil {
		return nil, fmt.Errorf("list-tools: %w", err)
	}
	var tools []ToolDescriptor
	if err := json.Unmarshal(out, &tools); err != nil {
		return nil, fmt.Errorf("list-tools parse: %w", err)
	}
	return tools, nil
}

// Result is the generic envelope returned by zypp-mcp-tool on stdout.
type Result struct {
	Type   string          `json:"type"`          // "result", "error", "elicitation", "progress"
	Tool   string          `json:"tool,omitempty"`
	Code   string          `json:"code,omitempty"`
	Detail string          `json:"detail,omitempty"`
	Raw    json.RawMessage `json:"-"`             // full original payload for pass-through
}

// Invoke spawns zypp-mcp-tool with the given tool name and argument,
// returning the final result frame. Intermediate frames (progress, elicitation)
// are handled via the provided callback.
//
// The onFrame callback receives every non-final frame. For elicitation frames,
// the callback must return the answer JSON to write to the worker's stdin.
// For progress/other frames it should return nil.
func Invoke(ctx context.Context, workerPath, tool, arg string, onFrame func(frame json.RawMessage) []byte) (*Result, error) {
	args := []string{"--tool", tool}
	if arg != "" {
		args = append(args, "--arg", arg)
	}

	cmd := exec.CommandContext(ctx, workerPath, args...)

	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, fmt.Errorf("stdin pipe: %w", err)
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, fmt.Errorf("stdout pipe: %w", err)
	}
	cmd.Stderr = nil // worker logs to file, not stderr

	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("start worker: %w", err)
	}

	var lastResult *Result

	for {
		body, err := readFrame(stdout)
		if err != nil || body == nil {
			break // EOF or read error — worker has exited
		}

		var envelope struct {
			Type string `json:"type"`
		}
		if err := json.Unmarshal(body, &envelope); err != nil {
			continue // skip malformed frames
		}

		switch envelope.Type {
		case "result", "error":
			var r Result
			if err := json.Unmarshal(body, &r); err != nil {
				return nil, fmt.Errorf("parse result: %w", err)
			}
			r.Raw = json.RawMessage(append([]byte(nil), body...))
			lastResult = &r

		case "elicitation":
			var answer []byte
			if onFrame != nil {
				answer = onFrame(json.RawMessage(body))
			}
			if answer == nil {
				answer = []byte(`{"answer":"decline"}`)
			}
			if err := writeFrame(stdin, answer); err != nil {
				break
			}

		case "progress":
			if onFrame != nil {
				onFrame(json.RawMessage(body))
			}
		}
	}

	if err := cmd.Wait(); err != nil {
		if lastResult != nil && lastResult.Type == "error" {
			return lastResult, nil
		}
		if lastResult == nil {
			return nil, fmt.Errorf("worker exited: %w", err)
		}
	}

	if lastResult == nil {
		return nil, fmt.Errorf("worker produced no result")
	}
	return lastResult, nil
}

// InvokeSimple is a convenience wrapper for tools that do not need elicitation.
func InvokeSimple(ctx context.Context, workerPath, tool, arg string) (json.RawMessage, error) {
	result, err := Invoke(ctx, workerPath, tool, arg, nil)
	if err != nil {
		return nil, err
	}
	if result.Type == "error" {
		return nil, fmt.Errorf("%s: %s", result.Code, strings.TrimSpace(result.Detail))
	}
	return result.Raw, nil
}

// ─── Content-Length framing ───────────────────────────────────────────────────

// readFrame reads one Content-Length framed message from r.
// Returns nil, nil on clean EOF with no data.
func readFrame(r io.Reader) ([]byte, error) {
	var hdr strings.Builder
	buf := make([]byte, 1)
	for {
		n, err := r.Read(buf)
		if n == 1 {
			hdr.WriteByte(buf[0])
			s := hdr.String()
			if len(s) >= 4 && s[len(s)-4:] == "\r\n\r\n" {
				break
			}
		}
		if err == io.EOF {
			if hdr.Len() == 0 {
				return nil, nil // clean EOF
			}
			return nil, fmt.Errorf("EOF in headers")
		}
		if err != nil {
			return nil, err
		}
	}

	var length int
	for _, line := range strings.Split(hdr.String(), "\r\n") {
		if strings.HasPrefix(line, "Content-Length: ") {
			v, err := strconv.Atoi(strings.TrimPrefix(line, "Content-Length: "))
			if err != nil {
				return nil, fmt.Errorf("invalid Content-Length: %w", err)
			}
			length = v
		}
	}
	if length == 0 {
		return nil, fmt.Errorf("missing or zero Content-Length")
	}

	body := make([]byte, length)
	if _, err := io.ReadFull(r, body); err != nil {
		return nil, fmt.Errorf("reading body: %w", err)
	}
	return body, nil
}

// writeFrame writes a Content-Length framed message to w.
func writeFrame(w io.Writer, data []byte) error {
	header := fmt.Sprintf("Content-Length: %d\r\n\r\n", len(data))
	if _, err := io.WriteString(w, header); err != nil {
		return err
	}
	_, err := w.Write(data)
	return err
}
