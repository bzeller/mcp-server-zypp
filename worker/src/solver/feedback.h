#ifndef MCP_SERVER_ZYPP_SOLVER_FEEDBACK_H
#define MCP_SERVER_ZYPP_SOLVER_FEEDBACK_H

// Ported from zypper's SolverRequester::Feedback. The enum and the data
// carried per entry are preserved verbatim (plus three additions at the
// end, never renumbering existing entries — see the trailing comment).
// Feedback::print(Out&, Options&) is zypper's own presentation layer and
// is deliberately NOT ported: it is replaced by a JSON conversion living
// outside this namespace entirely.

#include <string>

#include <zypp/PoolItem.h>

#include "packagespec.h"

namespace solverequest
{

/// One piece of feedback from a Requester run. Pure data — no
/// presentation logic, no I/O. See tools/feedbackjson.h for the MCP-side
/// rendering of these into JSON.
class Feedback
{
public:
    enum Id
    {
        // Given combination of arguments and options makes no sense, or the
        // functionality is not defined/implemented.
        INVALID_REQUEST,

        // The search for the string in object names failed, but will try caps.
        NOT_FOUND_NAME_TRYING_CAPS,
        NOT_FOUND_NAME,
        NOT_FOUND_CAP,

        // Removal or update was requested, but there's no installed item.
        NOT_INSTALLED,

        // Removal by capability requested, but no provider is installed.
        NO_INSTALLED_PROVIDER,

        // Selected object is already installed.
        ALREADY_INSTALLED,
        NO_UPD_CANDIDATE,
        UPD_CANDIDATE_CHANGES_VENDOR,
        UPD_CANDIDATE_HAS_LOWER_PRIO,
        UPD_CANDIDATE_IS_LOCKED,

        // The installed package is no longer available in repositories.
        // => can't reinstall, can't update/downgrade..
        NOT_IN_REPOS,

        // Selected object is not the highest available, because of caller
        // restrictions like repo(s), version, architecture.
        UPD_CANDIDATE_USER_RESTRICTED,
        INSTALLED_LOCKED,

        // Selected object is older than the installed. Won't allow downgrade
        // unless Options::oldpackage is set.
        SELECTED_IS_OLDER,

        // ── patch/pattern/product specialties ──────────────────────────────

        // !patch.status().isRelevant()
        PATCH_NOT_NEEDED,

        // Skipping a patch because it is marked interactive or has a license
        // to confirm and Options::skipInteractive is requested.
        PATCH_INTERACTIVE_SKIPPED,

        // Patch is optional and Options::skipOptionalPatches is requested.
        PATCH_OPTIONAL,

        // Patch was requested but is locked (set to ignore). Can be forced
        // via Options::force.
        PATCH_UNWANTED,

        // Patch is not in the specified category / severity.
        PATCH_WRONG_CAT,
        PATCH_WRONG_SEV,

        // Patch is too new and a date limit was specified.
        PATCH_TOO_NEW,

        // ── resulting solver/selection actions ─────────────────────────────

        SET_TO_INSTALL,
        FORCED_INSTALL,
        SET_TO_REMOVE,
        ADDED_REQUIREMENT,
        ADDED_CONFLICT,

        // ── additions made during the mcp-server-zypp port ───────────────────
        // Never insert new entries above this line — always append, so
        // integer values already stored/compared anywhere remain stable.

        // A patch category/severity filter value did not match any known
        // Patch::categoryEnum()/severityFlag() — was a zypper.out().warning()
        // in CliMatchPatch's constructor.
        SUSPICIOUS_CATEGORY_FILTER,
        SUSPICIOUS_SEVERITY_FILTER,

        // Options::withUpdate was dropped because a patch affecting package
        // management is being installed first — was a
        // Zypper::instance().out().info() call in updatePatches().
        DROPPED_WITH_UPDATE,
    };

    Feedback( Id id,
              PackageSpec reqpkg,
              zypp::PoolItem selected = zypp::PoolItem(),
              zypp::PoolItem installed = zypp::PoolItem() );

    Feedback( Id id, PackageSpec reqpkg, std::string userdata );

    Id id() const { return _id; }
    const PackageSpec & reqpkg() const { return _reqpkg; }
    const zypp::PoolItem & selectedObj()  const { return _objsel; }
    const zypp::PoolItem & installedObj() const { return _objinst; }
    const std::string & userdata() const { return _userdata; }

private:
    Id _id;
    PackageSpec _reqpkg;
    zypp::PoolItem _objsel;
    zypp::PoolItem _objinst;
    std::string _userdata;
};

} // namespace solverequest

#endif // MCP_SERVER_ZYPP_SOLVER_FEEDBACK_H
