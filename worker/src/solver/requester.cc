#include "requester.h"

#include <zypp-core/base/Easy.h>
#include <zypp-core/base/LogTools.h>
#include <zypp-core/base/String.h>

#include <zypp/Capability.h>
#include <zypp/PoolItemBest.h>
#include <zypp/PoolQuery.h>
#include <zypp/Patch.h>
#include <zypp/ResKind.h>
#include <zypp/ResTraits.h>
#include <zypp/VendorAttr.h>
#include <zypp/sat/Solvable.h>
#include <zypp/sat/SolvAttr.h>
#include <zypp/sat/WhatProvides.h>
#include <zypp/ui/Selectable.h>

// libzypp logger settings. This trace is preserved verbatim from
// upstream's SolverRequester.cc (same severities, same message text at
// every site) — it is the selection algorithm's own reasoning trail and
// is not zypper-owned output; see feedback.h's header comment for the
// distinction between this and the removed zypper.out() calls.
#undef  ZYPP_BASE_LOGGER_LOGGROUP
#define ZYPP_BASE_LOGGER_LOGGROUP "solverequest"

using namespace zypp;

namespace solverequest
{

// ─── file-local helpers, ported verbatim ──────────────────────────────────────
namespace
{
    void getCiMatchHint( PoolQuery & q_r, std::string & ciMatchHint_r )
    {
        q_r.setCaseSensitive( false );
        if ( ! q_r.empty() )
        {
            unsigned cnt = 0;
            for_( it, q_r.selectableBegin(), q_r.selectableEnd() )
            {
                if ( cnt == 3 )
                {
                    ciMatchHint_r += ",...";
                    break;
                }
                else
                {
                    if ( cnt )
                        ciMatchHint_r += ", ";
                    ciMatchHint_r += (*it)->name();
                }
                ++cnt;
            }
        }
    }

    PoolQuery pkg_spec_to_poolquery( const Capability & cap, const std::list<std::string> & repos )
    {
        sat::Solvable::SplitIdent splid( cap.detail().name() );

        PoolQuery q;
        q.setMatchGlob();
        q.setCaseSensitive( true );
        q.addKind( splid.kind() );
        for_( it, repos.begin(), repos.end() )
            q.addRepo( *it );
        q.addDependency( sat::SolvAttr::name, splid.name().asString(),
                         // only package names (no provides)
                         cap.detail().op(), cap.detail().ed(),
                         // defaults to Rel::ANY (NOOP) if no versioned cap
                         Arch( cap.detail().arch() ) );
        // defaults Arch_empty (NOOP) if no arch in cap

        DBG << "query: " << q << std::endl;
        return q;
    }

    PoolQuery pkg_spec_to_poolquery( const Capability & cap, const std::string & repo )
    {
        std::list<std::string> repos;
        if ( !repo.empty() )
            repos.push_back( repo );
        return pkg_spec_to_poolquery( cap, repos );
    }

    std::set<PoolItem> get_installed_providers( const Capability & cap )
    {
        std::set<PoolItem> providers;

        sat::WhatProvides q( cap );
        for_( it, q.poolItemBegin(), q.poolItemEnd() )
        {
            if ( traits::isPseudoInstalled( (*it).satSolvable().kind() ) )
            {
                if ( (*it).isSatisfied() )
                    providers.insert( *it );
            }
            else if ( (*it).satSolvable().isSystem() )
                providers.insert( *it );
        }
        return providers;
    }

    PoolItem get_installed_obj( ui::Selectable::Ptr & s )
    {
        PoolItem installed;
        if ( traits::isPseudoInstalled( s->kind() ) )
        {
            for_( it, s->availableBegin(), s->availableEnd() )
                // this is OK also for patches - isSatisfied() excludes !isRelevant()
                if ( it->status().isSatisfied() && ( !installed || installed->edition() < (*it)->edition() ) )
                    installed = *it;
        }
        else
            installed = s->installedObj();

        return installed;
    }

} // namespace

// ─── Requester ────────────────────────────────────────────────────────────────

void Requester::submit( const ResPool & pool, const Job & job )
{
    // Set BEFORE dispatch, deliberately: upstream's _command is written in
    // its public entry points and read deep inside private helpers
    // (install()/updateTo()/installPatch()); a naive port that dispatched
    // first and set _current afterward would compile cleanly while
    // silently branching on a stale value.
    _current = job.op;

    switch ( job.op )
    {
        case Operation::Install:
        case Operation::Remove:
            // Note: upstream's remove(const PackageArgs&) additionally
            // asserted args.options().do_by_default == false, logging an
            // INT and refusing to proceed otherwise — that guarded against
            // a caller misusing PackageArgs' CLI-parsing "do_by_default"
            // flag for the wrong command. A Job's dos/donts are already
            // fully resolved by the time submit() sees them, so there is
            // no equivalent ambiguity here to guard against; the check is
            // intentionally not ported.
            installRemove( pool, job.dos, job.donts );
            break;

        case Operation::Update:
            // update(PackageArgs) upstream only ever processed dos() via
            // install(pkg) — it never called remove() for donts(). Preserve
            // that asymmetry exactly rather than routing through the
            // shared installRemove() helper.
            for ( const auto & pkg : job.dos )
                install( pool, pkg );
            break;

        case Operation::UpdatePatterns:
            updatePatterns();
            break;

        case Operation::UpdatePatches:
            updatePatches( pool, job.updateStackOnly );
            break;
    }
}

// ----------------------------------------------------------------------------

void Requester::installRemove( const ResPool & pool, const PackageSpecSet & dos, const PackageSpecSet & donts )
{
    if ( dos.empty() && donts.empty() )
        return;

    for_( it, dos.begin(), dos.end() )
        install( pool, *it );

    // TODO solve before processing donts? so that we could unset any
    // donts that are already set for installation. This would allow
    //   install pattern:lamp_server -someunwantedpackage
    // and similar nice things.
    for_( it, donts.begin(), donts.end() )
        remove( pool, *it );
}

// ----------------------------------------------------------------------------

/*
 * For given PackageSpec & Options:
 *
 * 1) if forceByCap is not set, try to install 'by name' first, i.e. via
 *    ui::Selectable and/or PoolItem. Wildcards are supported (PoolQuery
 *    with match_glob).
 *
 * 2) if no package could be found by name and forceByName was not set, or
 *    forceByCap was set, install 'by capability', i.e. using
 *    Resolver::addRequire(cap) — expressed here as Selection::AddRequire.
 *
 * NOTES
 * - In both cases check for already installed packages, and if found,
 *   hand over to updateTo().
 * - If the current Operation is Update and the object is not installed,
 *   do no action other than report the fact. This is the only difference
 *   between Install and Update.
 * - If the spec contains a repo, issue the request restricted to it.
 */
void Requester::install( const ResPool & pool, const PackageSpec & pkg )
{
    std::string ciMatchHint; // hint on possible typo (case-insensitive matches)

    // first try by name
    if ( !_opts.forceByCap() )
    {
        PoolQuery q;
        if ( !pkg.repo_alias.empty() )
            q = pkg_spec_to_poolquery( pkg.parsed_cap, pkg.repo_alias );
        else
            q = pkg_spec_to_poolquery( pkg.parsed_cap, _opts.fromRepos );

        // get the best matching items and tag them for installation.
        // FIXME this ignores vendor lock - we need some way to do --from
        // which would respect vendor lock: e.g. a new
        // Selectable::updateCandidateObj(Options&). (upstream FIXME,
        // preserved — not fixed by this port.)
        PoolItemBest bestMatches( q.begin(), q.end(), PoolItemBest::preferNotLocked );

        if ( !bestMatches.empty() )
        {
            unsigned notInstalled = 0;
            for_( sit, bestMatches.begin(), bestMatches.end() )
            {
                ui::Selectable::Ptr s( ui::asSelectable()( *sit ) );
                if ( s->kind() == ResKind::patch )
                    installPatch( pkg, *sit );
                else
                {
                    PoolItem instobj = get_installed_obj( s );
                    if ( instobj )
                    {
                        if ( s->availableEmpty() )
                        {
                            if ( !_opts.force )
                                addFeedback( Feedback::ALREADY_INSTALLED, pkg, instobj, instobj );
                            addFeedback( Feedback::NOT_IN_REPOS, pkg, instobj, instobj );
                            MIL << s->name() << " not in repos, can't (re)install" << std::endl;
                            return;
                        }

                        // whether caller requested specific repo/version/arch
                        bool userconstraints = pkg.parsed_cap.detail().isVersioned()
                                            || pkg.parsed_cap.detail().hasArch()
                                            || !_opts.fromRepos.empty()
                                            || !pkg.repo_alias.empty();

                        // check vendor (since PoolItemBest does not do it)
                        bool changes_vendor = ! VendorAttr::instance().equivalent( instobj->vendor(), (*sit)->vendor() );

                        PoolItem best { s->updateCandidateObj() };
                        if ( best && best.status().isLocked()
                          && !(*sit).status().isLocked()
                          && (*sit).edition() > instobj.edition() )
                        {
                            // This is a partially locked item. Try the best unlocked version.
                            best = PoolItem();
                        }

                        if ( userconstraints )
                            updateTo( pool, pkg, *sit );
                        else if ( _opts.force )
                            updateTo( pool, pkg, s->highestAvailableVersionObj() );
                        else if ( best )
                            updateTo( pool, pkg, best );
                        else if ( changes_vendor && !_opts.allowVendorChange )
                            updateTo( pool, pkg, instobj );
                        else
                            updateTo( pool, pkg, *sit );
                    }
                    else if ( _current == Operation::Install )
                    {
                        setToInstall( *sit );
                        MIL << "installing " << *sit << std::endl;
                    }
                    else
                    {
                        ++notInstalled;
                        // delay Feedback::NOT_INSTALLED until we know there
                        // is not a single match installed.
                    }
                }
            }
            if ( notInstalled == bestMatches.size() )
                addFeedback( Feedback::NOT_INSTALLED, pkg );
            return;
        }
        else if ( _opts.forceByName() || pkg.modified )
        {
            addFeedback( Feedback::NOT_FOUND_NAME, pkg );
            WAR << pkg << " not found" << std::endl;
            return;
        }

        addFeedback( Feedback::NOT_FOUND_NAME_TRYING_CAPS, pkg );
        // Quick check whether there would have been matches with different case.
        getCiMatchHint( q, ciMatchHint );
    }

    // try by capability
    sat::WhatProvides q( pkg.parsed_cap );
    if ( q.empty() )
    {
        addFeedback( Feedback::NOT_FOUND_CAP, pkg, ciMatchHint );
        WAR << pkg << " not found" << std::endl;
        return;
    }

    // is the provider already installed?
    std::set<PoolItem> providers = get_installed_providers( pkg.parsed_cap );
    for_( it, providers.begin(), providers.end() )
    {
        if ( _current == Operation::Install )
            addFeedback( Feedback::ALREADY_INSTALLED, pkg, *it, *it );
        MIL << "provider '" << *it << "' of '" << pkg.parsed_cap << "' installed" << std::endl;
    }

    if ( providers.empty() )
    {
        DBG << "adding requirement " << pkg.parsed_cap << std::endl;
        addRequirement( pkg );
    }
}

// ----------------------------------------------------------------------------

void Requester::remove( const ResPool & pool, const PackageSpec & pkg )
{
    std::string ciMatchHint;

    if ( !_opts.forceByCap() )
    {
        PoolQuery q = pkg_spec_to_poolquery( pkg.parsed_cap, "" );

        if ( !q.empty() )
        {
            bool got_installed = false;
            for_( it, q.poolItemBegin(), q.poolItemEnd() )
            {
                if ( it->status().isInstalled() )
                {
                    DBG << "Marking for deletion: " << *it << std::endl;
                    setToRemove( *it );
                    got_installed = true;
                }
            }
            if ( got_installed )
                return;
            else
            {
                addFeedback( Feedback::NOT_INSTALLED, pkg );
                MIL << "'" << pkg.parsed_cap << "' is not installed" << std::endl;
                if ( _opts.forceByName() )
                    return;
            }
            // TODO handle patches (cannot uninstall!), patterns (remove content(?))
        }
        else if ( _opts.forceByName() || pkg.modified )
        {
            addFeedback( Feedback::NOT_FOUND_NAME, pkg );
            WAR << pkg << "' not found" << std::endl;
            return;
        }

        addFeedback( Feedback::NOT_FOUND_NAME_TRYING_CAPS, pkg );
        getCiMatchHint( q, ciMatchHint );
    }

    sat::WhatProvides q( pkg.parsed_cap );
    if ( q.empty() )
    {
        addFeedback( Feedback::NOT_FOUND_CAP, pkg, ciMatchHint );
        WAR << pkg << " not found" << std::endl;
        return;
    }

    std::set<PoolItem> providers = get_installed_providers( pkg.parsed_cap );

    if ( providers.empty() )
    {
        addFeedback( Feedback::NO_INSTALLED_PROVIDER, pkg );
        MIL << "no provider of " << pkg.parsed_cap << "is installed" << std::endl;
    }
    else
    {
        MIL << "adding conflict " << pkg.parsed_cap << std::endl;
        addConflict( pkg );
    }
}

// ----------------------------------------------------------------------------

void Requester::updatePatterns()
{ /*NOOP*/ }

// ----------------------------------------------------------------------------

void Requester::updatePatches( const ResPool & pool, bool updateStackOnly )
{
    DBG << "going to mark needed patches for installation" << std::endl;

    // search twice: if there are none with restartSuggested(), retry on all
    // unless updateStackOnly.
    // (in the first run, ignore_pkgmgmt == 0, in the second it is 1)
    bool any_marked = false;
    bool dateLimit = ( _opts.patchFilter.dateBefore() != Date() );
    for ( unsigned ignore_pkgmgmt = 0; !any_marked && ignore_pkgmgmt < 2; ++ignore_pkgmgmt )
    {
        for ( const auto & selPtr : pool.proxy().byKind( ResKind::patch ) )
        {
            PackageSpec patch;
            patch.orig_str = selPtr->name();
            patch.parsed_cap = Capability( selPtr->name() );

            // bnc#919709: a date limit must ignore newer patch candidates
            PoolItem candidateObj( selPtr->candidateObj() );
            if ( dateLimit && asKind<Patch>(candidateObj)->timestamp() > _opts.patchFilter.dateBefore() )
            {
                for ( const auto & pi : selPtr->available() )
                {
                    if ( asKind<Patch>(pi)->timestamp() <= _opts.patchFilter.dateBefore() )
                    {
                        candidateObj = pi;
                        break;
                    }
                }
            }

            if ( installPatch( patch, candidateObj, ignore_pkgmgmt ) )
                any_marked = true;
        }

        if ( ! ignore_pkgmgmt ) // just checked the update stack
        {
            if ( any_marked )
            {
                MIL << "got some pkgmgmt patches, will install these first" << std::endl;

                // When (auto)restricting patch to the updatestack only, drop
                // the caller's requested "with update" option.
                if ( _opts.withUpdate )
                {
                    WAR << "Drop with-update while patching the update stack" << std::endl;
                    _effects.droppedWithUpdate = true;
                    addFeedback( Feedback::DROPPED_WITH_UPDATE, PackageSpec() );
                }
            }

            if ( updateStackOnly )
            {
                MIL << "updatestack-only: will stop here!" << std::endl;
                break;
            }
        }
    }
}

// ----------------------------------------------------------------------------

bool Requester::installPatch( const PoolItem & selected )
{
    PackageSpec patchspec;
    patchspec.orig_str = str::form( "%s-%s", selected->name().c_str(), selected->edition().asString().c_str() );
    patchspec.parsed_cap = Capability( selected->name(), Rel::EQ, selected->edition(), ResKind::patch );

    return installPatch( patchspec, selected );
}

// ----------------------------------------------------------------------------

bool Requester::installPatch( const PackageSpec & patchspec, const PoolItem & selected, bool ignore_pkgmgmt )
{
    Patch::constPtr patch = asKind<Patch>(selected);

    if ( selected.status().isBroken() ) // bnc #506860
    {
        DBG << "Needed " << patch
            << " [" << unsigned(patch->interactiveFlags()) << "]"
            << " affects_pkgmgmt: " << patch->restartSuggested()
            << (ignore_pkgmgmt ? " (ignored)" : "") << std::endl;

        if ( ignore_pkgmgmt || patch->restartSuggested() )
        {
            Patch::InteractiveFlags ignoreFlags = Patch::NoFlags;
            if ( _opts.rebootReqNonInteractive )
                ignoreFlags |= Patch::Reboot;
            if ( _opts.autoAgreeWithLicenses )
                ignoreFlags |= Patch::License;

            if ( selected.isUnwanted() )
            {
                DBG << "candidate patch " << patch << " is locked" << std::endl;
                addFeedback( Feedback::PATCH_UNWANTED, patchspec, selected, selected );
                return false;
            }

            if ( _opts.skipOptionalPatches && patch->categoryEnum() == Patch::CAT_OPTIONAL )
            {
                DBG << "candidate patch " << patch << " is optional" << std::endl;
                addFeedback( Feedback::PATCH_OPTIONAL, patchspec, selected, selected );
                return false;
            }

            // bnc #221476
            if ( _opts.skipInteractive && patch->interactiveWhenIgnoring( ignoreFlags ) )
            {
                DBG << "candidate patch " << patch << " is too interactive [ignoring " << unsigned(ignoreFlags) << "]" << std::endl;
                addFeedback( Feedback::PATCH_INTERACTIVE_SKIPPED, patchspec, selected );
                return false;
            }

            {
                PatchFilter::Missmatch missmatch = _opts.patchFilter.missmatch( patch );
                if ( missmatch != PatchFilter::Missmatch::None )
                {
                    Feedback::Id id = Feedback::INVALID_REQUEST;
                    switch ( missmatch )
                    {
                        case PatchFilter::Missmatch::Date:     id = Feedback::PATCH_TOO_NEW;  break;
                        case PatchFilter::Missmatch::Category: id = Feedback::PATCH_WRONG_CAT; break;
                        case PatchFilter::Missmatch::Severity: id = Feedback::PATCH_WRONG_SEV; break;
                        case PatchFilter::Missmatch::None:     /* make gcc happy */ break;
                    }
                    DBG << "candidate patch " << patch << " does not pass filter (" << static_cast<unsigned>(missmatch) << ")" << std::endl;
                    addFeedback( id, patchspec, selected, selected );
                    return false;
                }
            }

            // passed:
            // TODO use _opts.force
            setToInstall( selected );
            MIL << "installing " << selected << std::endl;
            return true;
        }
    }
    else if ( selected.status().isSatisfied() )
    {
        if ( _current == Operation::Install || _current == Operation::Update )
        {
            DBG << "candidate patch " << patch << " is already satisfied" << std::endl;
            addFeedback( Feedback::ALREADY_INSTALLED, patchspec, selected, selected );
        }
    }
    else
    {
        if ( _current == Operation::Install || _current == Operation::Update )
        {
            DBG << "candidate patch " << patch << " is irrelevant" << std::endl;
            addFeedback( Feedback::PATCH_NOT_NEEDED, patchspec, selected );
        }
    }

    return false;
}

// ----------------------------------------------------------------------------

void Requester::updateTo( const ResPool & pool, const PackageSpec & pkg, const PoolItem & selected )
{
    if ( !selected )
    {
        INT << "Candidate is empty, returning! Pass PoolItem you want to update to." << std::endl;
        return;
    }

    ui::Selectable::Ptr s = ui::asSelectable()( selected );

    // the best object without repository, arch, or version restriction
    PoolItem theone = s->updateCandidateObj();
    // the best installed object
    PoolItem installed = get_installed_obj( s );
    // highest available version
    PoolItem highest = s->highestAvailableVersionObj();

    if ( !installed )
    {
        INT << "no installed object, nothing to update, returning" << std::endl;
        return;
        // TODO handle pseudoinstalled objects
    }

    DBG << "selected:  " << selected  << std::endl;
    DBG << "best:      " << theone    << std::endl;
    DBG << "highest:   " << highest   << std::endl;
    DBG << "installed: " << installed << std::endl;


    // ******* request ********
    bool action = true;
    if ( !identical( installed, selected ) || _opts.force )
    {
        if ( _opts.bestEffort )
        {
            // require version greater than the one installed
            Capability c( s->name(), Rel::GT, installed->edition(), s->kind() );
            PackageSpec req;
            req.orig_str = s->name();
            req.parsed_cap = c;
            addRequirement( req );
            MIL << *s << " update: adding requirement " << c << std::endl;
        }
        else if ( selected->edition() > installed->edition() )
        {
            setToInstall( selected );
            MIL << *s << " update: setting " << selected << " to install" << std::endl;
        }
        else if ( selected->edition() == installed->edition()
                && selected->arch() != installed->arch()
                && pkg.parsed_cap.detail().hasArch() /*caller-selected architecture*/ )
        {
            setToInstall( selected );
            MIL << *s << " update: setting " << selected << " to install (arch change request)" << std::endl;
        }
        else if ( selected->edition() == installed->edition()
                && !pkg.repo_alias.empty() /*caller-selected repo*/ )
        {
            setToInstall( selected );
            MIL << *s << " update: setting " << selected << " to install (repo change request)" << std::endl;
        }
        else if ( _opts.force || _opts.oldpackage )
        {
            setToInstall( selected );
            MIL << *s << " update: forced setting " << selected << " to install" << std::endl;
        }
        else
            action = false;
    }
    else
        action = false;


    // ******* report ********

    // the candidate is already installed
    if ( identical( installed, selected ) || ( !action && installed->edition() == selected->edition() ) )
    {
        if ( _opts.force )
            return;

        // only say 'already installed' in case of install, if update was
        // requested only report if we fail to install the newest version
        // (the code below)
        if ( _current == Operation::Install )
        {
            addFeedback( Feedback::ALREADY_INSTALLED, pkg, selected, installed );
            MIL << "'" << pkg.parsed_cap << "'";
            if ( !pkg.repo_alias.empty() )
                MIL << " from '" << pkg.repo_alias << "'";
            MIL << " already installed." << std::endl;
        }
        // TODO other kinds

        // no available object (bnc #591760)
        // !availableEmpty() <=> theone && highest
        if ( s->availableEmpty() )
        {
            addFeedback( Feedback::NO_UPD_CANDIDATE, pkg, PoolItem(), installed );
            DBG << "no available objects in repos, skipping update of " << s->name() << std::endl;
            return;
        }

        // the highest version is already there
        if ( identical( installed, highest ) || highest->edition() <= installed->edition() )
            addFeedback( Feedback::NO_UPD_CANDIDATE, pkg, selected, installed );
    }
    else if ( installed->edition() > selected->edition() )
    {
        if ( _opts.force || _opts.oldpackage )
            return;

        addFeedback( Feedback::SELECTED_IS_OLDER, pkg, selected, installed );
        MIL << "Selected is older than the installed. Will not downgrade unless oldpackage is set" << std::endl;
    }


    // there is a higher version available than the selected candidate; this
    // can happen because of repo priorities, locks, vendor lock, and
    // because of conditions given by the caller: repos/arch/version (bnc #522223)
    if ( highest           // should not happen, but just in case (bnc #607482 c#4)
      && !identical( selected, highest )
      && highest->edition() > installed->edition() )
    {
        bool userconstraints = pkg.parsed_cap.detail().isVersioned()
                            || pkg.parsed_cap.detail().hasArch()
                            || !_opts.fromRepos.empty()
                            || !pkg.repo_alias.empty();
        if ( userconstraints )
        {
            addFeedback( Feedback::UPD_CANDIDATE_USER_RESTRICTED, pkg, selected, installed );
            DBG << "Newer object exists, but has different repo/arch/version: " << highest << std::endl;
        }

        // update candidate locked (suppress if selected is not locked)
        if ( selected.status().isLocked() && ( s->status() == ui::S_Protected || highest.status().isLocked() ) )
        {
            addFeedback( Feedback::UPD_CANDIDATE_IS_LOCKED, pkg, selected, installed );
            DBG << "Newer object exists, but is locked: " << highest << std::endl;
        }

        // update candidate has different vendor
        if ( !VendorAttr::instance().equivalent( highest->vendor(), installed->vendor() )
          && !_opts.allowVendorChange )
        {
            addFeedback( Feedback::UPD_CANDIDATE_CHANGES_VENDOR, pkg, selected, installed );
            DBG << "Newer object with different vendor exists: " << highest
                << " (" << highest->vendor() << ")"
                << ". Installed vendor: " << installed->vendor() << std::endl;
        }

        // update candidate is from a low-priority (higher priority number) repo
        if ( highest->repoInfo().priority() > selected->repoInfo().priority() )
        {
            addFeedback( Feedback::UPD_CANDIDATE_HAS_LOWER_PRIO, pkg, selected, installed );
            DBG << "Newer object exists in lower-priority repo: " << highest << std::endl;
        }
    } // !identical(selected, highest) && highest->edition() > installed->edition()
}

// ----------------------------------------------------------------------------

void Requester::setToInstall( const PoolItem & pi )
{
    if ( _opts.force )
    {
        Selection sel;
        sel.kind   = Selection::Kind::SetToInstall;
        sel.item   = pi;
        sel.forced = true;
        _selections.push_back( std::move(sel) );
        addFeedback( Feedback::FORCED_INSTALL, PackageSpec(), pi );
    }
    else if ( ui::asSelectable()(pi)->hasLocks() )
    {
        // Use a solver request instead of selecting the item directly.
        // This lets the solver report the lock conflict, whereas directly
        // selecting the item would silently remove the lock (REVIEW.md F02).
        sat::Solvable solv( pi.satSolvable() );
        Capability cap( solv.arch(), solv.name(), Rel::EQ, solv.edition(), solv.kind() );
        Selection sel;
        sel.kind       = Selection::Kind::AddRequire;
        sel.capability = cap;
        _selections.push_back( std::move(sel) );
        _requires.insert( cap );
        addFeedback( Feedback::SET_TO_INSTALL, PackageSpec(), pi );
        addFeedback( Feedback::INSTALLED_LOCKED, PackageSpec(), pi );
        return;
    }
    else
    {
        Selection sel;
        sel.kind   = Selection::Kind::SetToInstall;
        sel.item   = pi;
        sel.forced = false;
        _selections.push_back( std::move(sel) );
        addFeedback( Feedback::SET_TO_INSTALL, PackageSpec(), pi );
    }
    _toinst.insert( pi );
}

// ----------------------------------------------------------------------------

void Requester::setToRemove( const PoolItem & pi )
{
    if ( ui::asSelectable()(pi)->hasLocks() )
    {
        sat::Solvable solv( pi.satSolvable() );
        Capability cap( solv.arch(), solv.name(), Rel::EQ, solv.edition(), solv.kind() );
        Selection sel;
        sel.kind       = Selection::Kind::AddConflict;
        sel.capability = cap;
        _selections.push_back( std::move(sel) );
        _conflicts.insert( cap );
        addFeedback( Feedback::SET_TO_REMOVE, PackageSpec(), pi );
        addFeedback( Feedback::INSTALLED_LOCKED, PackageSpec(), pi );
        return;
    }
    Selection sel;
    sel.kind = Selection::Kind::SetToRemove;
    sel.item = pi;
    _selections.push_back( std::move(sel) );
    addFeedback( Feedback::SET_TO_REMOVE, PackageSpec(), pi );
    _toremove.insert( pi );
}

// ----------------------------------------------------------------------------

void Requester::addRequirement( const PackageSpec & pkg )
{
    Selection sel;
    sel.kind       = Selection::Kind::AddRequire;
    sel.capability = pkg.parsed_cap;
    _selections.push_back( std::move(sel) );
    addFeedback( Feedback::ADDED_REQUIREMENT, pkg );
    _requires.insert( pkg.parsed_cap );
}

// ----------------------------------------------------------------------------

void Requester::addConflict( const PackageSpec & pkg )
{
    Selection sel;
    sel.kind       = Selection::Kind::AddConflict;
    sel.capability = pkg.parsed_cap;
    _selections.push_back( std::move(sel) );
    addFeedback( Feedback::ADDED_CONFLICT, pkg );
    _conflicts.insert( pkg.parsed_cap );
}

// ----------------------------------------------------------------------------

bool Requester::hasFeedback( const Feedback::Id id ) const
{
    for_( fb, _feedback.begin(), _feedback.end() )
        if ( fb->id() == id )
            return true;
    return false;
}

// ----------------------------------------------------------------------------

std::set<PoolItem> Requester::toInstall() const { return _toinst; }
std::set<PoolItem> Requester::toRemove()  const { return _toremove; }
std::set<Capability> Requester::dep_requires()  const { return _requires; }
std::set<Capability> Requester::dep_conflicts() const { return _conflicts; }

} // namespace solverequest
