#include "tools.h"
#include "validate.h"
#include "../transport.h"
#include "../system.h"
#include "zypp/base/Algorithm.h"

#include <zypp/ResPool.h>
#include <zypp/ResFilters.h>
#include <zypp/Resolver.h>
#include <zypp/PoolItem.h>
#include <zypp/Capability.h>
#include <zypp/Package.h>
#include <zypp-core/parser/json/JsonValue.h>

using namespace zypp;

// ─── Schema ──────────────────────────────────────────────────────────────────
static const zypp::json::Object kInstallSchema = {
    { "type", "object" },
    { "properties", zypp::json::Object{
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
            { "description", "Match by capability rather than exact name (default: true — match by capability)." },
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
        } } },
        // Reserved — not yet exposed to the LLM:
        // "force":            reinstall / downgrade / vendor change silently
        // "force_resolution": let solver find aggressive solution
        // "solver_focus":     Job / Update / Upgrade attitude
        { "testcase", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Path to a solver testcase directory. Omit to use the live system." }
        } } }
    } },
    { "required", zypp::json::Array{ "package" } }
};

const zypp::json::Object & schema_install() { return kInstallSchema; }

// ─── Helpers ─────────────────────────────────────────────────────────────────
// kindFromString() is in tools.h
namespace
{    zypp::json::Object planToJson( const std::string & toolName, const ResPool & pool )
    {
        zypp::json::Array toInstall, toRemove, licenses;

        for ( const auto & pi : pool )
        {
            if ( pi.status().isToBeInstalled() )
            {
                toInstall.add( zypp::json::Object{ {
                    { "name",    pi.name()               },
                    { "edition", pi.edition().asString() },
                    { "arch",    pi.arch().asString()     }
                } } );

                const std::string lic = pi.satSolvable().licenseToConfirm();
                if ( !lic.empty() && pi.satSolvable().needToAcceptLicense() )
                {
                    licenses.add( zypp::json::Object{ {
                        { "package", pi.name() },
                        { "text",    lic        }
                    } } );
                }
            }
            else if ( pi.status().isToBeUninstalled() )
            {
                toRemove.add( zypp::json::Object{ {
                    { "name",    pi.name()               },
                    { "edition", pi.edition().asString() },
                    { "arch",    pi.arch().asString()     }
                } } );
            }
        }

        return zypp::json::Object{ {
            { "type", "result"  },
            { "tool", toolName  },
            { "plan", zypp::json::Object{ {
                { "to_install", std::move(toInstall) },
                { "to_remove",  std::move(toRemove)  },
                { "licenses",   std::move(licenses)  }
            } } }
        } };
    }
}

// ─── Tool implementation ─────────────────────────────────────────────────────
int tool_install( const zypp::json::Object & arg, McpTransport & t )
{
    // ── Validate and parse all arguments upfront ──────────────────────────────
    // All validation happens before loadSystem() — fail fast, no ZYpp lock
    // acquired for bad input. Errors are actionable so the LLM can self-correct.

    const std::string pkg  = validate::requireNonEmpty( arg, "package" );
    const std::string type = validate::optionalEnum( arg, "type",
        { "package", "patch", "pattern", "product" } );
    const std::string repo = validate::optionalNonEmpty( arg, "repo" );

    // capability: default true (match by capability), false = match by name.
    // If repo is specified, name-based lookup is forced regardless.
    const bool byCapability =
        repo.empty() && validate::optionalBool( arg, "capability", true );

    const bool noRecommends = validate::optionalBool( arg, "no_recommends" );

    // solver_options — each sub-field is an optional boolean.
    bool allowDowngrade    = false;
    bool allowNameChange   = false;
    bool allowArchChange   = false;
    bool allowVendorChange = false;
    if ( arg.contains("solver_options") )
    {
        const auto & opts = arg.value("solver_options").asObject();
        allowDowngrade    = validate::optionalBool( opts, "allow_downgrade"    );
        allowNameChange   = validate::optionalBool( opts, "allow_name_change"  );
        allowArchChange   = validate::optionalBool( opts, "allow_arch_change"  );
        allowVendorChange = validate::optionalBool( opts, "allow_vendor_change");
    }

    // Reserved — not yet exposed:
    // const bool force           = false;  // reinstall / downgrade / vendor change
    // const bool forceResolution = false;  // aggressive solver
    // ResolverFocus solverFocus  = ResolverFocus::Job;

    const std::optional<zypp::Pathname> testcase =
        arg.contains("testcase")
            ? std::optional<zypp::Pathname>( validate::requireNonEmpty( arg, "testcase" ) )
            : std::nullopt;

    // ── Load pool ─────────────────────────────────────────────────────────────
    ZYpp::Ptr zypp = loadSystem( testcase );

    const ResKind kind = kindFromString( type );

    // ── Apply solver options ──────────────────────────────────────────────────
    Resolver_Ptr resolver = zypp->resolver();

    if ( noRecommends )
        resolver->setOnlyRequires( true );
    if ( allowDowngrade )
        resolver->setAllowDowngrade( true );
    if ( allowNameChange )
        resolver->setAllowNameChange( true );
    if ( allowArchChange )
        resolver->setAllowArchChange( true );
    if ( allowVendorChange )
        resolver->setAllowVendorChange( true );

    // Reserved:
    // resolver->setForceResolve( forceResolution );
    // resolver->setFocus( solverFocus );

    // ── Create the install job ────────────────────────────────────────────────
    if ( byCapability )
    {
        // Capability-based: let the solver find the best provider.
        // Mirrors deptestomatic's addExtraRequire / zypper's --capability mode.
        resolver->addRequire( Capability( pkg, kind ) );
    }
    else
    {
        // Name-based (or repo-restricted): find specific pool item, mark it.
        // Mirrors deptestomatic's get_poolItem + setToBeInstalled.
        PoolItem found;
        auto findAvailable = [&]( const PoolItem & pi ) -> bool {
            if ( pi.status().isInstalled() )
                return true; // skip — already installed
            if ( !repo.empty() &&
                 pi.satSolvable().repository().info().alias() != repo )
                return true; // skip — wrong repo
            found = pi;
            return false;   // stop
        };

        invokeOnEach( zypp->pool().byIdentBegin( kind, pkg ),
                      zypp->pool().byIdentEnd( kind, pkg ),
                      std::ref(findAvailable) );

        if ( !found )
        {
            const std::string detail = repo.empty()
                ? "Package '" + pkg + "' not found in any repository."
                : "Package '" + pkg + "' not found in repository '" + repo + "'.";
            t.writeFrame( zypp::json::Object{ {
                { "type",   "error"           },
                { "code",   "NOT_FOUND"        },
                { "tool",   "install_package"  },
                { "detail", detail             }
            } }.asJSON() );
            return 1;
        }

        found.status().setToBeInstalled( ResStatus::USER );
    }

    // ── Resolve ───────────────────────────────────────────────────────────────
    if ( !resolver->resolvePool() )
    {
        t.writeFrame( solverProblemsToJson( "install_package", resolver ).asJSON() );
        return 1;
    }

    t.writeFrame( planToJson( "install_package", zypp->pool() ).asJSON() );
    return 0;
}
