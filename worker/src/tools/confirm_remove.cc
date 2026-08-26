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

    // See confirm_install.cc for the full rationale on both branches below
    // (commitFailureToJson, the COMMIT_FAILED/COMMIT_PREPARE_FAILED split,
    // and why the catch branch cannot rely on attemptToModify()).
    ZYppCommitResult result;
    try
    {
        result = zypp->commit( policy );
    }
    catch ( const zypp::Exception & )
    {
        if ( ctx.failures().hasErrors() )
        {
            t.writeFrame( commitFailureToJson(
                "confirm_remove", "COMMIT_FAILED",
                "confirm_remove failed, some packages may have been changed.",
                result, ctx.failures() ).asJSON() );
            return 1;
        }
        throw; // unrelated failure — main()'s handler reports it
    }

    if ( !result.allDone() )
    {
        const bool attempted = result.attemptToModify();
        t.writeFrame( commitFailureToJson(
            "confirm_remove",
            attempted ? "COMMIT_FAILED" : "COMMIT_PREPARE_FAILED",
            attempted ? "confirm_remove failed, some packages may have been changed."
                      : "confirm_remove failed, nothing was removed.",
            result, ctx.failures() ).asJSON() );
        return 1;
    }

    zypp::json::Array removed;
    for ( const auto & step : result.transactionStepList() )
    {
        if ( step.stepType() == sat::Transaction::TRANSACTION_ERASE )
            removed.add( zypp::json::Object{ {
                { "name",    step.ident().asString()   },
                { "edition", step.edition().asString()  }
            } } );
    }

    // See confirm_install.cc for the full rationale (mirrors zypper's
    // ZYPPER_EXIT_INF_RPM_SCRIPT_FAILED).
    zypp::json::Object resultFrame = {
        { "type",    "result"         },
        { "tool",    "confirm_remove" },
        { "removed", std::move(removed) }
    };
    zypp::json::Array warnings = commitIssuesToJson( ctx.failures() );
    if ( warnings.size() > 0 )
    {
        resultFrame.add( "warnings", std::move(warnings) );
        resultFrame.add( "detail", "confirm_remove completed successfully; see "
                                   "\"warnings\" for non-fatal issues encountered "
                                   "during the transaction (e.g. a scriptlet "
                                   "failure that did not prevent removal)." );
    }
    t.writeFrame( resultFrame.asJSON() );
    return 0;
}
