#ifndef MCP_SERVER_ZYPP_CANCELLATION_H
#define MCP_SERVER_ZYPP_CANCELLATION_H

// Cooperative cancellation for the download/preload phase of a commit — the
// only phase that can still be safely unwound before the "zypp_control"/
// "commit_active" point of no return (see McpCommitActiveReceive in
// callbacks.h). The proxy requests cancellation by sending SIGTERM to this
// process (see proxy/internal/worker/worker.go: invokeIO's watcher
// goroutine) and escalates to SIGKILL if it has not exited within a grace
// period — never once the point of no return has been reached.
//
// Scope: this only helps during commit()'s download/preload phase, where
// McpDownloadReceive::progress()/McpCommitPreloadReceive::progress() poll
// cancellationRequested() on every tick. A SIGTERM received during, say,
// resolvePool() or while blocked on an elicitation answer sets the flag but
// nothing acts on it until the next poll point (or, failing that, the
// proxy's SIGKILL escalation after the grace period — the same outcome as
// before this existed, just delayed by the grace period rather than
// immediate).
namespace mcp
{
    // true once SIGTERM has been received by this process.
    bool cancellationRequested();

    // Installs a SIGTERM handler that latches cancellationRequested() to
    // true (backed by a volatile std::sig_atomic_t — the standard type for
    // safe signal-handler/interrupted-code communication, see
    // cancellation.cc). Call once, early in main(), before any
    // download/preload activity can begin. Uses SA_RESTART deliberately:
    // cancellation here is purely cooperative polling, not syscall
    // interruption — an EINTR leaking into a libzypp/libcurl-internal
    // blocking call this process does not control would be a far riskier
    // change than the one grace-period delay it might save.
    void installCancellationHandler();
}

#endif // MCP_SERVER_ZYPP_CANCELLATION_H
