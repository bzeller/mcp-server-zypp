#ifndef MCP_SERVER_ZYPP_SOLVER_PATCHFILTER_H
#define MCP_SERVER_ZYPP_SOLVER_PATCHFILTER_H

// Ported from zypper's CliMatchPatch, renamed PatchFilter. The zypper
// coupling was exactly two zypper.out().warning() calls in the
// constructor, reporting a category/severity value that doesn't match any
// known Patch::categoryEnum()/severityFlag(). Those become Feedback::Id
// entries collected in `warnings()` instead, which the caller (Requester)
// drains into its own feedback vector — the class itself produces no
// caller-facing output.
//
// _dateBefore was accessed directly by SolverRequester::updatePatches()
// via `friend class SolverRequester;`; that becomes the dateBefore()
// accessor below instead.

#include <set>
#include <string>
#include <vector>

#include <zypp-core/Date.h>
#include <zypp/Patch.h>
#include <zypp/PoolItem.h>

#include "feedback.h"

namespace solverequest
{

/// Functor testing whether a Patch matches date/category/severity
/// filters (non-patches always pass).
struct PatchFilter
{
    /// Default: no filter, everything passes.
    PatchFilter() = default;

    PatchFilter( const std::vector<zypp::Date> & dates_r,
                 std::set<std::string> categories_r,
                 std::set<std::string> severities_r )
    {
        for ( const auto & val : dates_r )
        {
            if ( val && ( !_dateBefore || val < _dateBefore ) )
                _dateBefore = val;
        }
        _categories = std::move( categories_r );
        for ( const std::string & cat : _categories )
        {
            if ( zypp::Patch::categoryEnum( cat ) == zypp::Patch::CAT_OTHER )
                _warnings.push_back( Feedback::SUSPICIOUS_CATEGORY_FILTER );
        }
        _severities = std::move( severities_r );
        for ( const std::string & sev : _severities )
        {
            if ( zypp::Patch::severityFlag( sev ) == zypp::Patch::SEV_OTHER )
                _warnings.push_back( Feedback::SUSPICIOUS_SEVERITY_FILTER );
        }
    }

    enum class Missmatch
    {
        None     = 0,
        Date     = 1<<0,
        Category = 1<<1,
        Severity = 1<<2,
    };

    Missmatch missmatch( const zypp::Patch::constPtr & patch_r ) const
    {
        if ( patch_r )  // non-patches pass
        {
            if ( _dateBefore && patch_r->timestamp() > _dateBefore )
                return Missmatch::Date;
            if ( ! ( _categories.empty() || patch_r->isCategory( _categories ) ) )
                return Missmatch::Category;
            if ( ! ( _severities.empty() || patch_r->isSeverity( _severities ) ) )
                return Missmatch::Severity;
        }
        return Missmatch::None;
    }

    bool operator()( const zypp::Patch::constPtr & patch_r ) const
    { return missmatch( patch_r ) == Missmatch::None; }

    bool operator()( const zypp::PoolItem & pi_r ) const
    { return not pi_r.isKind<zypp::Patch>() || operator()( zypp::asKind<zypp::Patch>(pi_r) ); }

    const zypp::Date & dateBefore() const { return _dateBefore; }

    /// Constructor-time diagnostics (suspicious filter values). The caller
    /// is responsible for turning these into real Feedback entries with
    /// the right PackageSpec context — this class has none to offer.
    const std::vector<Feedback::Id> & warnings() const { return _warnings; }

private:
    zypp::Date _dateBefore;
    std::set<std::string> _categories;
    std::set<std::string> _severities;
    std::vector<Feedback::Id> _warnings;
};

} // namespace solverequest

#endif // MCP_SERVER_ZYPP_SOLVER_PATCHFILTER_H
