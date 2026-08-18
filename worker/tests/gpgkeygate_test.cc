// Pure unit tests for GpgKeyGate — no zypp, no fixture, no subprocess.
#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE GpgKeyGateTests
#include <boost/test/unit_test.hpp>

#include "../src/gpgkeygate.h"

BOOST_AUTO_TEST_CASE( is_accepted_is_false_by_default_and_records_nothing )
{
    GpgKeyGate gate;
    BOOST_CHECK( !gate.isAccepted( "DEADBEEF" ) );
    BOOST_CHECK( gate.rejected().empty() );
    BOOST_CHECK( !gate.hasRejections() );
}

BOOST_AUTO_TEST_CASE( accepted_key_passes )
{
    GpgKeyGate gate;
    gate.accept( { "DEADBEEF" } );
    BOOST_CHECK( gate.isAccepted( "DEADBEEF" ) );
    BOOST_CHECK( gate.rejected().empty() );
}

BOOST_AUTO_TEST_CASE( fingerprint_match_is_case_insensitive )
{
    GpgKeyGate gate;
    gate.accept( { "deadbeef" } );
    BOOST_CHECK( gate.isAccepted( "DEADBEEF" ) );
}

BOOST_AUTO_TEST_CASE( accept_replaces_previous_set )
{
    GpgKeyGate gate;
    gate.accept( { "AAAA" } );
    BOOST_CHECK( gate.isAccepted( "AAAA" ) );

    gate.accept( { "BBBB" } );
    BOOST_CHECK( !gate.isAccepted( "AAAA" ) );
    BOOST_CHECK(  gate.isAccepted( "BBBB" ) );
}

BOOST_AUTO_TEST_CASE( rejection_is_recorded )
{
    GpgKeyGate gate;
    gate.recordRejection( "DEADBEEF", "Some Vendor", "repo-oss" );

    BOOST_REQUIRE_EQUAL( gate.rejected().size(), 1u );
    const RejectedKey & r = gate.rejected()[0];
    BOOST_CHECK_EQUAL( r.fingerprint, "DEADBEEF" );
    BOOST_CHECK_EQUAL( r.name,        "Some Vendor" );
    BOOST_CHECK_EQUAL( r.repo,        "repo-oss" );
    BOOST_CHECK( gate.hasRejections() );
}

BOOST_AUTO_TEST_CASE( repeated_rejection_deduplicated )
{
    GpgKeyGate gate;
    gate.recordRejection( "DEADBEEF", "Some Vendor", "repo-oss" );
    gate.recordRejection( "DEADBEEF", "Some Vendor", "repo-oss" );
    gate.recordRejection( "DEADBEEF", "Some Vendor", "repo-oss" );

    BOOST_CHECK_EQUAL( gate.rejected().size(), 1u );
}

BOOST_AUTO_TEST_CASE( distinct_keys_recorded_separately )
{
    GpgKeyGate gate;
    gate.recordRejection( "AAAA", "Vendor A", "repo-a" );
    gate.recordRejection( "BBBB", "Vendor B", "repo-b" );

    BOOST_CHECK_EQUAL( gate.rejected().size(), 2u );
}
