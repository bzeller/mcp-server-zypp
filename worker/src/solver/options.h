#ifndef MCP_SERVER_ZYPP_SOLVER_OPTIONS_H
#define MCP_SERVER_ZYPP_SOLVER_OPTIONS_H

// Ported from zypper's SolverRequester::Options, de-globalized: every
// field that used to be seeded from a zypper singleton (SolverSettings,
// ZConfig, Zypper::config(), LicenseAgreementPolicy) is now a plain
// member the caller sets explicitly. A caller that wants the same
// defaults zypper uses is responsible for reading ZConfig itself before
// constructing this — Options performs no ambient reads of its own.

#include <list>
#include <string>

#include "patchfilter.h"

namespace solverequest
{

/// Policy for one Requester session (one or more Job submissions). Held
/// by value inside Requester, immutable for the session's lifetime.
struct Options
{
    /// If true, force the operation in some defined cases even if it
    /// would otherwise not be allowed (see updateTo()/setToInstall() for
    /// the exact cases).
    bool force = false;

    /// If true, an explicit version downgrade upon install is allowed.
    /// This is the RESOLVER-level permission
    /// (Resolver::setAllowDowngrade's counterpart); it is deliberately
    /// distinct from allowDowngrade below, which is the separate
    /// COMMIT-level permission (ZYppCommitPolicy::allowDowngrade()).
    /// libzypp requires both to actually perform a downgrade — see
    /// REVIEW.md F06.
    bool oldpackage = false;

    /// Commit-level downgrade permission — see oldpackage above.
    bool allowDowngrade = false;

    /// Package-selection strategy. These two flags are overrides on a
    /// THREE-state space, not a boolean pair:
    ///
    ///   forceByCap  forceByName   behaviour
    ///   ----------  -----------   ------------------------------------
    ///   false       false         try by name, fall back to capability
    ///   true        false         capability only
    ///   false       true          name only, no fallback
    ///
    /// Both-false is the default and is a meaningful state, not an
    /// "unset" one. The remaining combination (both true) is
    /// contradictory and unrepresentable by design: the setters below
    /// clear the sibling when one is turned on, not when turned off.
    bool forceByCap()  const { return _forceByCap; }
    bool forceByName() const { return _forceByName; }

    /// Turn capability-only selection on or off. Turning it ON clears
    /// forceByName (logging a DBG line if that changed anything);
    /// turning it OFF leaves forceByName alone, so the default
    /// name-then-capability state stays reachable.
    void setForceByCap( bool value = true );

    /// See setForceByCap(); same asymmetry, same reasoning.
    void setForceByName( bool value = true );

    /// Whether to request updates via Selectable::updateCandidateObj()
    /// or via addRequire(higher-than-installed).
    bool bestEffort = false;

    /// Whether to skip installs/updates that need user interaction.
    bool skipInteractive = false;

    /// Whether to skip optional patches.
    bool skipOptionalPatches = false;

    /// Whether to ignore vendor when selecting packages. Was seeded from
    /// SolverSettings::instance()/ZConfig::instance() upstream; the
    /// caller now sets this explicitly.
    bool allowVendorChange = false;

    /// Was Zypper::instance().config().reboot_req_non_interactive.
    bool rebootReqNonInteractive = false;

    /// Was LicenseAgreementPolicy::instance()._autoAgreeWithLicenses.
    bool autoAgreeWithLicenses = false;

    /// Was the mutable Zypper::instance().runtimeData().solve_with_update.
    /// Read by updatePatches(); if it ends up being dropped because a
    /// pkgmgmt-affecting patch is being installed first, that is reported
    /// via Outcome::Effects::droppedWithUpdate rather than written back
    /// here — Options stays immutable for the whole session.
    bool withUpdate = false;

    /// Aliases of the repos from which packages should be installed.
    std::list<std::string> fromRepos;

    /// Patch-specific date/category/severity filter.
    PatchFilter patchFilter;

private:
    bool _forceByCap  = false;
    bool _forceByName = false;
};

} // namespace solverequest

#endif // MCP_SERVER_ZYPP_SOLVER_OPTIONS_H
