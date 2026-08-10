#include "transaction.h"
#include "tools.h"
#include "../transport.h"
#include "../system.h"

#include <unistd.h>

#include <zypp/Resolver.h>
#include <zypp/ZYppCommitPolicy.h>
#include <zypp/ZYppCommitResult.h>
#include <zypp-core/parser/json/JsonValue.h>

using namespace zypp;

// ─── Schema ──────────────────────────────────────────────────────────────────
// Identical args to plan_install minus testcase — confirm always hits the live system.
static const zypp::json::Object kConfirmInstallSchema = {
    { "type",       "object"                       },
    { "properties", installSchemaProperties()      },
    { "required",   zypp::json::Array{ "package" } }
};

const zypp::json::Object & schema_confirm_install() { return kConfirmInstallSchema; }

// ─── Tool implementation ─────────────────────────────────────────────────────
int tool_confirm_install( const zypp::json::Object & arg, McpTransport & t )
{
    if ( geteuid() != 0 )
    {
        t.writeFrame( jsonError( "PERMISSION_DENIED", "confirm_install requires root privileges." ) );
        return 1;
    }
    // Always live system — no testcase parameter.
    ZYpp::Ptr zypp = loadSystem( std::nullopt );

    if ( !setupInstall( arg, zypp, "confirm_install", t ) )
        return 1;

    if ( !zypp->resolver()->resolvePool() )
    {
        t.writeFrame( solverProblemsToJson( "confirm_install", zypp->resolver() ).asJSON() );
        return 1;
    }

    ZYppCommitPolicy policy;
    policy.syncPoolAfterCommit( true );
    ZYppCommitResult result = zypp->commit( policy );

    zypp::json::Array installed;
    for ( const auto & step : result.transactionStepList() )
    {
        if ( step.stepType() == sat::Transaction::TRANSACTION_INSTALL )
            installed.add( zypp::json::Object{ {
                { "name",    step.ident().asString()               },
                { "edition", step.satSolvable().edition().asString() }
            } } );
    }

    t.writeFrame( zypp::json::Object{ {
        { "type",      "result"          },
        { "tool",      "confirm_install" },
        { "installed", std::move(installed) }
    } }.asJSON() );
    return 0;
}
