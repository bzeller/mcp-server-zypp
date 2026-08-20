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

// fakeKiller records Terminate/Kill calls for assertions, without touching
// any real process. Safe for concurrent use — invokeIO's watcher goroutine
// and the test's assertions run concurrently.
type fakeKiller struct {
	mu         sync.Mutex
	terminated bool
	killed     bool
}

func (f *fakeKiller) Terminate() error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.terminated = true
	return nil
}

func (f *fakeKiller) Kill() error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.killed = true
	return nil
}

func (f *fakeKiller) wasTerminated() bool {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.terminated
}

func (f *fakeKiller) wasKilled() bool {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.killed
}

// testGracePeriod is short so tests run quickly and deterministically —
// production uses gracefulKillGracePeriod (5s).
const testGracePeriod = 80 * time.Millisecond

// TestInvokeIO_TerminatesOnCancelBeforeLatch verifies that ctx cancellation
// sends SIGTERM (Terminate) when no "zypp_control"/"commit_active" frame has
// been seen yet — the default, cancellable state — and that SIGKILL (Kill)
// is not needed when the worker exits promptly afterward.
func TestInvokeIO_TerminatesOnCancelBeforeLatch(t *testing.T) {
	pr, pw := io.Pipe()
	ctx, cancel := context.WithCancel(context.Background())

	k := &fakeKiller{}

	done := make(chan struct{})
	go func() {
		defer close(done)
		invokeIO(ctx, discardStdin(), pr, k, testGracePeriod, nil)
	}()

	// Write a plain progress frame — still cancellable.
	if err := writeFrame(pw, []byte(`{"type":"progress","action":"download","percent":10}`)); err != nil {
		t.Fatalf("writeFrame: %v", err)
	}

	// Cancel before any latch frame arrives, then simulate the process
	// exiting promptly in response to SIGTERM (pipe closes, readFrame hits
	// EOF) — well within the grace period.
	cancel()
	pw.Close()

	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("invokeIO did not return after cancellation")
	}

	if !k.wasTerminated() {
		t.Error("expected Terminate to be called when cancelled before commit_active latch")
	}
	if k.wasKilled() {
		t.Error("Kill should not be called when the worker exits within the grace period")
	}
}

// TestInvokeIO_EscalatesToKillIfWorkerDoesNotExit verifies that a worker
// which does not exit within the grace period after SIGTERM is escalated to
// SIGKILL — the liveness backstop.
func TestInvokeIO_EscalatesToKillIfWorkerDoesNotExit(t *testing.T) {
	pr, pw := io.Pipe()
	ctx, cancel := context.WithCancel(context.Background())

	k := &fakeKiller{}

	done := make(chan struct{})
	go func() {
		defer close(done)
		invokeIO(ctx, discardStdin(), pr, k, testGracePeriod, nil)
	}()

	if err := writeFrame(pw, []byte(`{"type":"progress","action":"download","percent":10}`)); err != nil {
		t.Fatalf("writeFrame: %v", err)
	}

	// Cancel but do NOT close the pipe — simulates a worker that does not
	// respond to SIGTERM (e.g. wedged before ever reaching a poll point).
	cancel()

	// Wait comfortably past the grace period, then assert escalation.
	time.Sleep(testGracePeriod * 3)
	if !k.wasTerminated() {
		t.Error("expected Terminate to be called")
	}
	if !k.wasKilled() {
		t.Error("expected Kill to be called after the worker failed to exit within the grace period")
	}

	// Let invokeIO actually return so the test doesn't leak goroutines —
	// simulates the (real) SIGKILL finally taking effect.
	pw.Close()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("invokeIO did not return after simulated SIGKILL")
	}
}

// TestInvokeIO_DoesNotKillAfterLatch verifies that once a "zypp_control"/
// "commit_active" frame has been latched (proceed), a later ctx cancellation
// invokes neither Terminate nor Kill — the worker must run to completion
// (RPM transaction safety). This is the primary safety check: the decision
// not to touch the process at all is made *before* Terminate is ever
// called, at the very first stillCancellable read in the watcher goroutine.
func TestInvokeIO_DoesNotKillAfterLatch(t *testing.T) {
	pr, pw := io.Pipe()
	ctx, cancel := context.WithCancel(context.Background())

	k := &fakeKiller{}

	var result *Result
	var resultErr error
	done := make(chan struct{})
	go func() {
		defer close(done)
		result, resultErr = invokeIO(ctx, discardStdin(), pr, k, testGracePeriod, nil)
	}()

	// Latches non-cancellable — simulates McpCommitActiveReceive::start()
	// succeeding (ctx not yet cancelled at the time this frame is processed).
	if err := writeFrame(pw, []byte(`{"type":"zypp_control","event":"commit_active"}`)); err != nil {
		t.Fatalf("writeFrame: %v", err)
	}

	// Give the frame loop a moment to process and latch before cancelling.
	time.Sleep(50 * time.Millisecond)
	cancel()

	// Wait past the grace period — neither Terminate nor Kill must ever
	// fire, at any point, regardless of how long ctx stays cancelled.
	time.Sleep(testGracePeriod * 2)
	if k.wasTerminated() {
		t.Fatal("Terminate was called after commit_active latch — must never happen")
	}
	if k.wasKilled() {
		t.Fatal("Kill was called after commit_active latch — must never happen")
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

	if k.wasTerminated() || k.wasKilled() {
		t.Error("neither Terminate nor Kill must have been called at any point after latch")
	}
	if resultErr != nil {
		t.Fatalf("unexpected error: %v", resultErr)
	}
	if result == nil || result.Type != "result" {
		t.Fatalf("expected result frame, got %+v", result)
	}
}

// Note on the grace-period re-check in invokeIO's watcher goroutine: after
// Terminate() and the grace-period wait, it re-reads stillCancellable
// before ever calling Kill(), specifically so SIGKILL cannot land once the
// transaction may have started, even if some future change altered how
// long the commit_active ack round-trip takes. Under the *current* design
// this second read can never actually observe a different value than the
// first (stillCancellable can only transition to false via the frame
// loop's own ctx.Err()==nil check — and ctx.Err() becomes permanently
// non-nil in the same atomic step that closes ctx.Done(), which is what
// wakes this watcher goroutine up in the first place; so if the first read
// already observed cancellation, the frame loop can never subsequently see
// "not yet cancelled" either). That makes the branch unreachable today —
// intentionally kept anyway as a local, cheap invariant rather than an
// assumption resting on the ack-decision logic elsewhere in this file
// never changing. Not exercised by a dedicated test here since doing so
// would require asserting behavior that cannot currently occur; the four
// tests above cover every state actually reachable through invokeIO's
// public timing.

// TestInvokeIO_NoCancellationNoKill verifies the baseline: without any
// cancellation, neither Terminate nor Kill is invoked regardless of latch
// state.
func TestInvokeIO_NoCancellationNoKill(t *testing.T) {
	pr, pw := io.Pipe()
	ctx := context.Background()

	k := &fakeKiller{}

	var result *Result
	done := make(chan struct{})
	go func() {
		defer close(done)
		result, _ = invokeIO(ctx, discardStdin(), pr, k, testGracePeriod, nil)
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

	if k.wasTerminated() || k.wasKilled() {
		t.Error("neither Terminate nor Kill must be called without cancellation")
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
		invokeIO(ctx, stdinW, pr, &fakeKiller{}, testGracePeriod, nil)
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
// relying solely on a racing Kill().
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

	// A no-op killer — this test only asserts the ack content, not the
	// watcher-goroutine Terminate/Kill path (covered separately above).
	done := make(chan struct{})
	go func() {
		defer close(done)
		invokeIO(ctx, stdinW, pr, &fakeKiller{}, testGracePeriod, nil)
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
		invokeIO(ctx, stdinW, pr, &fakeKiller{}, testGracePeriod, onFrame)
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
