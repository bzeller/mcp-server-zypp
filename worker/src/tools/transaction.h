#ifndef MCP_SERVER_ZYPP_TRANSACTION_H
#define MCP_SERVER_ZYPP_TRANSACTION_H

/** \file transaction.h
 *
 * Shared solver-setup and plan-emission helpers used by both the plan_*
 * and confirm_* tool pairs. Keeping the logic here avoids duplication and
 * guarantees that confirm re-runs exactly the same solver setup as plan.
 */

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <zypp/ZYpp.h>
#include <zypp/ResPool.h>
#include <zypp/ZYppCommitResult.h>
#include <zypp-core/parser/json/JsonValue.h>

#include "../commitfailurelog.h"

class McpTransport;

// ─── Shared schema property objects ──────────────────────────────────────────
// Returned by reference — static storage, no copies.

const zypp::json::Object & installSchemaProperties();
const zypp::json::Object & removeSchemaProperties();

// ─── Solver option accessors ──────────────────────────────────────────────────

/// Whether the caller passed solver_options.allow_downgrade.
///
/// Exists because a downgrade needs permission at *two* independent
/// layers and setupInstall() can only grant one of them: the resolver
/// (Resolver::setAllowDowngrade, applied inside setupInstall) decides
/// whether a downgrade may be *planned*, while ZYppCommitPolicy::
/// allowDowngrade decides whether rpm will *execute* it. Granting only
/// the first produces a plan that then fails at commit. confirm_install
/// reads this to set the second; sharing one accessor keeps the two
/// readings from drifting apart.
bool requestsAllowDowngrade( const zypp::json::Object & arg );

// ─── setupInstall ─────────────────────────────────────────────────────────────
// Parse install args, apply solver options, create the install job.
// Returns true on success.
// On failure: writes an error frame to t and returns false.

bool setupInstall( const zypp::json::Object & arg,
                   zypp::ZYpp::Ptr            zypp,
                   const std::string &        toolName,
                   McpTransport &             t );

// ─── setupRemove ─────────────────────────────────────────────────────────────
enum class SetupRemoveResult { Ok, NotInstalled, Error };

// Parse remove args, apply solver options, create the remove job.
// NotInstalled: writes empty-plan result frame, caller should return 0.
// Error:        writes error frame, caller should return 1.
// Ok:           solver job is set up, caller should resolve and commit/emit.

SetupRemoveResult setupRemove( const zypp::json::Object & arg,
                               zypp::ZYpp::Ptr            zypp,
                               const std::string &        toolName,
                               McpTransport &             t );

// ─── Plan serialisation ───────────────────────────────────────────────────────

zypp::json::Object planInstallToJson( const std::string &   toolName,
                                      const zypp::ResPool & pool );

zypp::json::Object planRemoveToJson( const std::string &   toolName,
                                     const zypp::ResPool & pool );

// ─── License confirmation ─────────────────────────────────────────────────────
// No libzypp callback exists for license acceptance (unlike KeyRingReport/
// DigestReport) — zypper itself handles this entirely at the caller level
// (zypper/src/misc.cc: confirm_licenses), before commit() is ever invoked.
// We follow the same shape, but adapted to our stateless request/response
// model: rather than blocking mid-call on an interactive elicitation,
// plan_install surfaces exactly what needs confirming (via
// planInstallToJson's "licenses" array, using the same grouping/ids as
// here), and the caller passes back accepted_licenses on confirm_install.

/// One group of packages sharing an identical license text requiring
/// confirmation (matches zypper's dedup-by-text behavior — multiple
/// packages, e.g. sub-packages of one product, often share one license).
struct LicenseGroup
{
    std::string              text;
    std::vector<std::string> packages;
};

/// Collect to-be-installed items with an unconfirmed license
/// (needToAcceptLicense() true, licenseToConfirm() non-empty), deduped by
/// license text. Map key is a stable, deterministic identifier for the
/// license text ("license_id") — stable across the plan_install call and
/// any later confirm_install call against the same pool state.
std::map<std::string, LicenseGroup> collectLicensesToConfirm( const zypp::ResPool & pool );

/// Verify every license collected by collectLicensesToConfirm is present
/// in acceptedLicenseIds. Returns true if nothing is outstanding. On
/// failure, writes a LICENSE_CONFIRMATION_REQUIRED error frame
/// re-surfacing the missing license(s) (license_id/text/packages) so the
/// caller can obtain confirmation and retry with accepted_licenses
/// populated — and returns false. Must be called after a successful
/// resolvePool() and before commit().
bool checkLicensesAccepted( const zypp::ResPool &          pool,
                           const std::set<std::string> & acceptedLicenseIds,
                           const std::string &            toolName,
                           McpTransport &                  t );

// ─── Commit failure reporting ───────────────────────────────────────────────
// ZYppCommitResult cannot say *why* a commit failed — sat::Transaction::Step
// carries identity and stage only, no message field at all (see
// sat/Transaction.h). The reason, if any was captured, lives in
// CommitFailureLog (see commitfailurelog.h), populated during commit by the
// callbacks in callbacks.cc. This builds the combined error frame from both.

/// Builds a COMMIT_FAILED/COMMIT_PREPARE_FAILED-shaped error frame:
///   - failed_installs/failed_removals/skipped_installs/skipped_removals,
///     bucketed from result's transaction step list exactly as zypper's
///     CommitSummary::collectData() does (TRANSACTION_IGNORE steps are
///     always omitted, matching zypper). Each entry is {name, edition,
///     arch} — taken from the Step's own accessors, never satSolvable():
///     @System solvables within the transaction are invalidated once the
///     rpm database is reread post-commit (documented at
///     sat/Transaction.h:209-215), which is a real, immediate risk for
///     TRANSACTION_ERASE steps in particular, not a theoretical one.
///   - details: every entry captured in failures, in arrival order, as
///     {package, phase, severity, text} — package is "" for
///     transaction-scoped entries (see CommitFailureDetail). This is the
///     *only* source of "why" available anywhere; the step list alone can
///     only say "which".
///   - truncated: true if failures hit its cap and the oldest entries
///     were dropped to keep the newest (see CommitFailureLog::kMaxEntries).
zypp::json::Object commitFailureToJson( const std::string &            toolName,
                                        const std::string &            code,
                                        const std::string &            detail,
                                        const zypp::ZYppCommitResult & result,
                                        const CommitFailureLog &       failures );

/// Warning/Error-severity entries only (Detail entries — routine rpm
/// output kept only as context for an actual failure — are never
/// included), as {package?, phase, severity, text}. Empty if there is
/// nothing to report.
///
/// For use on the *success* path: a commit can complete with every
/// transaction step DONE and still have had something non-fatal go
/// wrong — canonically a %posttrans scriptlet failing (mirrors zypper's
/// ZYPPER_EXIT_INF_RPM_SCRIPT_FAILED, exit code 107, in the informational
/// band: the operation still succeeded). !result.allDone() never fires in
/// that case, so without this the information would be silently dropped
/// even though CommitFailureLog was already tracking it during commit.
zypp::json::Array commitIssuesToJson( const CommitFailureLog & failures );

#endif // MCP_SERVER_ZYPP_TRANSACTION_H
