#include "transaction.h"
#include "tools.h"
#include "validate.h"
#include "../transport.h"
#include "../context.h"

#include <zypp/Resolver.h>
#include <zypp-core/parser/json/JsonValue.h>

using namespace zypp;

// ─── Schema ──────────────────────────────────────────────────────────────────
// testcase is plan-only — confirm always runs against the live system.
static const zypp::json::Object kPlanRemoveSchema = []{
    auto props = removeSchemaProperties();
    props.add( "testcase", zypp::json::Object{ {
        { "type",        "string" },
        { "description", "Path to a solver testcase directory. Omit to use the live system." }
    } } );
    return zypp::json::Object{ {
        { "type",       "object" },
        { "properties", std::move(props) },
        { "required",   zypp::json::Array{ "package" } }
    } };
}();

const zypp::json::Object & schema_plan_remove() { return kPlanRemoveSchema; }

// ─── Tool implementation ─────────────────────────────────────────────────────
int tool_plan_remove( const zypp::json::Object & arg, ToolContext & ctx )
{
    McpTransport & t = ctx.transport();
    ZYpp::Ptr zypp = ctx.loadSystemFromArg( arg );

    const auto res = setupRemove( arg, zypp, "plan_remove", t );
    if ( res == SetupRemoveResult::NotInstalled ) return 0;
    if ( res == SetupRemoveResult::Error        ) return 1;

    if ( !zypp->resolver()->resolvePool() )
    {
        t.writeFrame( solverProblemsToJson( "plan_remove", zypp->resolver() ).asJSON() );
        return 1;
    }

    t.writeFrame( planRemoveToJson( "plan_remove", zypp->pool() ).asJSON() );
    return 0;
}
