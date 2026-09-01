#ifndef MCP_SERVER_ZYPP_SOLVER_REQUESTER_H
#define MCP_SERVER_ZYPP_SOLVER_REQUESTER_H

// Ported from zypper's SolverRequester. See requester.cc for the detailed
// per-function porting notes. Summary of what changed structurally:
//
//   upstream SolverRequester           this port
//   ─────────────────────────          ─────────────────────────────────
//   const Options _opts                Options, immutable, ctor-supplied
//   ZypperCommand _command              Operation, set per Job by submit()
//   std::vector<Feedback> _feedback     same, accumulates across submit()s
//   getZYpp()->pool()                   ResPool passed explicitly to submit()
//   getZYpp()->resolver()->addRequire/  never called here at all — becomes
//     addConflict()                     a Selection the caller applies later
//   pi.status().setToBeInstalled() /    same — deferred into a Selection,
//     Selectable::setOnSystem()         see selection.h: Selection::forced
//
// One honest caveat: zypp::sat::WhatProvides and zypp::ui::Selectable::get()
// are themselves backed by the one process-wide zypp::sat::Pool singleton
// in current libzypp — there is no libzypp API to construct an independent
// pool instance. Passing ResPool explicitly to submit() therefore documents
// the dependency and is a real improvement in API clarity, but it does not
// (and currently cannot) fully eliminate every implicit singleton touch
// inside libzypp's own query helpers. What IS fully eliminated is every
// zypper-owned global (Zypper::instance(), ZConfig::instance(),
// SolverSettings::instance(), LicenseAgreementPolicy::instance()) and every
// direct Resolver_Ptr mutation — those are the couplings this port exists
// to remove.

#include <set>
#include <vector>

#include <zypp/PoolItem.h>
#include <zypp/ResPool.h>

#include "feedback.h"
#include "options.h"
#include "packagespec.h"
#include "selection.h"

namespace solverequest
{

/// A session that accumulates Selections and Feedback across one or more
/// submit() calls, exactly as upstream's SolverRequester accumulates
/// across multiple install()/remove()/update() calls on one instance.
/// submit() itself never mutates a PoolItem's status and never touches a
/// Resolver — see apply.h for the step that actually does.
class Requester
{
public:
    Requester() = default;
    explicit Requester( Options opts ) : _opts( std::move(opts) ) {}

    /// Process one Job against pool, appending to selections()/feedback().
    /// Pure with respect to package/resolver state (see the header
    /// comment's caveat re: sat::WhatProvides/ui::Selectable). Emits
    /// libzypp MIL/DBG/WAR trace exactly as upstream did — see
    /// requester.cc's top-of-file note.
    void submit( const zypp::ResPool & pool, const Job & job );

    bool hasFeedback( Feedback::Id id ) const;
    const std::vector<Feedback>  & feedback()   const { return _feedback; }
    const std::vector<Selection> & selections() const { return _selections; }

    // Derived views over _selections — not separate state. Signatures
    // mirror upstream's toInstall()/toRemove()/dep_requires()/
    // dep_conflicts() exactly.
    std::set<zypp::PoolItem>   toInstall()     const;
    std::set<zypp::PoolItem>   toRemove()      const;
    std::set<zypp::Capability> dep_requires()  const;
    std::set<zypp::Capability> dep_conflicts() const;

    const Effects & effects() const { return _effects; }

private:
    void installRemove( const zypp::ResPool &, const PackageSpecSet & dos, const PackageSpecSet & donts );
    void install( const zypp::ResPool &, const PackageSpec & pkg );
    void remove ( const zypp::ResPool &, const PackageSpec & pkg );
    void updateTo( const zypp::ResPool &, const PackageSpec & pkg, const zypp::PoolItem & selected );

    bool installPatch( const zypp::PoolItem & selected );
    bool installPatch( const PackageSpec & patchspec, const zypp::PoolItem & selected, bool ignore_pkgmgmt = true );
    void updatePatches( const zypp::ResPool &, bool updateStackOnly );
    void updatePatterns();

    void setToInstall( const zypp::PoolItem & pi );
    void setToRemove ( const zypp::PoolItem & pi );
    void addRequirement( const PackageSpec & pkg );
    void addConflict   ( const PackageSpec & pkg );

    void addFeedback( Feedback::Id id, const PackageSpec & reqpkg,
                       const zypp::PoolItem & selected = zypp::PoolItem(),
                       const zypp::PoolItem & installed = zypp::PoolItem() )
    { _feedback.emplace_back( id, reqpkg, selected, installed ); }

    void addFeedback( Feedback::Id id, const PackageSpec & reqpkg, std::string userdata )
    { _feedback.emplace_back( id, reqpkg, std::move(userdata) ); }

    const Options          _opts;
    Operation              _current = Operation::Install;
    std::vector<Selection> _selections;
    std::vector<Feedback>  _feedback;
    Effects                _effects;

    // Mirrors upstream's _toinst/_toremove/_requires/_conflicts sets,
    // populated alongside _selections at the same call sites (setToInstall
    // etc.) rather than derived by re-scanning _selections on every
    // toInstall()/dep_requires() call — same tradeoff upstream made.
    std::set<zypp::PoolItem>   _toinst;
    std::set<zypp::PoolItem>   _toremove;
    std::set<zypp::Capability> _requires;
    std::set<zypp::Capability> _conflicts;
};

} // namespace solverequest

#endif // MCP_SERVER_ZYPP_SOLVER_REQUESTER_H
