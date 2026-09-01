#ifndef MCP_SERVER_ZYPP_SOLVER_SELECTION_H
#define MCP_SERVER_ZYPP_SOLVER_SELECTION_H

#include <vector>

#include <zypp/Capability.h>
#include <zypp/PoolItem.h>

#include "packagespec.h"

namespace solverequest
{

/// Replaces zypper's ZypperCommand for the purpose of Requester's
/// internal dispatch — carries only the distinctions the selection
/// algorithm actually branches on, with no zypper CLI-command coupling.
enum class Operation
{
    Install,
    Remove,
    Update,
    UpdatePatterns,
    UpdatePatches,
};

/// One request submitted to a Requester session.
struct Job
{
    Operation      op = Operation::Install;
    PackageSpecSet dos;               ///< Install / Remove / Update
    PackageSpecSet donts;             ///< the "install can also remove" case
    bool           updateStackOnly = false; ///< UpdatePatches only
};

/// The terminal outcome of selecting one package: what should eventually
/// be done to the pool/resolver. Never applied by the selector itself —
/// see apply.h. The five Kind values are exactly the terminal actions
/// zypper's own Feedback enum already names (SET_TO_INSTALL,
/// SET_TO_REMOVE, ADDED_REQUIREMENT, ADDED_CONFLICT).
struct Selection
{
    enum class Kind
    {
        Nothing,       ///< no action; see the accompanying Feedback for why
        SetToInstall,
        SetToRemove,
        AddRequire,    ///< locks, by-cap fallback, best-effort updates
        AddConflict,
    };

    Kind             kind = Kind::Nothing;
    zypp::PoolItem   item;        ///< valid for SetToInstall / SetToRemove
    zypp::Capability capability;  ///< valid for AddRequire / AddConflict

    /// SetToInstall only. Upstream's setToInstall() has two genuinely
    /// different mutations depending on Options::force:
    ///   force  -> item.status().setToBeInstalled(ResStatus::USER)
    ///   !force -> ui::asSelectable(item)->setOnSystem(item, ResStatus::USER)
    /// setOnSystem() additionally arranges the candidate via setCandidate()
    /// and is not a drop-in replacement for the raw status mutation, so
    /// the distinction must survive into the applier rather than being
    /// collapsed.
    bool             forced = false;
};

/// Side effects of a submit() call that are not simple selections — today
/// just the one case upstream's updatePatches() had (a mutable write-back
/// to Zypper::instance().runtimeData().solve_with_update). The caller
/// decides what to do with it; Options itself stays immutable.
struct Effects
{
    bool droppedWithUpdate = false;
};

} // namespace solverequest

#endif // MCP_SERVER_ZYPP_SOLVER_SELECTION_H
