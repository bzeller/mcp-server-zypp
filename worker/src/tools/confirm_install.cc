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

    // writeCommitFailure reports why, not just that, a commit failed — see
    // transaction.h: commitFailureToJson(). COMMIT_FAILED vs
    // COMMIT_PREPARE_FAILED (result.attemptToModify()) distinguishes "some
    // packages may have already been changed" from "nothing was touched".
    auto writeCommitFailure = [&]( const ZYppCommitResult & result )
    {
        const bool attempted = result.attemptToModify();
        t.writeFrame( commitFailureToJson(
            "confirm_install",
            attempted ? "COMMIT_FAILED" : "COMMIT_PREPARE_FAILED",
            attempted ? "confirm_install failed, some packages may have been changed."
                      : "confirm_install failed, nothing was installed.",
            result, ctx.failures() ).asJSON() );
    };

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
        // A download/install/remove failure surfaces here too — e.g. a
        // problem() override returning ABORT raises AbortRequestException,
        // which TargetImpl::commit() converts into a thrown
        // TargetAbortedException. commit() returns ZYppCommitResult purely
        // by value, so when it throws, the assignment to result above never
        // happens — result is still the empty object default-constructed
        // before the try block, and attemptToModify() on that is always
        // false (ZYppCommitResult.cc: FalseBool). Reusing writeCommitFailure
        // here would therefore always claim "nothing was installed", even
        // if the exception fired deep into a real transaction. Report
        // COMMIT_FAILED unconditionally instead — the more cautious of the
        // two messages — since we genuinely cannot tell which is true;
        // ctx.failures() (populated independently of whether commit()
        // returns or throws) is still the reliable part of this frame.
        if ( ctx.failures().hasErrors() )
        {
            t.writeFrame( commitFailureToJson(
                "confirm_install", "COMMIT_FAILED",
                "confirm_install failed, some packages may have been changed.",
                result, ctx.failures() ).asJSON() );
            return 1;
        }
        throw; // unrelated failure — main()'s handler reports it
    }

    if ( ctx.gpgKeys().hasRejections() )
    {
        writeKeyError();
        return 1;
    }

    if ( !result.allDone() )
    {
        writeCommitFailure( result );
        return 1;
    }

    zypp::json::Array installed;
    for ( const auto & step : result.transactionStepList() )
    {
        if ( step.stepType() == sat::Transaction::TRANSACTION_INSTALL )
            installed.add( zypp::json::Object{ {
                { "name",    step.ident().asString()   },
                { "edition", step.edition().asString()  }
            } } );
    }

    // Every step completed, but something may still have gone wrong along
    // the way without stopping it — canonically a %posttrans scriptlet
    // failing non-fatally (mirrors zypper's ZYPPER_EXIT_INF_RPM_SCRIPT_FAILED,
    // exit code 107: still a successful operation, just one worth flagging).
    // See transaction.h: commitIssuesToJson().
    zypp::json::Object resultFrame = {
        { "type",      "result"          },
        { "tool",      "confirm_install" },
        { "installed", std::move(installed) }
    };
    zypp::json::Array warnings = commitIssuesToJson( ctx.failures() );
    if ( warnings.size() > 0 )
    {
        resultFrame.add( "warnings", std::move(warnings) );
        resultFrame.add( "detail", "confirm_install completed successfully; see "
                                   "\"warnings\" for non-fatal issues encountered "
                                   "during the transaction (e.g. a scriptlet "
                                   "failure that did not prevent installation)." );
    }
    t.writeFrame( resultFrame.asJSON() );
    return 0;
}
