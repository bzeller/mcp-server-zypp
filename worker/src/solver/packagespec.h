#ifndef MCP_SERVER_ZYPP_SOLVER_PACKAGESPEC_H
#define MCP_SERVER_ZYPP_SOLVER_PACKAGESPEC_H

// Ported near-verbatim from zypper's PackageSpec/PackageSpecCompare. No
// zypper dependency in either type.

#include <set>
#include <string>
#include <iosfwd>

#include <zypp/Capability.h>

namespace solverequest
{

/// One resolved argument: the capability the caller wants (or doesn't
/// want, depending which set it ends up in — see PackageArgs::dos()/
/// donts()), plus enough context to explain the decision in feedback.
struct PackageSpec
{
    PackageSpec() = default;

    std::string      orig_str;    ///< as given by the caller, for feedback/logging
    zypp::Capability parsed_cap;
    std::string      repo_alias;  ///< restrict candidate lookup to this repo, if set
    bool             modified = false; ///< true if guessPackageSpec() had to correct orig_str
};

/// Only compares parsed capabilities. Even though repo_alias may differ,
/// if the capability is the same we must rule out one of two conflicting
/// entries (see PackageArgs::argsToCaps()'s dedup logic) — this is
/// load-bearing, not incidental. Do not widen to a full-field comparison.
struct PackageSpecCompare
{
    bool operator()( const PackageSpec & lhs, const PackageSpec & rhs ) const
    { return lhs.parsed_cap < rhs.parsed_cap; }
};

using PackageSpecSet = std::set<PackageSpec, PackageSpecCompare>;

} // namespace solverequest

std::ostream & operator<<( std::ostream & out, const solverequest::PackageSpec & spec );

#endif // MCP_SERVER_ZYPP_SOLVER_PACKAGESPEC_H
