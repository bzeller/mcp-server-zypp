#ifndef MCP_SERVER_ZYPP_SOLVER_TEST_TESTSETUP_H
#define MCP_SERVER_ZYPP_SOLVER_TEST_TESTSETUP_H

// Minimal, zypper-independent replacement for zypper/tests/lib/TestSetup.h,
// providing just the repo-loading operations the ported SolverRequester
// test suite (solverrequester_test.cc) needs: loadTargetRepo()/loadRepo()/
// pool()/resolver().
//
// This is deliberately NOT a full port of TestSetup. TestSetup's Impl also
// binds Zypper::instance() (`Zypper & _zypper = Zypper::instance();`) and
// writes into its config (root_dir, rm_options) — a real, load-bearing
// dependency for the rest of zypper's own test suite, not incidental. But
// the specific methods used here never read that state at all: repomanager()
// goes through RepoManagerOptions::makeTestSetup(rootdir) — a plain static
// factory taking only a Pathname — and loadRepo()/pool()/resolver() go
// straight through libzypp's own getZYpp()/ResPool::instance()/
// sat::Pool::instance() globals, which is the same category of process-wide
// pool singleton every other tool in this worker already reads (see
// worker/src/system.cc: loadSystem()). Linking zypper's own Zypper/Config/
// output-writer classes into this test binary just to obtain a root
// directory string we can equally construct ourselves would reintroduce
// exactly the zypper coupling this whole port exists to remove.

#include <string>

// solverrequester_test.cc uses BOOST_AUTO_TEST_CASE/BOOST_CHECK, and the
// libzypp MIL/DBG/WAR/endl trace macros in its own body (ApplyLock,
// TestInit — both ported from upstream verbatim), without including
// either itself. Upstream's TestSetup.h provided both
// (<boost/test/unit_test.hpp>, and <zypp-core/base/LogTools.h> via
// LogControl.h) for the same reason; this file plays that same role for
// the ported test now.
#include <boost/test/unit_test.hpp>
#include <zypp-core/base/LogTools.h>

#undef  ZYPP_BASE_LOGGER_LOGGROUP
#define ZYPP_BASE_LOGGER_LOGGROUP "solverequest-test"

#include <zypp-core/Pathname.h>
#include <zypp/RepoInfo.h>
#include <zypp/RepoManager.h>
#include <zypp/ResPool.h>
#include <zypp/Resolver.h>
#include <zypp-core/Url.h>
#include <zypp/ZConfig.h>
#include <zypp/ZYppFactory.h>
#include <zypp/sat/Pool.h>
#include <zypp-core/fs/TmpPath.h>

namespace solverequest_test
{

/// Builds a throwaway repo cache below a fresh temp directory and loads
/// repos into the one process-wide pool — mirrors the handful of
/// zypper::TestSetup operations solverrequester_test.cc actually calls.
class SolverTestSetup
{
public:
    explicit SolverTestSetup( const zypp::Arch & sysarch = zypp::Arch_empty )
    : _rootdir( _tmpdir.path() )
    {
        // Mirrors TestSetup::Impl's ctor: only used to seed ZConfig's
        // system architecture for solving; no other side effect.
        if ( ! sysarch.empty() )
            zypp::ZConfig::instance().setSystemArchitecture( sysarch );
    }

    zypp::RepoManager repomanager() const
    { return zypp::RepoManager( zypp::RepoManagerOptions::makeTestSetup( _rootdir ) ); }

    zypp::ResPool pool() const { return zypp::ResPool::instance(); }
    zypp::Resolver & resolver() const { return *zypp::getZYpp()->resolver(); }

    /// Fake @System repo from a repodata directory — mirrors
    /// TestSetup::loadTargetRepo(const Pathname&).
    void loadTargetRepo( const zypp::Pathname & path )
    { loadRepo( path, zypp::sat::Pool::systemRepoAlias() ); }

    /// Load repo metadata from a directory into the pool under the given
    /// alias — mirrors TestSetup::loadRepo(const Pathname&, const string&).
    void loadRepo( const zypp::Pathname & path, const std::string & alias )
    {
        zypp::RepoInfo nrepo;
        nrepo.setAlias( alias );
        nrepo.addBaseUrl( path.asUrl() );
        nrepo.setGpgCheck( false );
        loadRepo( nrepo );
    }

    /// \overload taking a fully-populated RepoInfo (e.g. with a priority
    /// already set) — mirrors TestSetup::loadRepo(RepoInfo).
    void loadRepo( zypp::RepoInfo nrepo )
    {
        zypp::RepoManager rmanager( repomanager() );
        rmanager.addRepository( nrepo );
        rmanager.buildCache( nrepo );
        rmanager.loadFromCache( nrepo );
    }

private:
    zypp::filesystem::TmpDir _tmpdir;
    zypp::Pathname           _rootdir;
};

} // namespace solverequest_test

#endif // MCP_SERVER_ZYPP_SOLVER_TEST_TESTSETUP_H
