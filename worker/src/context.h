#ifndef MCP_SERVER_ZYPP_CONTEXT_H
#define MCP_SERVER_ZYPP_CONTEXT_H

#include <optional>

#include <zypp/ZYpp.h>
#include <zypp-core/Pathname.h>
#include <zypp-core/parser/json/JsonValue.h>

#include "transport.h"
#include "gpgkeygate.h"
#include "commitfailurelog.h"
#include "callbacks.h"

/// Per-invocation state, owned by main() for the lifetime of the process.
///
/// The worker runs exactly one tool per process, so everything here has
/// process lifetime and is owned by value — no borrowed references.
///
/// Owns McpCallbackScope as its final member (see the member list below for
/// why final matters). The only real constraint on that ownership is
/// uniqueness, not some inherent unsuitability of RAII-wrapping a global
/// side effect: libzypp registers callback receivers in a process-global
/// dispatcher (callback::DistributeReport<T>::instance(), a function-local
/// static holding one raw Receiver*), so at most one McpCallbackScope — and
/// therefore at most one ToolContext — may exist at a time. ToolContext is
/// already non-copyable, and the worker constructs exactly one, in main().
class ToolContext
{
public:
    ToolContext();

    ToolContext( const ToolContext & ) = delete;
    ToolContext & operator=( const ToolContext & ) = delete;

    McpTransport      & transport() { return _transport; }
    GpgKeyGate        & gpgKeys()   { return _gpgKeys; }
    CommitFailureLog  & failures()  { return _failures; }

    /// Live system at "/". Cannot be redirected to a testcase — confirm_*
    /// depend on that being structurally impossible rather than merely
    /// unused, hence a separate entry point instead of a defaulted argument.
    zypp::ZYpp::Ptr loadLiveSystem();

    /// Live system, or a testcase when the "testcase" argument is present.
    /// Read-only and plan_* tools only.
    zypp::ZYpp::Ptr loadSystemFromArg( const zypp::json::Object & arg );

private:
    zypp::ZYpp::Ptr load( const std::optional<zypp::Pathname> & testcase );

    McpTransport     _transport;
    GpgKeyGate       _gpgKeys;
    CommitFailureLog _failures;
    zypp::ZYpp::Ptr  _zypp;   ///< lazily acquired on first load

    // Declared LAST deliberately: members destruct in reverse declaration
    // order, so this must be the first thing torn down — disconnecting
    // every receiver *before* _transport/_gpgKeys/_failures/_zypp die.
    // Reversed, a report firing during ~ZYpp() would write through
    // dangling references. Constructed last too (see the .cc file): by
    // then every other member is fully initialized, which is all
    // McpCallbackScope's constructor requires — receivers only store the
    // ToolContext& they're given, they never dereference it during
    // construction.
    McpCallbackScope _callbacks;
};

#endif // MCP_SERVER_ZYPP_CONTEXT_H
