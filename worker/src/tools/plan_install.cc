#include "transaction.h"
#include "tools.h"
#include "validate.h"
#include "../transport.h"
#include "../system.h"

#include <zypp/Resolver.h>
#include <zypp-core/parser/json/JsonValue.h>

using namespace zypp;

// ─── Schema ──────────────────────────────────────────────────────────────────
// testcase is plan-only — confirm always runs against the live system.
static const zypp::json::Object kPlanInstallSchema = []{
    auto props = installSchemaProperties();
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

const zypp::json::Object & schema_plan_install() { return kPlanInstallSchema; }

// ─── Tool implementation ─────────────────────────────────────────────────────
int tool_plan_install( const zypp::json::Object & arg, McpTransport & t )
{
    const std::optional<zypp::Pathname> testcase =
        arg.contains("testcase")
            ? std::optional<zypp::Pathname>( validate::requireNonEmpty( arg, "testcase" ) )
            : std::nullopt;

    ZYpp::Ptr zypp = loadSystem( testcase );

    if ( !setupInstall( arg, zypp, "plan_install", t ) )
        return 1;

    if ( !zypp->resolver()->resolvePool() )
    {
        t.writeFrame( solverProblemsToJson( "plan_install", zypp->resolver() ).asJSON() );
        return 1;
    }

    t.writeFrame( planInstallToJson( "plan_install", zypp->pool() ).asJSON() );
    return 0;
}
