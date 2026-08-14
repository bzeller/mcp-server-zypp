// C++ unit tests for the license confirmation gate (transaction.cc).
//
// Deliberately calls collectLicensesToConfirm()/checkLicensesAccepted()
// directly, bypassing tool_confirm_install()'s entry point entirely — that
// entry point starts with an unconditional `geteuid() != 0` check (and must
// keep failing fast there, unconditionally; that check is never to be
// reordered or weakened for testability). The gate logic itself needs no
// root privileges and no real commit() — it's pure ResPool inspection, so
// it belongs in a real, no-root-needed unit test instead.
#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE TransactionLicenseTests
#include <boost/test/unit_test.hpp>

#include "../src/system.h"
#include "../src/transport.h"
#include "../src/tools/transaction.h"

#include <zypp/Resolver.h>
#include <zypp/ZYpp.h>
#include <zypp/ZYppFactory.h>
#include <zypp/sat/Pool.h>
#include <zypp-core/Pathname.h>

namespace
{
    // MCP_TEST_TESTDATA_DIR is injected by CMake (worker/CMakeLists.txt),
    // mirroring the Go suite's MCP_TESTDATA_DIR env var / testcase(t, name)
    // helper — both point at the same tests/testdata directory.
    zypp::Pathname testcase( const std::string & name )
    {
        return zypp::Pathname( MCP_TEST_TESTDATA_DIR ) / name;
    }

    // sat::Pool/getZYpp() are process-wide singletons. Unlike system.cc's
    // production loadSystem() (correctly single-shot: one call per worker
    // process), this test binary calls loadSystem() once per test case —
    // without a cleanup, repos from every prior case would accumulate in
    // the pool.
    struct ResetPoolFixture
    {
        ~ResetPoolFixture()
        {
            zypp::getZYpp()->resolver()->reset();
            zypp::getZYpp()->finishTarget();
            zypp::sat::Pool::instance().reposEraseAll();
        }
    };

    // Loads tc-license and sets up a plan_install-equivalent job for
    // pkg-license, exactly as plan_install.cc/confirm_install.cc do via
    // setupInstall() — resolves the pool and returns it ready for
    // collectLicensesToConfirm()/checkLicensesAccepted().
    zypp::ResPool resolvedLicensePool( McpTransport & t )
    {
        zypp::ZYpp::Ptr zypp = loadSystem( testcase( "tc-license" ) );

        const zypp::json::Object arg{ { { "package", std::string( "pkg-license" ) } } };
        BOOST_REQUIRE( setupInstall( arg, zypp, "test", t ) );
        BOOST_REQUIRE( zypp->resolver()->resolvePool() );

        return zypp->pool();
    }

    // tc-license-upgrade's @System is a real susetags repo (not Testtags —
    // see the fixture's zypp-control.yaml for why), so installed packages
    // can carry an =Eul: license and exercise collectLicensesToConfirm()'s
    // hasInstalledObj()/licenseToConfirm() comparison against an upgrade
    // target (mirrors zypper misc.cc::confirm_licenses, bnc#394396).
    zypp::ResPool resolvedUpgradePool( McpTransport & t, const std::string & package )
    {
        zypp::ZYpp::Ptr zypp = loadSystem( testcase( "tc-license-upgrade" ) );

        // repo restriction forces the solver to pick the newer version from
        // "avail" rather than leaving the already-satisfied installed v1 in
        // place (a plain capability requirement is a no-op here — matches
        // setupInstall's non-byCapability branch, exercised the same way
        // plan_install's own "repo" argument does).
        const zypp::json::Object arg{ {
            { "package", package },
            { "repo",    std::string( "avail" ) }
        } };
        BOOST_REQUIRE( setupInstall( arg, zypp, "test", t ) );
        BOOST_REQUIRE( zypp->resolver()->resolvePool() );

        return zypp->pool();
    }
}

BOOST_FIXTURE_TEST_CASE( fresh_install_surfaces_license, ResetPoolFixture )
{
    McpTransport t;
    const zypp::ResPool pool = resolvedLicensePool( t );

    const auto groups = collectLicensesToConfirm( pool );
    BOOST_REQUIRE_EQUAL( groups.size(), 1u );

    const auto & group = groups.begin()->second;
    BOOST_CHECK_EQUAL( group.text,
        "You must agree to this license before installing pkg-license." );
    BOOST_REQUIRE_EQUAL( group.packages.size(), 1u );
    BOOST_CHECK_EQUAL( group.packages[0], "pkg-license" );
}

BOOST_FIXTURE_TEST_CASE( unaccepted_license_blocks_confirm, ResetPoolFixture )
{
    McpTransport t;
    const zypp::ResPool pool = resolvedLicensePool( t );

    BOOST_CHECK( !checkLicensesAccepted( pool, {}, "test", t ) );
}

BOOST_FIXTURE_TEST_CASE( accepted_license_id_unblocks_confirm, ResetPoolFixture )
{
    McpTransport t;
    const zypp::ResPool pool = resolvedLicensePool( t );

    const auto groups = collectLicensesToConfirm( pool );
    BOOST_REQUIRE_EQUAL( groups.size(), 1u );
    const std::string id = groups.begin()->first;

    BOOST_CHECK( checkLicensesAccepted( pool, { id }, "test", t ) );
}

BOOST_FIXTURE_TEST_CASE( wrong_license_id_still_blocks_confirm, ResetPoolFixture )
{
    McpTransport t;
    const zypp::ResPool pool = resolvedLicensePool( t );

    BOOST_CHECK( !checkLicensesAccepted( pool, { "not-the-right-id" }, "test", t ) );
}

BOOST_FIXTURE_TEST_CASE( upgrade_with_identical_license_is_suppressed, ResetPoolFixture )
{
    McpTransport t;
    const zypp::ResPool pool = resolvedUpgradePool( t, "pkg-upgrade-same" );

    BOOST_CHECK( collectLicensesToConfirm( pool ).empty() );
    // Nothing outstanding — confirm must succeed with no accepted_licenses.
    BOOST_CHECK( checkLicensesAccepted( pool, {}, "test", t ) );
}

BOOST_FIXTURE_TEST_CASE( upgrade_with_changed_license_requires_confirmation, ResetPoolFixture )
{
    McpTransport t;
    const zypp::ResPool pool = resolvedUpgradePool( t, "pkg-upgrade-diff" );

    const auto groups = collectLicensesToConfirm( pool );
    BOOST_REQUIRE_EQUAL( groups.size(), 1u );

    const auto & group = groups.begin()->second;
    BOOST_CHECK_EQUAL( group.text, "License B." );
    BOOST_REQUIRE_EQUAL( group.packages.size(), 1u );
    BOOST_CHECK_EQUAL( group.packages[0], "pkg-upgrade-diff" );

    BOOST_CHECK( !checkLicensesAccepted( pool, {}, "test", t ) );
}
