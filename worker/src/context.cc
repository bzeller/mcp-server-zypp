#include "context.h"
#include "system.h"
#include "tools/validate.h"

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
