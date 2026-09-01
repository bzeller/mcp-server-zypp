#ifndef MCP_SERVER_ZYPP_SOLVER_PACKAGEARGS_H
#define MCP_SERVER_ZYPP_SOLVER_PACKAGEARGS_H

// Ported from zypper's PackageArgs. Two changes from upstream:
//
// 1. zypper.out().error() + setExitCode() + ZYPP_THROW(ExitRequestException)
//    become a single thrown InvalidArgumentError carrying the same detail
//    text. The three upstream steps were report-then-unconditionally-abort
//    collapsed into one control-flow event with a single call site (the
//    parse itself) and a single catch site (the caller) — one exception is
//    sufficient; there is nothing here that needs accumulating the way
//    Feedback does. The caller catches it and maps it to INVALID_ARG (see
//    REVIEW.md F15).
//
// 2. match_repo() — upstream's repo-argument matcher — resolved a "repo:"
//    prefix against Zypper::instance().repoManager() by alias, number,
//    name, or URL, plus a CLI-only "temporary_repos" list. This port
//    narrows that deliberately to alias-only matching. The caller may pass
//    an explicit set of known aliases (knownRepoAliases); when omitted
//    entirely (std::nullopt, the default — NOT the same as an explicit
//    empty set), the parser falls back to enumerating
//    zypp::sat::Pool::instance()'s currently loaded repo aliases directly.
//    That fallback is the one deliberate exception to "no ambient reads"
//    in this file: sat::Pool is the one process-wide pool singleton
//    every other tool in this worker already reads (see
//    worker/src/system.cc), not a hidden policy/config global like
//    Zypper::instance() — and a caller that wants full purity simply
//    supplies knownRepoAliases explicitly, bypassing the fallback
//    entirely. Number/name/URL matching and temporary_repos are CLI
//    ergonomics with no equivalent in MCP's structured-argument world and
//    are not ported.
//
// A second constructor is added (not present upstream) taking dos/donts
// directly, for callers that already have structured package specs and
// have no CLI string to parse — MCP is the primary user of that one, but
// nothing prevents a future caller depending on the fully-parsed form.
//
// PackageSpecCompare's capability-only ordering is preserved verbatim; see
// packagespec.h.

#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <zypp/ResKind.h>

#include "packagespec.h"

namespace solverequest
{

/// Thrown by PackageArgs' CLI-parsing constructor on a genuinely
/// malformed argument (a bare install/remove modifier, or a CLI option
/// string appearing where a package name was expected).
struct InvalidArgumentError : std::runtime_error
{
    explicit InvalidArgumentError( std::string detail ) : std::runtime_error( std::move(detail) ) {}
};

class PackageArgs
{
public:
    using StringSet = std::set<std::string>;

    struct Options
    {
        // Deliberately NOT a default member initializer (`= true`): Options
        // is used as a default-argument value (`= Options()`) in this
        // class's own constructor declarations below, and some compilers
        // (observed: Clang) reject a default member initializer referenced
        // that way with "needed within definition of enclosing class
        // outside of member functions" — a complete-class-context quirk.
        // An explicit constructor with a member-initializer list sidesteps
        // it entirely, and matches upstream zypper's own PackageArgs::Options,
        // which uses this exact style rather than an NSDMI.
        Options() : doByDefault( true ) {}

        /// Whether to do (install/update) or don't (remove) by default,
        /// if +/- is not specified in an argument.
        bool doByDefault;
    };

    /// CLI-style constructor: takes raw argument strings and parses them
    /// exactly as zypper does (join at comparison operators, +/-/~/!
    /// modifiers, "repo:" prefix, Capability::guessPackageSpec()).
    /// Throws InvalidArgumentError on a malformed argument.
    ///
    /// Parameter order deliberately matches upstream's original
    /// PackageArgs(args, kind, opts) exactly, with knownRepoAliases
    /// appended as a new trailing parameter — so existing call sites
    /// (zypper's own, and the ported test suite's) need no changes.
    /// See this header's point 2 above for knownRepoAliases' std::nullopt
    /// default.
    PackageArgs( const std::vector<std::string> & args,
                 const zypp::ResKind & kind = zypp::ResKind::package,
                 const Options & opts = Options(),
                 const std::optional<StringSet> & knownRepoAliases = std::nullopt );

    /// Structured constructor: the caller has already resolved dos/donts
    /// (e.g. from JSON tool arguments) and wants PackageArgs purely as
    /// the carrier zypper's algorithms expect. No parsing; cannot throw.
    PackageArgs( PackageSpecSet dos, PackageSpecSet donts, Options opts = Options() );

    const Options & options() const { return _opts; }

    const StringSet & asStringSet() const { return _args; }

    /// Capabilities we want to install/upgrade and don't want to remove,
    /// plus associated requested repo.
    const PackageSpecSet & dos() const { return _dos; }
    /// Capabilities we don't want to install/upgrade, or want to remove.
    const PackageSpecSet & donts() const { return _donts; }

    bool empty() const { return dos().empty() && donts().empty(); }

private:
    void preprocess( const std::vector<std::string> & args );
    void argsToCaps( const zypp::ResKind & kind, const std::optional<StringSet> & knownRepoAliases );

    Options        _opts;
    StringSet      _args;
    PackageSpecSet _dos;
    PackageSpecSet _donts;
};

} // namespace solverequest

std::ostream & operator<<( std::ostream & out, const solverequest::PackageSpec & spec );

#endif // MCP_SERVER_ZYPP_SOLVER_PACKAGEARGS_H
