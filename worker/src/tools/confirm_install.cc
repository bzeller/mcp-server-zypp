#include "transaction.h"
#include "tools.h"
#include "validate.h"
#include "../transport.h"
#include "../system.h"

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

    // Must run after resolvePool() (only then is isToBeInstalled() accurate)
    // and before commit() (licenses must be confirmed before anything is
    // touched). See transaction.h: checkLicensesAccepted.
    const auto acceptedLicenses = validate::optionalStringSet( arg, "accepted_licenses" );
    if ( !checkLicensesAccepted( zypp->pool(), acceptedLicenses, "confirm_install", t ) )
        return 1;

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
