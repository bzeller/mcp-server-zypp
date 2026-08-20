// Package worker provides the bridge between the Go MCP server and the
// zypp-mcp-tool C++ binary. Each tool invocation spawns a fresh worker process.
package worker

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

// gracefulKillGracePeriod bounds how long invokeIO waits, after sending
// SIGTERM, before escalating to SIGKILL — purely a liveness backstop for a
// worker wedged before the point of no return (e.g. a stuck connection),
// not a safety mechanism (see invokeIO's watcher goroutine for the actual
// safety invariant). Comfortably exceeds the ~500ms progress-tick interval
// (worker/src/... commitpackagepreloader.cc's throttle) so the worker gets
// several chances to observe the SIGTERM-set cancellation flag and unwind
// cooperatively before escalation is even considered.
const gracefulKillGracePeriod = 5 * time.Second

// killer abstracts the two-stage stop (SIGTERM, then SIGKILL as a last
// resort) so invokeIO can own escalation policy — including the
// point-of-no-return re-check — while remaining unit-testable against a
// fake, with no real subprocess involved.
type killer interface {
	// Terminate asks the worker to stop cooperatively (SIGTERM). The
	// worker aborts at its next cancellation poll point (download/preload
	// progress — see worker/src/cancellation.h) and exits on its own.
	Terminate() error
	// Kill forcibly stops the worker (SIGKILL). Last resort only.
	Kill() error
}

// processKiller implements killer against a real *os.Process.
type processKiller struct{ proc *os.Process }

func (p processKiller) Terminate() error { return p.proc.Signal(syscall.SIGTERM) }
func (p processKiller) Kill() error      { return p.proc.Kill() }

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

// Invoke spawns the worker binary with the given tool name and argument,
// returning the final result frame. Intermediate frames (progress, elicitation)
// are handled via the provided callback.
//
// The onFrame callback receives every non-final frame. For elicitation frames,
// the callback must return the answer JSON to write to the worker's stdin.
// For progress/other frames it should return nil.
//
// Cancellation: when ctx is cancelled, the worker is sent SIGTERM and given
// gracefulKillGracePeriod to exit on its own — a chance to abort cleanly at
// its next download/preload poll point (worker/src/cancellation.h) — before
// being escalated to SIGKILL as a liveness backstop. This is entirely
// suppressed once a "zypp_control"/"commit_active" frame has been received —
// that frame marks the point at which the RPM transaction becomes
// irreversible (see worker/src/callbacks.cc: McpCommitActiveReceive). Once
// seen, cancellation is silently ignored (neither signal is ever sent) and
// the worker runs to completion. This supports the confirm_install/
// confirm_remove transaction phase, which must never be interrupted once
// started. See invokeIO for the full protocol.
func Invoke(ctx context.Context, workerPath, tool, arg string, onFrame func(frame json.RawMessage) []byte) (*Result, error) {
	args := []string{"--tool", tool}
	if arg != "" {
		args = append(args, "--arg", arg)
	}

	// Use plain exec.Command — not exec.CommandContext — so invokeIO controls
	// exactly when and whether to kill the subprocess (see invokeIO doc).
	cmd := exec.Command(workerPath, args...)

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

	result, ioErr := invokeIO(ctx, stdin, stdout, processKiller{cmd.Process}, gracefulKillGracePeriod, onFrame)

	if err := cmd.Wait(); err != nil {
		if result != nil && result.Type == "error" {
			return result, nil
		}
		if result == nil {
			return nil, fmt.Errorf("worker exited: %w", err)
		}
	}

	if ioErr != nil {
		return nil, ioErr
	}
	if result == nil {
		return nil, fmt.Errorf("worker produced no result")
	}
	return result, nil
}

// invokeIO drives the frame protocol over an already-running worker's stdin/
// stdout, independent of how that worker was started. Split out from Invoke
// so the cancellation latch and frame loop can be unit tested against plain
// io.Pipe()s instead of a real subprocess.
//
// k is used when ctx is cancelled and no "zypp_control"/"commit_active"
// frame has been seen yet: k.Terminate() (SIGTERM) is sent first, giving the
// worker a chance to abort cooperatively at its next download/preload poll
// point (worker/src/cancellation.h) and exit on its own. k.Kill() (SIGKILL)
// is only a liveness backstop for a worker that does not exit within
// gracePeriod — see the watcher goroutine below for the safety invariant
// that governs whether it is ever actually invoked.
//
// On the "commit_active" frame, an explicit ack:true/ack:false is written
// back to the worker's stdin (unblocking its synchronous start() handshake —
// see worker/src/callbacks.cc: McpCommitActiveReceive), and if ctx was not
// already cancelled, cancellation is permanently latched off for the
// remainder of this call — k is not invoked even if ctx fires later. This
// is a one-way latch by design: it exists to protect an in-progress RPM
// transaction, which must run to completion once started.
//
// If ctx was already cancelled by the time the frame arrives, an ack:false
// is sent instead — letting the worker decline and unwind cleanly rather
// than relying on a racing Kill(). The latch is deliberately left untouched
// in that branch, so the ctx.Done() watcher below still fires as a backup
// in case the worker does not honor the decline promptly.
//
// The ack is written strictly after the mutex update above, and the worker
// blocks on reading it before proceeding into the transaction — this closes
// the race where ctx could fire in the (arbitrarily small) window between
// the worker writing its frame and the proxy processing it: either
// Terminate/Kill races in first (safe — worker hasn't entered the
// transaction yet, it's still blocked on the ack read) or the latch applies
// first (safe — k is permanently suppressed before the worker is released
// to proceed).
//
// Returns the last "result"/"error" frame seen, or an error if the frame
// stream itself was malformed (never for "worker declined to be killed" —
// that is not an error, just an accepted outcome).
func invokeIO(ctx context.Context, stdin io.WriteCloser, stdout io.Reader, k killer, gracePeriod time.Duration, onFrame func(frame json.RawMessage) []byte) (*Result, error) {
	// stillCancellable is latched to false on the "zypp_control"/
	// "commit_active" frame. Protected by mu.
	var mu sync.Mutex
	stillCancellable := true

	// workerDone is closed when the frame-reading loop exits, letting the
	// cancellation watcher know the process is already finished — no point
	// signaling something that has already exited.
	workerDone := make(chan struct{})

	go func() {
		select {
		case <-ctx.Done():
			mu.Lock()
			canStop := stillCancellable
			mu.Unlock()
			if !canStop {
				return
			}

			_ = k.Terminate()

			select {
			case <-workerDone:
				return // exited on its own within the grace period
			case <-time.After(gracePeriod):
			}

			// Grace period elapsed with no exit — before escalating,
			// re-check the latch rather than trusting the timer alone.
			// Under the current ctx.Err()-based decline logic below,
			// this can never actually observe a different value than
			// the check above (ctx.Err() becomes permanently non-nil in
			// the same atomic step that closes ctx.Done(), which is
			// what woke this goroutine up in the first place — so the
			// frame loop can never subsequently see "not yet cancelled"
			// either; see worker_cancel_test.go's note above
			// TestInvokeIO_NoCancellationNoKill for the full argument).
			// Kept anyway: SIGKILL must never land once the transaction
			// may have started, and re-checking here makes that a local
			// invariant enforced at the kill site itself, rather than
			// an assumption resting on the ack-decision logic elsewhere
			// in this function never changing.
			mu.Lock()
			stillCanKill := stillCancellable
			mu.Unlock()
			if stillCanKill {
				_ = k.Kill()
			}
		case <-workerDone:
		}
	}()
	defer close(workerDone)

	var lastResult *Result

	for {
		body, err := readFrame(stdout)
		if err != nil || body == nil {
			break // EOF or read error — worker has exited
		}

		var envelope struct {
			Type  string `json:"type"`
			Event string `json:"event"`
		}
		if err := json.Unmarshal(body, &envelope); err != nil {
			continue // skip malformed frames
		}

		// The sole, authoritative point-of-no-return signal — see
		// worker/src/callbacks.cc: McpCommitActiveReceive::start(). The
		// worker blocks reading stdin for this ack before proceeding into
		// the actual RPM transaction, and treats a missing/false ack as
		// "abort before touching anything."
		if envelope.Type == "zypp_control" && envelope.Event == "commit_active" {
			mu.Lock()
			// Prefer a graceful decline (ack:false, letting the worker
			// unwind cleanly via TargetAbortedException) over relying on a
			// racing SIGKILL, if cancellation was already requested by the
			// time this checkpoint was reached. Either outcome is safe —
			// the transaction has not yet started — but a graceful abort
			// allows normal stack unwinding where a hard kill would not.
			// The ctx.Done() watcher goroutine still fires as a backup in
			// this branch (stillCancellable is left true), in case the
			// worker does not honor the decline promptly.
			proceed := ctx.Err() == nil
			if proceed {
				stillCancellable = false
			}
			mu.Unlock()

			if proceed {
				_ = writeFrame(stdin, []byte(`{"ack":true}`))
			} else {
				_ = writeFrame(stdin, []byte(`{"ack":false}`))
			}
			continue
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
