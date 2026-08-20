#include "cancellation.h"

#include <csignal>

namespace
{
    // volatile std::sig_atomic_t is the type the C/C++ standard defines
    // specifically for "safely written by a signal handler, safely read by
    // the interrupted code" — no lock-free assumption needed (unlike
    // std::atomic<bool>, this guarantee is unconditional by definition).
    // Its single-thread guarantee is sufficient here: zypp-mcp-tool's
    // classic (non-zyppng) commit() runs synchronously on one thread, so
    // the signal handler and the progress() polling it interrupts are
    // always the same thread — no cross-thread visibility is required.
    volatile std::sig_atomic_t g_cancelled = 0;

    // extern "C" linkage: sigaction() calls this with the C calling
    // convention.
    extern "C" void handleSigterm( int /*signum*/ )
    {
        g_cancelled = 1;
    }
}

namespace mcp
{
    bool cancellationRequested()
    {
        return g_cancelled != 0;
    }

    void installCancellationHandler()
    {
        struct sigaction action{};
        action.sa_handler = handleSigterm;
        sigemptyset( &action.sa_mask );
        action.sa_flags = SA_RESTART;
        sigaction( SIGTERM, &action, nullptr );
    }
}
