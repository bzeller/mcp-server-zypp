// Package worker cancellation-latch tests.
//
// These are unit tests against invokeIO directly (not Invoke), using
// io.Pipe() to simulate the worker's stdin/stdout without spawning a real
// process. This lets us control exact frame timing relative to context
// cancellation — something an integration test against the real
// zypp-mcp-tool binary cannot reliably do.
package worker

import (
	"context"
	"encoding/json"
	"io"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// nopWriteCloser adapts an io.Writer to io.WriteCloser for tests that never
// need to observe stdin writes (elicitation is not exercised here).
type nopWriteCloser struct{ io.Writer }

func (nopWriteCloser) Close() error { return nil }

// discardStdin returns a WriteCloser that swallows any elicitation answers —
// none of the cancellation tests below use elicitation.
func discardStdin() io.WriteCloser { return nopWriteCloser{io.Discard} }

// TestInvokeIO_KillsOnCancelBeforeLatch verifies that ctx cancellation kills
// the worker when no "zypp_control"/"commit_active" frame has been seen yet —
// the default, cancellable state.
func TestInvokeIO_KillsOnCancelBeforeLatch(t *testing.T) {
	pr, pw := io.Pipe()
	ctx, cancel := context.WithCancel(context.Background())

	var killed atomic.Bool
	kill := func() { killed.Store(true) }

	done := make(chan struct{})
	go func() {
		defer close(done)
		invokeIO(ctx, discardStdin(), pr, kill, nil)
	}()

	// Write a plain progress frame — still cancellable.
	if err := writeFrame(pw, []byte(`{"type":"progress","action":"download","percent":10}`)); err != nil {
		t.Fatalf("writeFrame: %v", err)
	}

	// Cancel before any latch frame arrives, then simulate the process dying
	// as a real kill would cause (pipe closes, readFrame hits EOF).
	cancel()
	pw.Close()

	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("invokeIO did not return after cancellation")
	}

	if !killed.Load() {
		t.Error("expected kill to be called when cancelled before commit_active latch")
	}
}

// TestInvokeIO_DoesNotKillAfterLatch verifies that once a "zypp_control"/
// "commit_active" frame is seen, a later ctx cancellation does not invoke
// kill — the worker must run to completion (RPM transaction safety).
func TestInvokeIO_DoesNotKillAfterLatch(t *testing.T) {
	pr, pw := io.Pipe()
	ctx, cancel := context.WithCancel(context.Background())

	var killed atomic.Bool
	kill := func() { killed.Store(true) }

	var result *Result
	var resultErr error
	done := make(chan struct{})
	go func() {
		defer close(done)
		result, resultErr = invokeIO(ctx, discardStdin(), pr, kill, nil)
	}()

	// Latches non-cancellable — simulates McpCommitActiveReceive::start().
	if err := writeFrame(pw, []byte(`{"type":"zypp_control","event":"commit_active"}`)); err != nil {
		t.Fatalf("writeFrame: %v", err)
	}

	// Give the frame loop a moment to process and latch before cancelling.
	time.Sleep(50 * time.Millisecond)
	cancel()

	// Cancellation must not have killed anything — worker keeps running and
	// eventually produces a normal result.
	time.Sleep(50 * time.Millisecond)
	if killed.Load() {
		t.Fatal("kill was called after commit_active latch — must never happen")
	}

	if err := writeFrame(pw, []byte(`{"type":"result","tool":"confirm_install"}`)); err != nil {
		t.Fatalf("writeFrame: %v", err)
	}
	pw.Close()

	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("invokeIO did not return after result frame")
	}

	if killed.Load() {
		t.Error("kill must not have been called at any point after latch")
	}
	if resultErr != nil {
		t.Fatalf("unexpected error: %v", resultErr)
	}
	if result == nil || result.Type != "result" {
		t.Fatalf("expected result frame, got %+v", result)
	}
}

// TestInvokeIO_NoCancellationNoKill verifies the baseline: without any
// cancellation, kill is never invoked regardless of latch state.
func TestInvokeIO_NoCancellationNoKill(t *testing.T) {
	pr, pw := io.Pipe()
	ctx := context.Background()

	var killed atomic.Bool
	kill := func() { killed.Store(true) }

	var result *Result
	done := make(chan struct{})
	go func() {
		defer close(done)
		result, _ = invokeIO(ctx, discardStdin(), pr, kill, nil)
	}()

	if err := writeFrame(pw, []byte(`{"type":"zypp_control","event":"commit_active"}`)); err != nil {
		t.Fatalf("writeFrame: %v", err)
	}
	if err := writeFrame(pw, []byte(`{"type":"result","tool":"confirm_install"}`)); err != nil {
		t.Fatalf("writeFrame: %v", err)
	}
	pw.Close()

	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("invokeIO did not return")
	}

	if killed.Load() {
		t.Error("kill must not be called without cancellation")
	}
	if result == nil || result.Type != "result" {
		t.Fatalf("expected result frame, got %+v", result)
	}
}

// TestInvokeIO_CommitActiveAckWritten verifies that the "zypp_control"/
// "commit_active" frame receives an explicit ack:true written back to
// stdin — the worker's McpCommitActiveReceive::start() blocks on this
// before proceeding.
func TestInvokeIO_CommitActiveAckWritten(t *testing.T) {
	pr, pw := io.Pipe()
	stdinR, stdinW := io.Pipe()
	ctx := context.Background()

	var mu sync.Mutex
	var ackReceived []byte
	readDone := make(chan struct{})
	go func() {
		defer close(readDone)
		body, err := readFrame(stdinR)
		if err != nil {
			return
		}
		mu.Lock()
		ackReceived = body
		mu.Unlock()
	}()

	done := make(chan struct{})
	go func() {
		defer close(done)
		invokeIO(ctx, stdinW, pr, func() {}, nil)
	}()

	if err := writeFrame(pw, []byte(`{"type":"zypp_control","event":"commit_active"}`)); err != nil {
		t.Fatalf("writeFrame: %v", err)
	}

	select {
	case <-readDone:
	case <-time.After(2 * time.Second):
		t.Fatal("ack was not written to stdin")
	}

	mu.Lock()
	got := string(ackReceived)
	mu.Unlock()
	if got != `{"ack":true}` {
		t.Errorf(`expected {"ack":true} written to stdin, got %q`, got)
	}

	if err := writeFrame(pw, []byte(`{"type":"result","tool":"confirm_install"}`)); err != nil {
		t.Fatalf("writeFrame: %v", err)
	}
	pw.Close()

	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("invokeIO did not return")
	}
}

// TestInvokeIO_CommitActiveDeclinesWhenAlreadyCancelled verifies that if
// ctx is already cancelled by the time the "commit_active" frame arrives,
// the proxy replies ack:false (graceful decline) rather than ack:true —
// letting the worker unwind cleanly via TargetAbortedException instead of
// relying solely on a racing kill().
func TestInvokeIO_CommitActiveDeclinesWhenAlreadyCancelled(t *testing.T) {
	pr, pw := io.Pipe()
	stdinR, stdinW := io.Pipe()
	ctx, cancel := context.WithCancel(context.Background())
	cancel() // already cancelled before invokeIO even starts

	var mu sync.Mutex
	var ackReceived []byte
	readDone := make(chan struct{})
	go func() {
		defer close(readDone)
		body, err := readFrame(stdinR)
		if err != nil {
			return
		}
		mu.Lock()
		ackReceived = body
		mu.Unlock()
	}()

	// kill is a no-op here — this test only asserts the ack content, not
	// the watcher-goroutine kill path (covered separately).
	done := make(chan struct{})
	go func() {
		defer close(done)
		invokeIO(ctx, stdinW, pr, func() {}, nil)
	}()

	if err := writeFrame(pw, []byte(`{"type":"zypp_control","event":"commit_active"}`)); err != nil {
		t.Fatalf("writeFrame: %v", err)
	}

	select {
	case <-readDone:
	case <-time.After(2 * time.Second):
		t.Fatal("ack was not written to stdin")
	}

	mu.Lock()
	got := string(ackReceived)
	mu.Unlock()
	if got != `{"ack":false}` {
		t.Errorf(`expected {"ack":false} written to stdin when ctx already cancelled, got %q`, got)
	}

	pw.Close()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("invokeIO did not return")
	}
}

// TestInvokeIO_ElicitationRoundTrip verifies onFrame's answer is written back
// to stdin as a properly framed message, independent of the cancellation logic.
func TestInvokeIO_ElicitationRoundTrip(t *testing.T) {
	pr, pw := io.Pipe()
	stdinR, stdinW := io.Pipe()
	ctx := context.Background()

	onFrame := func(frame json.RawMessage) []byte {
		return []byte(`{"answer":"accept"}`)
	}

	var mu sync.Mutex
	var stdinReceived []byte
	readDone := make(chan struct{})
	go func() {
		defer close(readDone)
		body, err := readFrame(stdinR)
		if err != nil {
			return
		}
		mu.Lock()
		stdinReceived = body
		mu.Unlock()
	}()

	done := make(chan struct{})
	go func() {
		defer close(done)
		invokeIO(ctx, stdinW, pr, func() {}, onFrame)
	}()

	if err := writeFrame(pw, []byte(`{"type":"elicitation","method":"trust_key","data":{}}`)); err != nil {
		t.Fatalf("writeFrame: %v", err)
	}

	select {
	case <-readDone:
	case <-time.After(2 * time.Second):
		t.Fatal("stdin answer was not written")
	}

	mu.Lock()
	got := string(stdinReceived)
	mu.Unlock()
	if got != `{"answer":"accept"}` {
		t.Errorf("expected accept answer written to stdin, got %q", got)
	}

	if err := writeFrame(pw, []byte(`{"type":"result","tool":"confirm_install"}`)); err != nil {
		t.Fatalf("writeFrame: %v", err)
	}
	pw.Close()

	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("invokeIO did not return")
	}
}
