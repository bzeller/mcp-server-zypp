#include "transaction.h"
#include "tools.h"
#include "validate.h"
#include "../transport.h"

#include <zypp-core/base/LogTools.h>

#undef  ZYPP_BASE_LOGGER_LOGGROUP
#define ZYPP_BASE_LOGGER_LOGGROUP "zypp-mcp-tool"

#include <zypp/Resolver.h>
#include <zypp/PoolItem.h>
#include <zypp/Capability.h>
#include <zypp/ResStatus.h>
#include <zypp/ResPoolProxy.h>
#include <zypp/ui/Selectable.h>

#include "../solver/requester.h"
#include "../solver/apply.h"

#include <cstdint>
#include <iomanip>
#include <sstream>

using namespace zypp;

// ─── Shared schema property objects ──────────────────────────────────────────

const zypp::json::Object & installSchemaProperties()
{
    static const zypp::json::Object props = {
        { "package", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Package name or capability to install." }
        } } },
        { "type", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Type of resolvable to install." },
            { "enum",        zypp::json::Array{ "package", "patch", "pattern", "product" } },
            { "default",     "package" }
        } } },
        { "repo", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Install from this specific repository alias only." }
        } } },
        { "capability", zypp::json::Object{ {
            { "type",        "boolean" },
            { "description", "Match by capability rather than exact name (default: true)." },
            { "default",     true }
        } } },
        { "no_recommends", zypp::json::Object{ {
            { "type",        "boolean" },
            { "description", "Do not install recommended packages, only required ones (default: false)." },
            { "default",     false }
        } } },
        { "solver_options", zypp::json::Object{ {
            { "type",        "object" },
            { "description", "Fine-grained solver permissions." },
            { "properties",  zypp::json::Object{
                { "allow_downgrade",     zypp::json::Object{ { { "type", "boolean" } } } },
                { "allow_name_change",   zypp::json::Object{ { { "type", "boolean" } } } },
                { "allow_arch_change",   zypp::json::Object{ { { "type", "boolean" } } } },
                { "allow_vendor_change", zypp::json::Object{ { { "type", "boolean" } } } }
            } }
        } } }
    };
    return props;
}

const zypp::json::Object & removeSchemaProperties()
{
    static const zypp::json::Object props = {
        { "package", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Package name or capability to remove." }
        } } },
        { "type", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Type of resolvable to remove." },
            { "enum",        zypp::json::Array{ "package", "patch", "pattern", "product" } },
            { "default",     "package" }
        } } },
        { "repo", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Restrict removal to packages from this repository alias." }
        } } },
        { "capability", zypp::json::Object{ {
            { "type",        "boolean" },
            { "description", "Match by capability rather than exact name (default: false)." },
            { "default",     false }
        } } },
        { "clean_deps", zypp::json::Object{ {
            { "type",        "boolean" },
            { "description", "Automatically remove unneeded dependencies after removal (default: false)." },
            { "default",     false }
        } } }
    };
    return props;
}

// ─── Solver option accessors ──────────────────────────────────────────────────

bool requestsAllowDowngrade( const zypp::json::Object & arg )
{
    if ( !arg.contains("solver_options") )
        return false;
    return validate::optionalBool( arg.value("solver_options").asObject(), "allow_downgrade" );
}

// ─── setupInstall ─────────────────────────────────────────────────────────────

bool setupInstall( const zypp::json::Object & arg,
                   ZYpp::Ptr                  zypp,
                   const std::string &        toolName,
                   McpTransport &             t )
{
    const std::string pkg  = validate::requireNonEmpty( arg, "package" );
    const std::string type = validate::optionalEnum( arg, "type",
        { "package", "patch", "pattern", "product" } );
    const std::string repo = validate::optionalNonEmpty( arg, "repo" );

    // capability defaults true; forced false when repo is given (name-based lookup)
    const bool byCapability =
        repo.empty() && validate::optionalBool( arg, "capability", true );
    const bool noRecommends = validate::optionalBool( arg, "no_recommends" );

    Resolver_Ptr resolver = zypp->resolver();

    if ( noRecommends )
        resolver->setOnlyRequires( true );

    // Mirrored onto solverequest::Options below wherever the selection
    // algorithm itself (as opposed to the resolver) reads the same
    // permission — downgrade in particular has both a resolver-level and
    // a selection-level switch that need to agree.
    solverequest::Options solverOpts;

    if ( requestsAllowDowngrade( arg ) )
    {
        resolver->setAllowDowngrade( true );
        solverOpts.oldpackage = true;
    }

    if ( arg.contains("solver_options") )
    {
        const auto & opts = arg.value("solver_options").asObject();
        if ( validate::optionalBool( opts, "allow_name_change"  ) ) resolver->setAllowNameChange( true );
        if ( validate::optionalBool( opts, "allow_arch_change"  ) ) resolver->setAllowArchChange( true );
        if ( validate::optionalBool( opts, "allow_vendor_change") )
        {
            resolver->setAllowVendorChange( true );
            solverOpts.allowVendorChange = true;
        }
    }

    // byCapability maps directly onto the selection algorithm's own
    // mutually-exclusive forceByCap/forceByName switch — there is no
    // separate "raw resolver job" path here anymore. Letting
    // solverequest::Requester own the capability case too (rather than a
    // bare resolver->addRequire() special-cased in this file) gives it
    // the same already-installed/not-found handling as the by-name path,
    // instead of silently deferring an unresolvable capability all the
    // way to a generic solver error at resolvePool() time.
    if ( byCapability )
        solverOpts.setForceByCap( true );
    else
        solverOpts.setForceByName( true );

    const ResKind kind = kindFromString( type );

    solverequest::PackageSpec spec;
    spec.orig_str   = pkg;
    spec.parsed_cap = Capability( pkg, kind );
    spec.repo_alias = repo;

    solverequest::Requester requester( solverOpts );
    requester.submit( zypp->pool(), { solverequest::Operation::Install, { spec }, {} } );

    if ( requester.hasFeedback( solverequest::Feedback::NOT_FOUND_NAME )
      || requester.hasFeedback( solverequest::Feedback::NOT_FOUND_CAP ) )
    {
        const std::string detail = repo.empty()
            ? "Package '" + pkg + "' not found in any repository."
            : "Package '" + pkg + "' not found in repository '" + repo + "'.";

        MIL << detail << std::endl;

        t.writeFrame( zypp::json::Object{ {
            { "type",   "error"    },
            { "code",   "NOT_FOUND"},
            { "tool",   toolName   },
            { "detail", detail     }
        } }.asJSON() );
        return false;
    }

    // Applies exactly what was selected — a direct status mutation for an
    // unlocked item, or a solver requirement/conflict job (never a direct
    // mutation) for a locked one, so a lock surfaces as a solver conflict
    // instead of being silently cleared. Candidate selection itself is
    // vendor/priority/version/lock-aware rather than "whatever the pool
    // happens to iterate first".
    solverequest::applySelections( resolver, requester.selections() );
    return true;
}

// ─── setupRemove ─────────────────────────────────────────────────────────────

SetupRemoveResult setupRemove( const zypp::json::Object & arg,
                               ZYpp::Ptr                  zypp,
                               const std::string &        toolName,
                               McpTransport &             t )
{
    const std::string pkg  = validate::requireNonEmpty( arg, "package" );
    const std::string type = validate::optionalEnum( arg, "type",
        { "package", "patch", "pattern", "product" } );
    const std::string repo = validate::optionalNonEmpty( arg, "repo" );

    const bool byCapability = validate::optionalBool( arg, "capability", false );
    const bool cleanDeps    = validate::optionalBool( arg, "clean_deps" );

    Resolver_Ptr resolver = zypp->resolver();

    if ( cleanDeps )
        resolver->setCleandepsOnRemove( true );

    // Unconditional, and deliberately not a caller-settable option: a
    // removal must never pull in newly recommended packages as a solver
    // side effect. Mirrors zypper, which overrides both zypper.conf and
    // any --recommends flag for the remove command specifically
    // ("never install recommends when removing packages").
    resolver->setOnlyRequires( true );

    const ResKind kind = kindFromString( type );

    // byCapability maps onto the selection algorithm's own mutually
    // exclusive forceByCap/forceByName switch — see setupInstall() above
    // for why both branches go through solverequest::Requester rather
    // than one of them bypassing it with a bare resolver job.
    solverequest::Options solverOpts;
    if ( byCapability )
        solverOpts.setForceByCap( true );
    else
        solverOpts.setForceByName( true );

    solverequest::PackageSpec spec;
    spec.orig_str   = pkg;
    spec.parsed_cap = Capability( pkg, kind );
    spec.repo_alias = repo;
    // Note: repo restriction has no effect on removal regardless — an
    // installed item's own repository is always "@System" by the time it
    // is in the pool, so there is no origin repo left to filter on. Set
    // here purely for forward-compatibility should that ever change;
    // currently every removal ignores it exactly as it does above.

    solverequest::Requester requester( solverOpts );
    requester.submit( zypp->pool(), { solverequest::Operation::Remove, {}, { spec } } );

    // Every "nothing to remove" outcome (name/capability not found at
    // all, or found but nothing currently installed satisfies it) is
    // treated identically: an empty plan, not an error — matching this
    // tool's existing "removing something not installed is a no-op"
    // contract, which never distinguished "doesn't exist" from "not
    // installed" either.
    if ( requester.hasFeedback( solverequest::Feedback::NOT_FOUND_NAME )
      || requester.hasFeedback( solverequest::Feedback::NOT_FOUND_CAP )
      || requester.hasFeedback( solverequest::Feedback::NOT_INSTALLED )
      || requester.hasFeedback( solverequest::Feedback::NO_INSTALLED_PROVIDER ) )
    {
        t.writeFrame( zypp::json::Object{ {
            { "type", "result"  },
            { "tool", toolName  },
            { "plan", zypp::json::Object{ {
                { "to_remove", zypp::json::Array{} }
            } } }
        } }.asJSON() );
        return SetupRemoveResult::NotInstalled;
    }

    solverequest::applySelections( resolver, requester.selections() );
    return SetupRemoveResult::Ok;
}

// ─── planInstallToJson ────────────────────────────────────────────────────────

zypp::json::Object planInstallToJson( const std::string &   toolName,
                                      const ResPool &        pool )
{
    zypp::json::Array toInstall, toRemove;
    for ( const auto & pi : pool )
    {
        if ( pi.status().isToBeInstalled() )
        {
            toInstall.add( zypp::json::Object{ {
                { "name",    pi.name()               },
                { "edition", pi.edition().asString() },
                { "arch",    pi.arch().asString()    }
            } } );
        }
        else if ( pi.status().isToBeUninstalled() )
        {
            toRemove.add( zypp::json::Object{ {
                { "name",    pi.name()               },
                { "edition", pi.edition().asString() },
                { "arch",    pi.arch().asString()    }
            } } );
        }
    }

    // Same grouping/ids checkLicensesAccepted will later verify against —
    // the caller can copy license_id values straight from here into
    // confirm_install's accepted_licenses.
    zypp::json::Array licenses;
    for ( const auto & [id, group] : collectLicensesToConfirm( pool ) )
    {
        zypp::json::Array pkgs;
        for ( const auto & name : group.packages )
            pkgs.add( name );
        licenses.add( zypp::json::Object{ {
            { "license_id", id              },
            { "text",       group.text      },
            { "packages",   std::move(pkgs) }
        } } );
    }

    return zypp::json::Object{ {
        { "type", "result" },
        { "tool", toolName },
        { "plan", zypp::json::Object{ {
            { "to_install", std::move(toInstall) },
            { "to_remove",  std::move(toRemove)  },
            { "licenses",   std::move(licenses)  }
        } } }
    } };
}

// ─── planRemoveToJson ─────────────────────────────────────────────────────────

zypp::json::Object planRemoveToJson( const std::string &   toolName,
                                     const ResPool &        pool )
{
    zypp::json::Array toRemove;
    for ( const auto & pi : pool )
    {
        if ( !pi.status().isToBeUninstalled() ) continue;
        toRemove.add( zypp::json::Object{ {
            { "name",    pi.name()               },
            { "edition", pi.edition().asString() },
            { "arch",    pi.arch().asString()    }
        } } );
    }
    return zypp::json::Object{ {
        { "type", "result"  },
        { "tool", toolName  },
        { "plan", zypp::json::Object{ {
            { "to_remove", std::move(toRemove) }
        } } }
    } };
}

// ─── License confirmation ─────────────────────────────────────────────────────

namespace
{
    /// A stable, deterministic key for the license text, used purely to
    /// dedupe identical licenses across packages and to round-trip between
    /// plan_install's output (in one worker process) and a later
    /// confirm_install's accepted_licenses input (in a separate worker
    /// process) — NOT a cryptographic digest, no security property is
    /// required here, just this one round-trip guarantee.
    ///
    /// Deliberately NOT std::hash<std::string>: the standard only requires
    /// std::hash to be consistent within a single program execution
    /// ([unord.hash]) — nothing guarantees the same text hashes identically
    /// across the two separate zypp-mcp-tool invocations plan_install and
    /// confirm_install actually are. FNV-1a is fully specified here instead,
    /// so the result is guaranteed stable across processes, builds, and
    /// architectures — not merely "currently happens to be stable" the way
    /// libstdc++'s fixed-seed std::hash implementation currently is.
    std::uint64_t fnv1a64( const std::string & text )
    {
        // Standard FNV-1a 64-bit constants — see
        // http://www.isthe.com/chongo/tech/comp/fnv/.
        std::uint64_t hash = 0xcbf29ce484222325ULL; // offset basis
        for ( unsigned char c : text )
        {
            hash ^= c;
            hash *= 0x100000001b3ULL; // FNV prime
        }
        return hash;
    }

    std::string licenseId( const std::string & text )
    {
        std::ostringstream os;
        os << std::hex << std::setfill('0') << std::setw(16) << fnv1a64( text );
        return os.str();
    }
} // namespace

std::map<std::string, LicenseGroup> collectLicensesToConfirm( const ResPool & pool )
{
    std::map<std::string, LicenseGroup> groups; // keyed by license_id
    for ( const auto & pi : pool )
    {
        if ( !pi.status().isToBeInstalled() )          continue;
        if ( !pi.satSolvable().needToAcceptLicense() )  continue;
        const std::string text = pi.satSolvable().licenseToConfirm();
        if ( text.empty() )                             continue;

        // Mirrors zypper's misc.cc::confirm_licenses (bnc#394396), fixed: a
        // truly-installed (RPMDB-derived) solvable never actually carries a
        // SOLVABLE_EULA attribute — rpmdb2solv doesn't extract one from the
        // RPM header, for any resolvable kind — so upstream's direct
        // inst->licenseToConfirm() comparison against the installed item is
        // unconditionally "" and can never detect "unchanged", making the
        // whole suppression dead code in practice (see bug report). To make
        // it actually work, look for an exact-NEVRA "available" twin of the
        // installed item — the same technique zypper's own report_licenses()
        // uses to solve the identical problem — and prefer its license text,
        // the best obtainable proxy for "what the user was shown when this
        // exact build was originally installed". Falls back to the
        // installed item's own (normally empty) text if no such twin is
        // currently published in any configured repo, which safely
        // degrades to "always confirm" rather than risking a false skip.
        ui::Selectable::Ptr selectable = pool.proxy().lookup( pi.kind(), pi.name() );
        if ( selectable && selectable->hasInstalledObj() )
        {
            bool differs = false;
            for ( auto inst = selectable->installedBegin(); inst != selectable->installedEnd(); ++inst )
            {
                std::string instLicense = inst->satSolvable().licenseToConfirm();
                for ( auto avail = selectable->availableBegin(); avail != selectable->availableEnd(); ++avail )
                {
                    if ( avail->satSolvable().sameNVRA( inst->satSolvable() ) )
                    {
                        instLicense = avail->satSolvable().licenseToConfirm();
                        break;
                    }
                }
                if ( instLicense != text )
                {
                    differs = true;
                    break;
                }
            }
            if ( !differs )
                continue;
        }

        const std::string id = licenseId( text );
        LicenseGroup & group = groups[id];
        group.text = text;
        group.packages.push_back( pi.name() );
    }
    return groups;
}

bool checkLicensesAccepted( const ResPool &                pool,
                           const std::set<std::string> & acceptedLicenseIds,
                           const std::string &            toolName,
                           McpTransport &                  t )
{
    zypp::json::Array pending;
    for ( const auto & [id, group] : collectLicensesToConfirm( pool ) )
    {
        if ( acceptedLicenseIds.count( id ) )
            continue; // already accepted by the caller

        zypp::json::Array pkgs;
        for ( const auto & name : group.packages )
            pkgs.add( name );

        pending.add( zypp::json::Object{ {
            { "license_id", id              },
            { "text",       group.text      },
            { "packages",   std::move(pkgs) }
        } } );
    }

    if ( pending.size() == 0 )
        return true;

    t.writeFrame( zypp::json::Object{ {
        { "type",     "error"                         },
        { "code",     "LICENSE_CONFIRMATION_REQUIRED"  },
        { "tool",     toolName                         },
        { "detail",   "One or more packages require license confirmation. "
                      "Review the license text(s) below and retry with "
                      "accepted_licenses containing the listed license_id(s)." },
        { "licenses", std::move(pending)               }
    } }.asJSON() );
    return false;
}

// ─── Commit failure reporting ───────────────────────────────────────────────
namespace
{
    /// Append one {name, edition, arch} entry built from a Step's own
    /// accessors — never satSolvable(), see transaction.h's doc comment on
    /// commitFailureToJson() for why that matters here specifically.
    void addStepEntry( zypp::json::Array & array, const sat::Transaction::Step & step )
    {
        array.add( zypp::json::Object{ {
            { "name",    step.ident().asString()   },
            { "edition", step.edition().asString()  },
            { "arch",    step.arch().asString()     }
        } } );
    }

    /// {package?, phase, severity, text} for one CommitFailureDetail —
    /// shared between commitFailureToJson()'s "details" array and
    /// commitIssuesToJson(), so both serialize entries identically.
    zypp::json::Object detailToJson( const CommitFailureDetail & entry )
    {
        zypp::json::Object d = {
            { "phase",    std::string( phaseName( entry.phase ) )       },
            { "severity", std::string( severityName( entry.severity ) ) },
            { "text",     entry.text }
        };
        if ( !entry.package.empty() )
            d.add( "package", entry.package );
        return d;
    }
}

zypp::json::Object commitFailureToJson( const std::string &            toolName,
                                        const std::string &            code,
                                        const std::string &            detail,
                                        const zypp::ZYppCommitResult & result,
                                        const CommitFailureLog &       failures )
{
    zypp::json::Array failedInstalls, failedRemovals, skippedInstalls, skippedRemovals;

    // Bucketed exactly as zypper's CommitSummary::collectData() does
    // (src/CommitSummary.cc) — TRANSACTION_IGNORE steps carry no actionable
    // information (obsoletes, non-package actions) and are always omitted,
    // matching zypper.
    for ( const auto & step : result.transactionStepList() )
    {
        const bool failed = step.stepStage() == sat::Transaction::STEP_ERROR;
        switch ( step.stepType() )
        {
            case sat::Transaction::TRANSACTION_ERASE:
                addStepEntry( failed ? failedRemovals : skippedRemovals, step );
                break;
            case sat::Transaction::TRANSACTION_INSTALL:
            case sat::Transaction::TRANSACTION_MULTIINSTALL:
                addStepEntry( failed ? failedInstalls : skippedInstalls, step );
                break;
            case sat::Transaction::TRANSACTION_IGNORE:
            default:
                break;
        }
    }

    zypp::json::Array details;
    for ( const auto & entry : failures.entries() )
        details.add( detailToJson( entry ) );

    zypp::json::Object frame = {
        { "type",             "error"                       },
        { "code",             code                          },
        { "tool",             toolName                      },
        { "detail",           detail                        },
        { "failed_installs",  std::move(failedInstalls)     },
        { "failed_removals",  std::move(failedRemovals)     },
        { "skipped_installs", std::move(skippedInstalls)    },
        { "skipped_removals", std::move(skippedRemovals)    },
        { "details",          std::move(details)            }
    };
    if ( failures.wasTruncated() )
        frame.add( "truncated", true );
    return frame;
}

zypp::json::Array commitIssuesToJson( const CommitFailureLog & failures )
{
    zypp::json::Array issues;
    for ( const auto & entry : failures.entries() )
    {
        if ( entry.severity == CommitSeverity::Detail )
            continue; // routine context only, never surfaced on its own
        issues.add( detailToJson( entry ) );
    }
    return issues;
}
