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
// system — plus accepted_licenses/accepted_keys, needed only at confirm time.
// See transaction.h: checkLicensesAccepted for the license_id values expected
// here, which plan_install's "licenses" array already surfaces. There is no
// equivalent pre-listing for accepted_keys: a package's signing key is only
// known once its file is downloaded during commit (see gpgkeygate.h), so
// the first attempt may need a retry with the fingerprint(s) named in a
// KEY_CONFIRMATION_REQUIRED error.
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
    props.add( "accepted_keys", zypp::json::Object{ {
        { "type",        "array" },
        { "items",       zypp::json::Object{ { { "type", "string" } } } },
        { "description", "GPG key fingerprints that have been reviewed and "
                          "accepted as trusted for signing packages in this "
                          "transaction. A package signed by a key not listed "
                          "here aborts with KEY_CONFIRMATION_REQUIRED, which "
                          "reports the fingerprint to confirm." },
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

    // Unlike licenses, a package's signing key is only known once its file
    // is downloaded during commit() — there is nothing to check upfront.
    // See gpgkeygate.h.
    ctx.gpgKeys().accept( validate::optionalStringSet( arg, "accepted_keys" ) );

    auto writeKeyError = [&]{
        zypp::json::Array keys;
        for ( const auto & k : ctx.gpgKeys().rejected() )
            keys.add( zypp::json::Object{ {
                { "fingerprint", k.fingerprint },
                { "name",        k.name        },
                { "repo",        k.repo        }
            } } );
        t.writeFrame( zypp::json::Object{ {
            { "type",   "error"                        },
            { "code",   "KEY_CONFIRMATION_REQUIRED"     },
            { "tool",   "confirm_install"               },
            { "detail", "One or more packages are signed by a key that has "
                        "not been accepted. Review the key(s) below and "
                        "retry with accepted_keys containing the listed "
                        "fingerprint(s)." },
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
