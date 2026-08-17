// Pure unit tests for GpgKeyGate — no zypp, no fixture, no subprocess.
#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE GpgKeyGateTests
#include <boost/test/unit_test.hpp>

#include "../src/gpgkeygate.h"

BOOST_AUTO_TEST_CASE( unaccepted_key_is_rejected_and_recorded )
{
    GpgKeyGate gate;

    BOOST_CHECK( !gate.isAccepted( "DEADBEEF", "Some Vendor", "repo-oss" ) );
    BOOST_REQUIRE_EQUAL( gate.rejected().size(), 1u );

    const RejectedKey & r = gate.rejected()[0];
    BOOST_CHECK_EQUAL( r.fingerprint, "DEADBEEF" );
    BOOST_CHECK_EQUAL( r.name,        "Some Vendor" );
    BOOST_CHECK_EQUAL( r.repo,        "repo-oss" );
    BOOST_CHECK( gate.hasRejections() );
}

BOOST_AUTO_TEST_CASE( accepted_key_passes )
{
    GpgKeyGate gate;
    gate.accept( { "DEADBEEF" } );

    BOOST_CHECK( gate.isAccepted( "DEADBEEF", "Some Vendor", "repo-oss" ) );
    BOOST_CHECK( !gate.hasRejections() );
}

BOOST_AUTO_TEST_CASE( fingerprint_match_is_case_insensitive )
{
    GpgKeyGate gate;
    gate.accept( { "deadbeef" } );

    BOOST_CHECK( gate.isAccepted( "DEADBEEF", "Some Vendor", "repo-oss" ) );
    BOOST_CHECK( !gate.hasRejections() );
}

BOOST_AUTO_TEST_CASE( repeated_rejection_deduplicated )
{
    GpgKeyGate gate;

    gate.isAccepted( "DEADBEEF", "Some Vendor", "repo-oss" );
    gate.isAccepted( "DEADBEEF", "Some Vendor", "repo-oss" );
    gate.isAccepted( "DEADBEEF", "Some Vendor", "repo-oss" );

    BOOST_CHECK_EQUAL( gate.rejected().size(), 1u );
}

BOOST_AUTO_TEST_CASE( accept_replaces_previous_set )
{
    GpgKeyGate gate;

    gate.accept( { "AAAA" } );
    BOOST_CHECK( gate.isAccepted( "AAAA", "A", "repo" ) );

    gate.accept( { "BBBB" } );
    BOOST_CHECK( !gate.isAccepted( "AAAA", "A", "repo" ) );
    BOOST_CHECK(  gate.isAccepted( "BBBB", "B", "repo" ) );
}
