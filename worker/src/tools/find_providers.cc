#include "tools.h"
#include "validate.h"
#include "../transport.h"
#include "../system.h"

#include <zypp/ResPool.h>
#include <zypp/sat/WhatProvides.h>
#include <zypp/Capability.h>
#include <zypp/sat/SolvAttr.h>
#include <zypp-core/parser/json/JsonValue.h>

using namespace zypp;

// ─── Schema ──────────────────────────────────────────────────────────────────
static const zypp::json::Object kFindProvidersSchema = {
    { "type", "object" },
    { "properties", zypp::json::Object{
        { "capability", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "The capability to find providers of. "
                             "Can be a package name, soname, or versioned capability "
                             "(e.g. 'curl', 'libcurl.so.4', 'curl >= 7.0')." }
        } } },
        { "repo", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Restrict results to this repository alias." }
        } } },
        { "type", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Resolvable type to return (default: package). "
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
    { "required", zypp::json::Array{ "capability" } }
};

const zypp::json::Object & schema_find_providers() { return kFindProvidersSchema; }

// ─── Tool implementation ─────────────────────────────────────────────────────
int tool_find_providers( const zypp::json::Object & arg, McpTransport & t )
{
    // ── Validate arguments ────────────────────────────────────────────────────
    const std::string capStr = validate::requireNonEmpty( arg, "capability" );
    const std::string repo   = validate::optionalNonEmpty( arg, "repo" );
    const std::string type   = validate::optionalEnum( arg, "type",
        { "package", "patch", "pattern", "product", "all" } );

    const std::optional<zypp::Pathname> testcase =
        arg.contains("testcase")
            ? std::optional<zypp::Pathname>( validate::requireNonEmpty( arg, "testcase" ) )
            : std::nullopt;

    const bool installedOnly = validate::optionalBool( arg, "installed_only" );

    // ── Load pool ─────────────────────────────────────────────────────────────
    ZYpp::Ptr zypp = loadSystem( testcase );

    // Default to package — WhatProvides returns all kinds; users asking
    // "what provides libcurl?" want packages, not patches. Use type=all to override.
    const bool filterKind    = ( type != "all" );
    const ResKind kindFilter  = kindFromString( type ); // "all" → package, but filterKind=false
    const bool filterRepo    = !repo.empty();

    // ── Find providers via sat::WhatProvides ──────────────────────────────────
    const sat::WhatProvides providers( (Capability( capStr )) );

    zypp::json::Array packages;
    for ( const sat::Solvable & solv : Iterable(providers.begin(), providers.end()) )
    {
        if ( filterKind && solv.kind() != kindFilter )
            continue;
        if ( filterRepo && solv.repository().alias() != repo )
            continue;
        if ( !picklistAccept( solv, installedOnly ) )
            continue;

        packages.add( zypp::json::Object{ {
            { "name",      solv.name()                                       },
            { "edition",   solv.edition().asString()                         },
            { "arch",      solv.arch().asString()                            },
            { "repo",      solv.repository().alias()                         },
            { "status",    std::string( packageStatus( PoolItem(solv) ) )    },
            { "summary",   solv.lookupStrAttribute( sat::SolvAttr::summary ) }
        } } );
    }

    if ( packages.size() == 0 )
    {
        const std::string typeDesc = ( type.empty() || type == "all" ) ? "resolvable" : type;
        t.writeFrame( zypp::json::Object{ {
            { "type",   "error"          },
            { "code",   "NOT_FOUND"      },
            { "tool",   "find_providers" },
            { "detail", "No " + typeDesc + " provides '" + capStr + "'." }
        } }.asJSON() );
        return 1;
    }

    t.writeFrame( zypp::json::Object{ {
        { "type",     "result"            },
        { "tool",     "find_providers"    },
        { "packages", std::move(packages) }
    } }.asJSON() );
    return 0;
}
