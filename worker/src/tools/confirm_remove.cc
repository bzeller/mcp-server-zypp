#include "transaction.h"
#include "tools.h"
#include "../transport.h"
#include "../context.h"

#include <unistd.h>

#include <zypp/Resolver.h>
#include <zypp/ZYppCommitPolicy.h>
#include <zypp/ZYppCommitResult.h>
#include <zypp-core/parser/json/JsonValue.h>

using namespace zypp;

// ─── Schema ──────────────────────────────────────────────────────────────────
// Identical args to plan_remove minus testcase — confirm always hits the live system.
static const zypp::json::Object kConfirmRemoveSchema = {
    { "type",       "object"                       },
    { "properties", removeSchemaProperties()       },
    { "required",   zypp::json::Array{ "package" } }
};

const zypp::json::Object & schema_confirm_remove() { return kConfirmRemoveSchema; }

// ─── Tool implementation ─────────────────────────────────────────────────────
int tool_confirm_remove( const zypp::json::Object & arg, ToolContext & ctx )
{
    McpTransport & t = ctx.transport();

    if ( geteuid() != 0 )
    {
        t.writeFrame( jsonError( "PERMISSION_DENIED", "confirm_remove requires root privileges." ) );
        return 1;
    }
    // Always live system — no testcase parameter.
    ZYpp::Ptr zypp = ctx.loadLiveSystem();

    const auto res = setupRemove( arg, zypp, "confirm_remove", t );
    if ( res == SetupRemoveResult::NotInstalled ) return 0;
    if ( res == SetupRemoveResult::Error        ) return 1;

    if ( !zypp->resolver()->resolvePool() )
    {
        t.writeFrame( solverProblemsToJson( "confirm_remove", zypp->resolver() ).asJSON() );
        return 1;
    }

    ZYppCommitPolicy policy;
    policy.syncPoolAfterCommit( true );
    ZYppCommitResult result = zypp->commit( policy );

    zypp::json::Array removed;
    for ( const auto & step : result.transactionStepList() )
    {
        if ( step.stepType() == sat::Transaction::TRANSACTION_ERASE )
            removed.add( zypp::json::Object{ {
                { "name",    step.ident().asString()               },
                { "edition", step.satSolvable().edition().asString() }
            } } );
    }

    t.writeFrame( zypp::json::Object{ {
        { "type",    "result"         },
        { "tool",    "confirm_remove" },
        { "removed", std::move(removed) }
    } }.asJSON() );
    return 0;
}
