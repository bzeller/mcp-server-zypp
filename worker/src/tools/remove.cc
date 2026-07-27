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
#include <zypp-core/parser/json/JsonValue.h>

using namespace zypp;

// ─── Schema ──────────────────────────────────────────────────────────────────
static const zypp::json::Object kRemoveSchema = {
    { "type", "object" },
    { "properties", zypp::json::Object{
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
            { "description", "Match by capability rather than exact name (default: false — match by name)." },
            { "default",     false }
        } } },
        { "clean_deps", zypp::json::Object{ {
            { "type",        "boolean" },
            { "description", "Automatically remove unneeded dependencies after removal (default: false)." },
            { "default",     false }
        } } },
        // Reserved — not yet exposed to the LLM:
        // "force_resolution": let solver find aggressive solution
        // "solver_focus":     Job / Update / Upgrade attitude
        { "testcase", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Path to a solver testcase directory. Omit to use the live system." }
        } } }
    } },
    { "required", zypp::json::Array{ "package" } }
};

const zypp::json::Object & schema_remove() { return kRemoveSchema; }

// ─── Tool implementation ─────────────────────────────────────────────────────
int tool_remove( const zypp::json::Object & arg, McpTransport & t )
{
    // ── Validate and parse all arguments upfront ──────────────────────────────
    // All validation happens before loadSystem() — fail fast, no ZYpp lock
    // acquired for bad input. Errors are actionable so the LLM can self-correct.

    const std::string pkg  = validate::requireNonEmpty( arg, "package" );
    const std::string type = validate::optionalEnum( arg, "type",
        { "package", "patch", "pattern", "product" } );
    const std::string repo = validate::optionalNonEmpty( arg, "repo" );

    // capability=false (default): find installed pool item, setToBeUninstalled.
    // capability=true: addConflict(Capability) — let solver figure out what to remove.
    const bool byCapability = validate::optionalBool( arg, "capability", false );

    const bool cleanDeps = validate::optionalBool( arg, "clean_deps" );

    // Reserved — not yet exposed:
    // const bool forceResolution = false;
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

    if ( cleanDeps )
        resolver->setCleandepsOnRemove( true );

    // Reserved:
    // resolver->setForceResolve( forceResolution );
    // resolver->setFocus( solverFocus );

    // ── Create the remove job ─────────────────────────────────────────────────
    if ( byCapability )
    {
        // Capability-based: let the solver find what provides the capability
        // and remove it. Mirrors deptestomatic's addConflict path.
        resolver->addConflict( Capability( pkg, kind ) );
    }
    else
    {
        // Name-based: find the specific installed pool item, mark it for removal.
        // Mirrors deptestomatic's get_poolItem("@System") + setToBeUninstalled.
        PoolItem found;
        auto findInstalled = [&]( const PoolItem & pi ) -> bool {
            if ( !pi.status().isInstalled() )
                return true; // skip — not installed
            if ( !repo.empty() &&
                 pi.satSolvable().repository().info().alias() != repo )
                return true; // skip — wrong repo
            found = pi;
            return false;   // stop
        };

        invokeOnEach( zypp->pool().byIdentBegin( kind, pkg ),
                      zypp->pool().byIdentEnd( kind, pkg ),
                      std::ref(findInstalled) );

        if ( !found )
        {
            // Not installed — return empty plan, not an error.
            t.writeFrame( zypp::json::Object{ {
                { "type", "result"         },
                { "tool", "remove_package" },
                { "plan", zypp::json::Object{ {
                    { "to_remove", zypp::json::Array{} }
                } } }
            } }.asJSON() );
            return 0;
        }

        found.status().setToBeUninstalled( ResStatus::USER );
    }

    // ── Resolve ───────────────────────────────────────────────────────────────
    if ( !resolver->resolvePool() )
    {
        t.writeFrame( solverProblemsToJson( "remove_package", resolver ).asJSON() );
        return 1;
    }

    zypp::json::Array toRemove;
    for ( const auto & pi : zypp->pool() )
    {
        if ( !pi.status().isToBeUninstalled() )
            continue;
        toRemove.add( zypp::json::Object{ {
            { "name",    pi.name()                },
            { "edition", pi.edition().asString()  },
            { "arch",    pi.arch().asString()      }
        } } );
    }

    t.writeFrame( zypp::json::Object{ {
        { "type", "result"         },
        { "tool", "remove_package" },
        { "plan", zypp::json::Object{ {
            { "to_remove", std::move(toRemove) }
        } } }
    } }.asJSON() );
    return 0;
}
