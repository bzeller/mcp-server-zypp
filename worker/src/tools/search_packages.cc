#include "tools.h"
#include "validate.h"
#include "../transport.h"
#include "../system.h"

#include <zypp/PoolQuery.h>
#include <zypp/ResPool.h>
#include <zypp/Capability.h>
#include <zypp/sat/SolvAttr.h>
#include <zypp/Pattern.h>
#include <zypp-core/base/Regex.h>
#include <zypp-core/parser/json/JsonValue.h>

using namespace zypp;

// ─── Schema ──────────────────────────────────────────────────────────────────
static const zypp::json::Object kSearchPackagesSchema = {
    { "type", "object" },
    { "properties", zypp::json::Object{
        { "pattern", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Search string. Enclose in '/' for regex (e.g. /^lib/). Wildcards * and ? supported." }
        } } },
        { "match", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "How to match the pattern." },
            { "enum",        zypp::json::Array{ "substrings", "words", "exact" } },
            { "default",     "substrings" }
        } } },
        { "search_in", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Which attribute to search. Default: name." },
            { "enum",        zypp::json::Array{
                "name", "provides", "requires", "recommends", "conflicts", "obsoletes"
            } },
            { "default", "name" }
        } } },
        { "search_descriptions", zypp::json::Object{ {
            { "type",        "boolean" },
            { "description", "Also search package summaries and descriptions (default: false)." },
            { "default",     false }
        } } },
        { "case_sensitive", zypp::json::Object{ {
            { "type",        "boolean" },
            { "description", "Case-sensitive search (default: false)." },
            { "default",     false }
        } } },
        { "installed_only", zypp::json::Object{ {
            { "type",        "boolean" },
            { "description", "Show only installed packages (default: false)." },
            { "default",     false }
        } } },
        { "not_installed_only", zypp::json::Object{ {
            { "type",        "boolean" },
            { "description", "Show only packages not yet installed (default: false)." },
            { "default",     false }
        } } },
        { "details", zypp::json::Object{ {
            { "type",        "boolean" },
            { "description", "Show one row per solvable (every repo/version) instead of one row per package name. Adds edition, arch and repo fields to each result (default: false)." },
            { "default",     false }
        } } },
        { "type", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Restrict search to this resolvable type." },
            { "enum",        zypp::json::Array{ "package", "patch", "pattern", "product" } }
        } } },
        { "repo", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Restrict search to this repository alias." }
        } } },
        { "testcase", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Path to a solver testcase directory. Omit to use the live system." }
        } } }
    } },
    { "required", zypp::json::Array{ "pattern" } }
};

const zypp::json::Object & schema_search_packages() { return kSearchPackagesSchema; }

// ─── Tool implementation ─────────────────────────────────────────────────────
int tool_search_packages( const zypp::json::Object & arg, McpTransport & t )
{
    // ── Validate arguments ────────────────────────────────────────────────────
    const std::string pattern     = validate::requireNonEmpty( arg, "pattern" );
    const std::string match       = validate::optionalEnum( arg, "match",
        { "substrings", "words", "exact" } );
    const std::string searchIn    = validate::optionalEnum( arg, "search_in",
        { "name", "provides", "requires", "recommends", "conflicts", "obsoletes" } );
    const bool searchDescriptions = validate::optionalBool( arg, "search_descriptions" );
    const bool caseSensitive      = validate::optionalBool( arg, "case_sensitive" );
    const bool installedOnly      = validate::optionalBool( arg, "installed_only" );
    const bool notInstalledOnly   = validate::optionalBool( arg, "not_installed_only" );
    const bool details            = validate::optionalBool( arg, "details" );
    const std::string type        = validate::optionalEnum( arg, "type",
        { "package", "patch", "pattern", "product" } );
    const std::string repo        = validate::optionalNonEmpty( arg, "repo" );

    if ( installedOnly && notInstalledOnly )
        ZYPP_THROW( zypp::Exception(
            "installed_only and not_installed_only are mutually exclusive." ) );

    const std::optional<zypp::Pathname> testcase =
        arg.contains("testcase")
            ? std::optional<zypp::Pathname>( validate::requireNonEmpty( arg, "testcase" ) )
            : std::nullopt;

    // ── Load pool ─────────────────────────────────────────────────────────────
    ZYpp::Ptr zypp = loadSystem( testcase );

    // ── Build PoolQuery — mirroring zypper search setup ───────────────────────
    PoolQuery query;

    if      ( match == "words" ) query.setMatchWord();
    else if ( match == "exact" ) query.setMatchExact();
    else                         query.setMatchSubstring();

    if ( caseSensitive )
        query.setCaseSensitive();
    if ( notInstalledOnly )
        query.setUninstalledOnly();
    if ( !type.empty() )
        query.addKind( kindFromString( type ) );
    if ( !repo.empty() )
        query.addRepo( repo );

    // Detect regex (/pattern/) and glob (pattern with * or ?) — mirrors zypper.
    std::string name = pattern;
    Match::Mode matchmode = Match::OTHER;
    if ( match.empty() )
    {
        if ( name.size() >= 2 && name.front() == '/' && name.back() == '/' )
        {
            name = name.substr( 1, name.size() - 2 );
            matchmode = Match::REGEX;
        }
        else if ( name.find_first_of("?*") != std::string::npos )
            matchmode = Match::GLOB;
    }

    Capability cap( name );
    sat::SolvAttr attr = searchInToAttr( searchIn );
    query.addDependency( attr, cap.detail().name().asString(),
                         cap.detail().op(), cap.detail().ed(),
                         Arch( cap.detail().arch() ), matchmode );

    if ( searchDescriptions )
    {
        query.addDependency( sat::SolvAttr::summary,     name, Rel::ANY, Edition(), Arch(), matchmode );
        query.addDependency( sat::SolvAttr::description, name, Rel::ANY, Edition(), Arch(), matchmode );
    }

    // ── Collect results ───────────────────────────────────────────────────────
    zypp::json::Array packages;

    if ( details )
    {
        // One entry per solvable — every repo/version combination.
        // Picklist filter suppresses @System duplicates that have an identical
        // available twin (which carries the real repo alias).
        for ( const sat::Solvable & solv : query )
        {
            if ( !picklistAccept( solv, installedOnly ) )
                continue;

            packages.add( zypp::json::Object{ {
                { "name",    solv.name()                                       },
                { "edition", solv.edition().asString()                         },
                { "arch",    solv.arch().asString()                            },
                { "repo",    solv.repository().alias()                         },
                { "status",  std::string( packageStatus( PoolItem(solv) ) )    },
                { "summary", solv.lookupStrAttribute( sat::SolvAttr::summary ) }
            } } );
        }
    }
    else
    {
        // One entry per package name — deduped via selectableBegin/End.
        // Edition/arch reflect the installed or candidate object.
        // Repo: for installed packages, resolved to the origin repo via
        // identicalAvailableObj() — @System only for genuine orphans.
        using S = zypp::misc::PoolInstallState;
        for ( auto it = query.selectableBegin(); it != query.selectableEnd(); ++it )
        {
            const ui::Selectable::constPtr & sel = *it;

            // Skip user-invisible patterns (mirrors zypper behaviour).
            if ( sel->kind() == ResKind::pattern )
                if ( const auto p = asKind<Pattern>( sel->theObj() ) )
                    if ( !p->userVisible() )
                        continue;

            if ( installedOnly || notInstalledOnly )
            {
                const auto f = zypp::misc::poolInstallState( *sel );
                const bool anyInstalled = bool( f & ( S::AutoInstalled | S::UserInstalled ) );
                if ( installedOnly  && !anyInstalled ) continue;
                if ( notInstalledOnly &&  anyInstalled ) continue;
            }

            // Resolve the representative object and repo.
            // Prefer the installed version's origin repo; fall back to candidate.
            PoolItem repObj;
            std::string repoAlias;
            if ( sel->hasInstalledObj() )
            {
                repObj = sel->installedObj();
                // identicalAvailableObj() returns the available twin if it exists,
                // giving us the real repo alias instead of @System.
                if ( PoolItem origin = sel->identicalAvailableObj( repObj ) )
                    repoAlias = origin.satSolvable().repository().alias();
                else
                    repoAlias = repObj.satSolvable().repository().alias(); // @System — genuine orphan
            }
            else if ( sel->hasCandidateObj() )
            {
                repObj    = sel->candidateObj();
                repoAlias = repObj.satSolvable().repository().alias();
            }
            else
            {
                repObj    = sel->theObj();
                repoAlias = repObj.satSolvable().repository().alias();
            }

            packages.add( zypp::json::Object{ {
                { "name",    sel->name()                                       },
                { "kind",    sel->kind().asString()                            },
                { "edition", repObj->edition().asString()                      },
                { "arch",    repObj->arch().asString()                         },
                { "repo",    repoAlias                                         },
                { "status",  std::string( packageStatus( *sel ) )              },
                { "summary", repObj->summary()                                 }
            } } );
        }
    }

    t.writeFrame( zypp::json::Object{ {
        { "type",     "result"            },
        { "tool",     "search_packages"   },
        { "packages", std::move(packages) }
    } }.asJSON() );
    return 0;
}
