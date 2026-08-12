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
#include <zypp-core/parser/json/JsonValue.h>

class McpTransport;

// ─── Shared schema property objects ──────────────────────────────────────────
// Returned by reference — static storage, no copies.

const zypp::json::Object & installSchemaProperties();
const zypp::json::Object & removeSchemaProperties();

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

#endif // MCP_SERVER_ZYPP_TRANSACTION_H
