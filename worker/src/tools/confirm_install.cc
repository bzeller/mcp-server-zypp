#include "transaction.h"
#include "tools.h"
#include "validate.h"
#include "../transport.h"
#include "../context.h"

#include <unistd.h>

#include <zypp/Resolver.h>
#include <zypp/ZYppCommitPolicy.h>
#include <zypp/ZYppCommitResult.h>
#include <zypp-core/parser/json/JsonValue.h>

using namespace zypp;

// ─── Schema ──────────────────────────────────────────────────────────────────
// Identical args to plan_install minus testcase — confirm always hits the live
// system — plus accepted_licenses, needed only at confirm time. See
// transaction.h: checkLicensesAccepted for the license_id values expected
// here, which plan_install's "licenses" array already surfaces.
static const zypp::json::Object kConfirmInstallSchema = []{
    auto props = installSchemaProperties();
    props.add( "accepted_licenses", zypp::json::Object{ {
        { "type",        "array" },
        { "items",       zypp::json::Object{ { { "type", "string" } } } },
        { "description", "license_id values (from a prior plan_install call's "
                          "\"licenses\" array) that have been reviewed and "
                          "accepted. Any license still requiring confirmation "
                          "that is not listed here aborts with "
                          "LICENSE_CONFIRMATION_REQUIRED." },
        { "default",     zypp::json::Array{} }
    } } );
    return zypp::json::Object{ {
        { "type",       "object" },
        { "properties", std::move(props) },
        { "required",   zypp::json::Array{ "package" } }
    } };
}();

const zypp::json::Object & schema_confirm_install() { return kConfirmInstallSchema; }

// ─── Tool implementation ─────────────────────────────────────────────────────
int tool_confirm_install( const zypp::json::Object & arg, ToolContext & ctx )
{
    McpTransport & t = ctx.transport();

    if ( geteuid() != 0 )
    {
        t.writeFrame( jsonError( "PERMISSION_DENIED", "confirm_install requires root privileges." ) );
        return 1;
    }
    // Always live system — no testcase parameter.
    ZYpp::Ptr zypp = ctx.loadLiveSystem();

    if ( !setupInstall( arg, zypp, "confirm_install", t ) )
        return 1;

    if ( !zypp->resolver()->resolvePool() )
    {
        t.writeFrame( solverProblemsToJson( "confirm_install", zypp->resolver() ).asJSON() );
        return 1;
    }

    // Must run after resolvePool() (only then is isToBeInstalled() accurate)
    // and before commit() (licenses must be confirmed before anything is
    // touched). See transaction.h: checkLicensesAccepted.
    const auto acceptedLicenses = validate::optionalStringSet( arg, "accepted_licenses" );
    if ( !checkLicensesAccepted( zypp->pool(), acceptedLicenses, "confirm_install", t ) )
        return 1;

    auto writeKeyError = [&]{
        zypp::json::Array keys;
        for ( const auto & k : ctx.gpgKeys().rejected() )
            keys.add( zypp::json::Object{ {
                { "fingerprint", k.fingerprint },
                { "name",        k.name        },
                { "repo",        k.repo        }
            } } );
        t.writeFrame( zypp::json::Object{ {
            { "type",   "error"            },
            { "code",   "KEY_NOT_TRUSTED"  },
            { "tool",   "confirm_install"  },
            { "detail", "One or more packages are signed by a key that was "
                        "not trusted, so the transaction was aborted. The "
                        "key(s) below must be reviewed and trusted on the "
                        "system before this install can proceed; this cannot "
                        "be overridden by a tool argument." },
            { "keys",   std::move(keys) }
        } }.asJSON() );
    };

    ZYppCommitPolicy policy;
    policy.syncPoolAfterCommit( true );

    ZYppCommitResult result;
    try
    {
        result = zypp->commit( policy );
    }
    catch ( const zypp::Exception & )
    {
        // A rejected key is the more specific, actionable diagnosis when
        // both are true — report it instead of the raw commit exception.
        if ( ctx.gpgKeys().hasRejections() )
        {
            writeKeyError();
            return 1;
        }
        throw; // unrelated failure — main()'s handler reports it
    }

    if ( ctx.gpgKeys().hasRejections() )
    {
        writeKeyError();
        return 1;
    }

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
