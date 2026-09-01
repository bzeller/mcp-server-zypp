#ifndef MCP_SERVER_ZYPP_SOLVER_APPLY_H
#define MCP_SERVER_ZYPP_SOLVER_APPLY_H

#include <vector>

#include <zypp/Resolver.h>

#include "selection.h"

namespace solverequest
{

/// Applies previously computed Selections to the pool/resolver. Contains
/// no policy whatsoever — every decision (whether to select an item
/// directly, force it, or fall back to a solver job because it is locked)
/// was already made by Requester::submit(); this function only replays
/// it. Split out specifically so lock handling (Selection::Kind::AddRequire/
/// AddConflict) cannot be bypassed by a caller that only wants to "apply
/// the selections" — there is no other way to mutate package status from
/// a Selection.
void applySelections( const zypp::Resolver_Ptr & resolver, const std::vector<Selection> & selections );

} // namespace solverequest

#endif // MCP_SERVER_ZYPP_SOLVER_APPLY_H
