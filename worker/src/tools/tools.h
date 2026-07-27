#ifndef MCP_SERVER_ZYPP_TOOLS_H
#define MCP_SERVER_ZYPP_TOOLS_H

#include "registry.h"

#include <string>
#include <zypp/ResKind.h>
#include <zypp/sat/SolvAttr.h>
#include <zypp/PoolItem.h>
#include <zypp/ui/Selectable.h>
#include <zypp/misc/PoolInstallState.h>

// ─── Shared conversion helpers ────────────────────────────────────────────────
// Used across multiple tool implementations — not validation, just mapping.

inline zypp::ResKind kindFromString( const std::string & s )
{
    const auto k = zypp::ResKind::fromBuiltin (s);
    if ( k == zypp::ResKind::nokind ) return zypp::ResKind::package;  // default and "all" — callers handle "all" separately
    return k;
}

inline zypp::sat::SolvAttr relationToAttr( const std::string & rel )
{
    if ( rel == "recommends" ) return zypp::sat::SolvAttr::dep_recommends;
    if ( rel == "conflicts"  ) return zypp::sat::SolvAttr::dep_conflicts;
    if ( rel == "obsoletes"  ) return zypp::sat::SolvAttr::dep_obsoletes;
    return zypp::sat::SolvAttr::dep_requires; // default
}

inline zypp::sat::SolvAttr searchInToAttr( const std::string & s )
{
    if ( s == "provides"   ) return zypp::sat::SolvAttr::dep_provides;
    if ( s == "requires"   ) return zypp::sat::SolvAttr::dep_requires;
    if ( s == "recommends" ) return zypp::sat::SolvAttr::dep_recommends;
    if ( s == "conflicts"  ) return zypp::sat::SolvAttr::dep_conflicts;
    if ( s == "obsoletes"  ) return zypp::sat::SolvAttr::dep_obsoletes;
    return zypp::sat::SolvAttr::name; // default
}

// ─── Internal helper ─────────────────────────────────────────────────────────
inline std::string_view poolInstallStateFlagsToStatus( zypp::misc::PoolInstallStateFlags f )
{
    using S = zypp::misc::PoolInstallState;
    if ( f & S::Satisfied )           return "satisfied";
    if ( f & S::Broken    )           return "broken";
    const bool otherVer = bool( f & S::OtherVersionInstalled );
    const bool isAuto   = bool( f & S::AutoInstalled );
    const bool isUser   = bool( f & S::UserInstalled  );
    if ( !isAuto && !isUser )         return "not-installed";
    if ( otherVer && isAuto )         return "other-version-auto";
    if ( otherVer )                   return "other-version-user";
    if ( isAuto   )                   return "auto-installed";
    return "user-installed";
}

/** Map a \ref PoolItem to a stable status string token.
 *
 * Use in the details (per-solvable) path. The Selectable is looked up
 * internally for the \c OtherVersionInstalled check.
 *
 * Tokens: \c not-installed / \c installed / \c auto-installed /
 *         \c other-version / \c other-version-auto / \c satisfied / \c broken
 */
inline std::string_view packageStatus( const zypp::PoolItem & pi )
{
    return poolInstallStateFlagsToStatus( zypp::misc::poolInstallState( pi ) );
}

/** \overload taking a \ref ui::Selectable — use in the deduped-per-name path.
 *
 * Delegates to \ref poolInstallState(Selectable) which correctly sets
 * \c OtherVersionInstalled via \c identicalAvailable() — something the
 * PoolItem overload cannot do for \c \@System solvables.
 */
inline std::string_view packageStatus( const zypp::ui::Selectable & sel )
{
    return poolInstallStateFlagsToStatus( zypp::misc::poolInstallState( sel ) );
}

/** Picklist-based accept predicate for search result loops.
 *
 * Mirrors the logic in zypper's \c FillSearchTableSolvable: the PoolQuery
 * delivers \e both available and installed solvables. For an installed package
 * that has an identical available solvable, libzypp injects the installed
 * \c \@System solvable but assigns it \c picklistNoPos — signalling "use the
 * available one instead, it carries the real repo".
 *
 * Returning \c false from here drops the solvable from the result set.
 * Callers must NOT additionally filter by \c isSystem() — this predicate
 * subsumes that check correctly.
 *
 * \param solv         The solvable to test.
 * \param installedOnly  If true, also reject solvables for which nothing is
 *                       installed (status == "not-installed").
 */
inline bool picklistAccept( const zypp::sat::Solvable & solv, bool installedOnly )
{
    zypp::ui::Selectable::Ptr sel = zypp::ui::Selectable::get( solv );
    if ( !sel )
        return false;

    // Drop solvables superseded by an identical available — they carry @System
    // as repo, which is misleading. The available twin will appear separately.
    if ( sel->picklistPos( solv ) == zypp::ui::Selectable::picklistNoPos )
        return false;

    if ( installedOnly )
    {
        using S = zypp::misc::PoolInstallState;
        const auto f = zypp::misc::poolInstallState( *sel );
        // Reject if nothing is installed for this selectable at all.
        if ( !( f & ( S::AutoInstalled | S::UserInstalled ) ) )
            return false;
    }

    return true;
}

inline zypp::json::Object solverProblemsToJson( const std::string & toolName, zypp::Resolver_Ptr resolver )
{
    zypp::json::Array problems;
    for ( const auto & p : resolver->problems() )
        problems.add( zypp::json::Object{ { { "description", p->description() } } } );

    return zypp::json::Object{ {
        { "type",     "error"        },
        { "tool",     toolName       },
        { "code",     "SOLVER_ERROR" },
        { "problems", std::move(problems) }
    } };
}

#endif // MCP_SERVER_ZYPP_TOOLS_H
