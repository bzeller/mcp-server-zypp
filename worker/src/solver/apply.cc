#include "apply.h"

#include <zypp-core/base/Logger.h>
#include <zypp/ui/Selectable.h>

#undef  ZYPP_BASE_LOGGER_LOGGROUP
#define ZYPP_BASE_LOGGER_LOGGROUP "solverequest"

namespace solverequest
{

void applySelections( const zypp::Resolver_Ptr & resolver, const std::vector<Selection> & selections )
{
    using namespace zypp;

    for ( const auto & sel : selections )
    {
        switch ( sel.kind )
        {
            case Selection::Kind::Nothing:
                break;

            case Selection::Kind::SetToInstall:
                // Two genuinely different mutations depending on how the
                // selection was forced — see selection.h: Selection::forced.
                if ( sel.forced )
                {
                    sel.item.status().setToBeInstalled( ResStatus::USER );
                }
                // setOnSystem() can in principle fail (returns false), but
                // every way it can is already excluded before we get here:
                // the item is by definition in its own selectable's
                // available list (asSelectable() derives the selectable
                // from the item); ResStatus::USER is the maximum
                // TransactByValue, so it can never be outranked by an
                // existing transacting candidate's causer; and a locked
                // selectable never reaches this branch at all, because
                // Requester::setToInstall() routes those to
                // Kind::AddRequire instead. Upstream ignores the return
                // value for exactly these reasons. Log rather than plumb
                // it: there is no sensible caller-facing error for a
                // condition that cannot be triggered, but if any of the
                // three assumptions above ever stops holding, this makes
                // it visible instead of silently reporting success.
                else if ( !ui::asSelectable()( sel.item )->setOnSystem( sel.item, ResStatus::USER ) )
                {
                    WAR << "setOnSystem failed for " << sel.item << " - selection not applied" << std::endl;
                }
                break;

            case Selection::Kind::SetToRemove:
                sel.item.status().setToBeUninstalled( ResStatus::USER );
                break;

            case Selection::Kind::AddRequire:
                resolver->addRequire( sel.capability );
                break;

            case Selection::Kind::AddConflict:
                resolver->addConflict( sel.capability );
                break;
        }
    }
}

} // namespace solverequest
