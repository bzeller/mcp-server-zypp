#include "tools.h"
#include "validate.h"
#include "../transport.h"
#include "../context.h"

#include <vector>
#include <unordered_map>

#include <zypp/PoolQuery.h>
#include <zypp/ResPool.h>
#include <zypp/sat/Pool.h>
#include <zypp/sat/SolvAttr.h>
#include <zypp/sat/Queue.h>
#include <zypp/Capability.h>
#include <zypp-core/parser/json/JsonValue.h>

using namespace zypp;

// ─── Schema ──────────────────────────────────────────────────────────────────
static const zypp::json::Object kFindDependentsSchema = {
    { "type", "object" },
    { "properties", zypp::json::Object{
        { "package", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "The package name to find dependents of." }
        } } },
        { "relation", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Dependency relation to search. "
                             "'requires': what needs this package. "
                             "'recommends': what recommends this package. "
                             "'conflicts': what conflicts with this package. "
                             "'obsoletes': what obsoletes this package." },
            { "enum",        zypp::json::Array{ "requires", "recommends", "conflicts", "obsoletes" } },
            { "default",     "requires" }
        } } },
        { "exact", zypp::json::Object{ {
            { "type",        "boolean" },
            { "description", "Exact name match for the initial package lookup (default: true). "
                             "Set false for substring match — may return a much larger set." },
            { "default",     true }
        } } },
        { "repo", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Restrict results to this repository alias." }
        } } },
        { "type", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Restrict results to this resolvable type (default: package). "
                             "Use 'all' to include patches, patterns and products." },
            { "enum",        zypp::json::Array{ "package", "patch", "pattern", "product", "all" } },
            { "default",     "package" }
        } } },
        { "testcase", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Path to a solver testcase directory. Omit to use the live system." }
        } } },
        { "installed_only", zypp::json::Object{ {
           { "type",        "boolean" },
           { "description", "Show only installed packages (default: false)." },
           { "default",     false }
       } } }
    } },
    { "required", zypp::json::Array{ "package" } }
};

const zypp::json::Object & schema_find_dependents() { return kFindDependentsSchema; }

// ─── Tool implementation ─────────────────────────────────────────────────────
int tool_find_dependents( const zypp::json::Object & arg, ToolContext & ctx )
{
    McpTransport & t = ctx.transport();

    // ── Validate arguments ────────────────────────────────────────────────────
    const std::string pkg      = validate::requireNonEmpty( arg, "package" );
    const std::string relation = validate::optionalEnum( arg, "relation",
        { "requires", "recommends", "conflicts", "obsoletes" } );
    const bool exact           = validate::optionalBool( arg, "exact", true );
    const std::string repo     = validate::optionalNonEmpty( arg, "repo" );
    const std::string type     = validate::optionalEnum( arg, "type",
        { "package", "patch", "pattern", "product", "all" } );

    const bool installedOnly = validate::optionalBool( arg, "installed_only" );

    // ── Load pool ─────────────────────────────────────────────────────────────
    ZYpp::Ptr zypp = ctx.loadSystemFromArg( arg );

    // Pre-compute all filters once.
    const sat::SolvAttr relAttr = relationToAttr( relation );
    const bool filterKind       = ( type != "all" );
    const ResKind kindFilter    = kindFromString( type );
    const bool filterRepo       = !repo.empty();

    // ── Step 1: seed query — find the named package(s) ────────────────────────
    // Exact match by default so "glibc" does not pull in "glibc-devel" etc.
    // into the provides set, which would inflate the dependent list.
    PoolQuery seedQuery;
    seedQuery.addKind( ResKind::package );
    seedQuery.addDependency( sat::SolvAttr::name, pkg );
    if ( exact )
        seedQuery.setMatchExact();
    else
        seedQuery.setMatchSubstring();

    std::vector<sat::Solvable> seeds;
    for ( const sat::Solvable & slv : seedQuery ) {
        seeds.push_back( slv );
    }

    if ( seeds.empty() )
    {
        t.writeFrame( zypp::json::Object{ {
            { "type",   "error"           },
            { "code",   "NOT_FOUND"       },
            { "tool",   "find_dependents" },
            { "detail", "No package named '" + pkg + "' found. "
                        "Try exact=false for substring search, or use "
                        "search_packages to find the correct name." }
        } }.asJSON() );
        return 1;
    }

    // ── Step 2: for each seed find dependents via whatMatchesSolvable ─────────
    // Mirrors zypper's *-pkg implementation:
    //   sat::Queue q = sat::Pool::instance().whatMatchesSolvable(attr, slv)
    std::unordered_map<sat::Solvable::IdType, sat::Solvable> seen;

    for ( const sat::Solvable & seed : seeds )
    {
        sat::Queue q = sat::Pool::instance().whatMatchesSolvable( relAttr, seed );
        for ( auto solvId : q )
        {
            sat::Solvable matched( static_cast<sat::Solvable::IdType>( solvId ) );
            if ( filterKind && matched.kind() != kindFilter )
                continue;
            if ( filterRepo && matched.repository().alias() != repo )
                continue;
            if ( !picklistAccept( matched, installedOnly ) )
                continue;
            seen.emplace( matched.id(), matched );
        }
    }

    // ── Emit results ──────────────────────────────────────────────────────────
    zypp::json::Array packages;
    for ( const auto & kv : seen )
    {
        const sat::Solvable & solv = kv.second;
        packages.add( zypp::json::Object{ {
            { "name",      solv.name()                                       },
            { "edition",   solv.edition().asString()                         },
            { "arch",      solv.arch().asString()                            },
            { "repo",      solv.repository().alias()                         },
            { "status",    std::string( packageStatus( PoolItem(solv) ) )    },
            { "summary",   solv.lookupStrAttribute( sat::SolvAttr::summary ) }
        } } );
    }

    t.writeFrame( zypp::json::Object{ {
        { "type",       "result"                                                 },
        { "tool",       "find_dependents"                                        },
        { "relation",   relation.empty() ? std::string("requires") : relation   },
        { "seed_count", static_cast<int64_t>( seeds.size() )                    },
        { "packages",   std::move(packages)                                      }
    } }.asJSON() );
    return 0;
}
