#include "registry.h"
#include "../transport.h"
#include "../context.h"

#include <zypp/ResPool.h>
#include <zypp/Patch.h>
#include <zypp-core/Date.h>
#include <zypp-core/base/String.h>
#include <zypp-core/base/Regex.h>
#include <zypp-core/parser/json/JsonValue.h>

using namespace zypp;

// ─── Schema ──────────────────────────────────────────────────────────────────
static const zypp::json::Object kUpdatesSchema = {
    { "type", "object" },
    { "properties", zypp::json::Object{
        { "applicable_only", zypp::json::Object{ {
            { "type",        "boolean" },
            { "description", "Only return patches that are needed (default: true). Set false to list all patches including already satisfied ones." },
            { "default",     true }
        } } },
        { "category", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Filter by patch category: security, recommended, bugfix, optional, feature." },
            { "enum",        zypp::json::Array{ "security", "recommended", "bugfix", "optional", "feature" } }
        } } },
        { "severity", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Filter by severity: critical, important, moderate, low." },
            { "enum",        zypp::json::Array{ "critical", "important", "moderate", "low" } }
        } } },
        { "date", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Select patches issued up to but not including this date (YYYY-MM-DD)." },
            { "pattern",     "^[0-9]{4}-[0-9]{2}-[0-9]{2}$" }
        } } },
        { "cve", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Filter patches that fix the specified CVE identifier (e.g. CVE-2024-1234)." }
        } } },
        { "bugzilla", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Filter patches that fix the specified Bugzilla issue number." }
        } } },
        { "testcase", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Path to a solver testcase directory. Omit to use the live system." }
        } } }
    } }
};

const zypp::json::Object & schema_updates() { return kUpdatesSchema; }

// ─── Helpers ─────────────────────────────────────────────────────────────────
namespace
{
    /// Throw a descriptive exception for an invalid argument value.
    /// The message is designed so the LLM can self-correct and retry.
    [[ noreturn ]] void invalidArg( const std::string & name,
                                    const std::string & value,
                                    const std::string & hint )
    {
        ZYPP_THROW( zypp::Exception(
            "Invalid value for argument '" + name + "': '" + value + "'. " + hint ) );
    }

    /// Check if patch references contain the given CVE or bugzilla id.
    bool patchMatchesIssue( Patch::constPtr patch,
                            const std::string & cve,
                            const std::string & bugzilla )
    {
        if ( cve.empty() && bugzilla.empty() )
            return true;

        for ( auto it = patch->referencesBegin();
              it != patch->referencesEnd(); ++it )
        {
            if ( !cve.empty() && it.type() == "cve" &&
                 str::compareCI( it.id(), cve ) == 0 )
                return true;
            if ( !bugzilla.empty() && it.type() == "bugzilla" &&
                 it.id() == bugzilla )
                return true;
        }
        return false;
    }

    /// Emit issue references as a JSON array.
    zypp::json::Array patchIssues( Patch::constPtr patch )
    {
        zypp::json::Array issues;
        for ( auto it = patch->referencesBegin();
              it != patch->referencesEnd(); ++it )
        {
            issues.add( zypp::json::Object{ {
                { "id",    it.id()    },
                { "type",  it.type()  },
                { "title", it.title() },
                { "href",  it.href()  }
            } } );
        }
        return issues;
    }

    /// Valid values for the `category` and `severity` enum arguments, checked
    /// via `isValidEnum` below against the caller-supplied `initializer_list`.
    bool isValidEnum( std::string_view value,
                      std::initializer_list<std::string_view> allowed )
    {
        for ( auto & v : allowed )
            if ( str::compareCI( std::string(value), std::string(v) ) == 0 )
                return true;
        return false;
    }
}

// ─── Tool implementation ─────────────────────────────────────────────────────
int tool_updates( const zypp::json::Object & arg, ToolContext & ctx )
{
    McpTransport & t = ctx.transport();

    // ── Validate and parse all arguments upfront ──────────────────────────────
    // All validation happens before loadSystem() so bad input fails fast
    // without acquiring the ZYpp lock or touching the pool.

    const bool applicableOnly =
        !arg.contains("applicable_only") ||
        arg.value("applicable_only").asBool();

    std::string filterCategory;
    if ( arg.contains("category") )
    {
        filterCategory = static_cast<std::string>( arg.value("category").asString() );
        if ( !isValidEnum( filterCategory,
                { "security", "recommended", "bugfix", "optional", "feature" } ) )
            invalidArg( "category", filterCategory,
                "Must be one of: security, recommended, bugfix, optional, feature." );
    }

    std::string filterSeverity;
    if ( arg.contains("severity") )
    {
        filterSeverity = static_cast<std::string>( arg.value("severity").asString() );
        if ( !isValidEnum( filterSeverity,
                { "critical", "important", "moderate", "low" } ) )
            invalidArg( "severity", filterSeverity,
                "Must be one of: critical, important, moderate, low." );
    }

    Date filterDate;
    if ( arg.contains("date") )
    {
        const std::string dateStr =
            static_cast<std::string>( arg.value("date").asString() );
        try
        {
            filterDate = Date( dateStr, "%Y-%m-%d" );
        }
        catch ( const DateFormatException & )
        {
            invalidArg( "date", dateStr,
                "Expected format: YYYY-MM-DD (e.g. 2024-03-15)." );
        }
    }

    std::string filterCve;
    if ( arg.contains("cve") )
    {
        filterCve = static_cast<std::string>( arg.value("cve").asString() );
        // CVE-YYYY-NNNNN — year 4 digits, ID 4+ digits, case-insensitive prefix.
        static const str::regex rxCve( "^CVE-[0-9]{4}-[0-9]{4,}$",
                                       str::regex::icase );
        if ( !str::regex_match( filterCve, rxCve ) )
            invalidArg( "cve", filterCve,
                "Expected CVE identifier format: CVE-YYYY-NNNNN "
                "(e.g. CVE-2024-1234). Year must be 4 digits, "
                "ID must be 4 or more digits." );
    }

    std::string filterBugzilla;
    if ( arg.contains("bugzilla") )
    {
        filterBugzilla = static_cast<std::string>( arg.value("bugzilla").asString() );
        // Bugzilla IDs are purely numeric, at least 1 digit.
        static const str::regex rxBug( "^[0-9]+$" );
        if ( !str::regex_match( filterBugzilla, rxBug ) )
            invalidArg( "bugzilla", filterBugzilla,
                "Expected a numeric Bugzilla issue number (e.g. 1234567)." );
    }

    // ── Load system — ZYpp lock acquired here ─────────────────────────────────
    ZYpp::Ptr zypp = ctx.loadSystemFromArg( arg );

    // ── Iterate patches ───────────────────────────────────────────────────────
    zypp::json::Array patches;
    for ( const auto & pi : zypp->pool() )
    {
        if ( !pi.isKind<Patch>() )
            continue;

        // applicable_only: use isNeeded() which includes broken + pending-install,
        // excludes locked. Without filter: include all patches.
        if ( applicableOnly && !pi.isNeeded() )
            continue;

        Patch::constPtr patch = asKind<Patch>( pi.resolvable() );
        if ( !patch )
            continue;

        if ( !filterCategory.empty() && !patch->isCategory( filterCategory ) )
            continue;

        if ( !filterSeverity.empty() && !patch->isSeverity( filterSeverity ) )
            continue;

        // Date filter: skip patches issued on or after filterDate.
        if ( filterDate && patch->timestamp() >= filterDate )
            continue;

        if ( !patchMatchesIssue( patch, filterCve, filterBugzilla ) )
            continue;

        patches.add( zypp::json::Object{ {
            { "name",        patch->name()                                   },
            { "edition",     patch->edition().asString()                     },
            { "category",    patch->category()                               },
            { "severity",    patch->severity()                               },
            { "date",        patch->timestamp().asString()                   },
            { "interactive", patch->interactiveWhenIgnoring( Patch::Reboot ) },
            { "reboot",      patch->rebootSuggested()                        },
            { "issues",      patchIssues( patch )                            }
        } } );
    }

    zypp::json::Object result{ {
        { "type",    "result"        },
        { "tool",    "check_updates" },
        { "patches", std::move(patches) }
    } };
    t.writeFrame( result.asJSON() );
    return 0;
}
