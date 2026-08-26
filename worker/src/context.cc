#include "context.h"
#include "system.h"
#include "tools/validate.h"

// _callbacks is constructed last (declaration order, see context.h) after
// every other member already exists — McpCallbackScope's constructor
// connects 13 receivers, each of which stores this ToolContext& without
// dereferencing it, so it's safe for that to happen before the rest of the
// initializer list "runs" from _callbacks's own perspective; by the time
// any receiver's virtual override is actually invoked, construction has
// long since completed.
ToolContext::ToolContext() : _callbacks( *this ) {}

zypp::ZYpp::Ptr ToolContext::load( const std::optional<zypp::Pathname> & testcase )
{
    // Lazy: the ZYpp lock is taken here, not at construction, so tools keep
    // validating arguments before anything acquires the lock.
    if ( !_zypp )
        _zypp = loadSystem( testcase );
    return _zypp;
}

zypp::ZYpp::Ptr ToolContext::loadLiveSystem()
{ return load( std::nullopt ); }

zypp::ZYpp::Ptr ToolContext::loadSystemFromArg( const zypp::json::Object & arg )
{
    std::optional<zypp::Pathname> testcase;
    if ( arg.contains( "testcase" ) )
        testcase = zypp::Pathname( validate::requireNonEmpty( arg, "testcase" ) );
    return load( testcase );
}
