#ifndef MCP_SERVER_ZYPP_TRANSACTION_H
#define MCP_SERVER_ZYPP_TRANSACTION_H

/** \file transaction.h
 *
 * Shared solver-setup and plan-emission helpers used by both the plan_*
 * and confirm_* tool pairs. Keeping the logic here avoids duplication and
 * guarantees that confirm re-runs exactly the same solver setup as plan.
 */

#include <optional>
#include <string>

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

#endif // MCP_SERVER_ZYPP_TRANSACTION_H
