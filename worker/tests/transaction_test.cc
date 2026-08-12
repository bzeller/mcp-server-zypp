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
}

BOOST_AUTO_TEST_CASE( fresh_install_surfaces_license )
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

BOOST_AUTO_TEST_CASE( unaccepted_license_blocks_confirm )
{
    McpTransport t;
    const zypp::ResPool pool = resolvedLicensePool( t );

    BOOST_CHECK( !checkLicensesAccepted( pool, {}, "test", t ) );
}

BOOST_AUTO_TEST_CASE( accepted_license_id_unblocks_confirm )
{
    McpTransport t;
    const zypp::ResPool pool = resolvedLicensePool( t );

    const auto groups = collectLicensesToConfirm( pool );
    BOOST_REQUIRE_EQUAL( groups.size(), 1u );
    const std::string id = groups.begin()->first;

    BOOST_CHECK( checkLicensesAccepted( pool, { id }, "test", t ) );
}

BOOST_AUTO_TEST_CASE( wrong_license_id_still_blocks_confirm )
{
    McpTransport t;
    const zypp::ResPool pool = resolvedLicensePool( t );

    BOOST_CHECK( !checkLicensesAccepted( pool, { "not-the-right-id" }, "test", t ) );
}
