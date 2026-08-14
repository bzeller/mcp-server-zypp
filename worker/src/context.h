#ifndef MCP_SERVER_ZYPP_CONTEXT_H
#define MCP_SERVER_ZYPP_CONTEXT_H

#include <optional>

#include <zypp/ZYpp.h>
#include <zypp-core/Pathname.h>
#include <zypp-core/parser/json/JsonValue.h>

#include "transport.h"

/// Per-invocation state, owned by main() for the lifetime of the process.
///
/// The worker runs exactly one tool per process, so everything here has
/// process lifetime and is owned by value — no borrowed references.
///
/// Deliberately does NOT own McpCallbackScope: libzypp registers callback
/// receivers in a process-global dispatcher
/// (callback::DistributeReport<T>::instance(), a function-local static
/// holding one raw Receiver*). That registration is a global side effect,
/// not state this object can own, so its RAII handle stays in main().
class ToolContext
{
public:
    ToolContext() = default;

    ToolContext( const ToolContext & ) = delete;
    ToolContext & operator=( const ToolContext & ) = delete;

    McpTransport & transport() { return _transport; }

    /// Live system at "/". Cannot be redirected to a testcase — confirm_*
    /// depend on that being structurally impossible rather than merely
    /// unused, hence a separate entry point instead of a defaulted argument.
    zypp::ZYpp::Ptr loadLiveSystem();

    /// Live system, or a testcase when the "testcase" argument is present.
    /// Read-only and plan_* tools only.
    zypp::ZYpp::Ptr loadSystemFromArg( const zypp::json::Object & arg );

private:
    zypp::ZYpp::Ptr load( const std::optional<zypp::Pathname> & testcase );

    McpTransport    _transport;
    zypp::ZYpp::Ptr _zypp;   ///< lazily acquired on first load
};

#endif // MCP_SERVER_ZYPP_CONTEXT_H
