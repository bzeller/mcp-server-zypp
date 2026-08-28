// C++ unit tests for preloadProgressFrame() — the pure JSON-shape builder
// behind McpCommitPreloadReceive::progress() (callbacks.h/.cc). The point
// of interest is the deterministic key mapping libzypp's UserData uses
// ("bytesReceived"/"bytesRequired", "dbps_avg"/"dbps_current") onto this
// worker's own JSON keys ("bytes_received"/"bytes_required", same names for
// the dbps pair) — the exact contract the Go proxy's byte-based progress
// mapping (Fix 4) depends on.
//
// Deliberately tests the free function directly with a UserData built by
// the test itself, rather than driving a real network preload: whether a
// *real* download's UserData ever actually contains these keys at all is a
// separate, genuinely non-deterministic question of network timing — see
// tests/e2e/scenarios/commit_failure.py, which only asserts that
// progress() fired at all (a "percent" key present), not which of its
// optional fields happened to be populated. The key-name mapping itself has
// no such excuse: it is a pure function of a UserData the test constructs,
// so it belongs here, not in e2e.
//
// No BOOST_TEST_MODULE in this file — transaction_test.cc already defines
// the module (and therefore main()) for the zypp-mcp-tool-tests binary this
// file is linked into; this file only adds more cases to it.
#include <boost/test/unit_test.hpp>

#include "../src/callbacks.h"

#include <zypp/ZYppCallbacks.h>

BOOST_AUTO_TEST_CASE( preload_progress_frame_basic_shape )
{
    const auto frame = preloadProgressFrame( 42, zypp::callback::UserData() );

    BOOST_CHECK_EQUAL( std::string( frame.value("type").asString() ), "progress" );
    BOOST_CHECK_EQUAL( std::string( frame.value("action").asString() ), "preload" );
    BOOST_CHECK_EQUAL( frame.value("percent").asInt().value(), std::int64_t(42) );

    // None of the optional fields are present without a matching UserData key.
    BOOST_CHECK( !frame.contains("dbps_avg") );
    BOOST_CHECK( !frame.contains("dbps_current") );
    BOOST_CHECK( !frame.contains("bytes_received") );
    BOOST_CHECK( !frame.contains("bytes_required") );
}

BOOST_AUTO_TEST_CASE( preload_progress_frame_clamps_percent )
{
    BOOST_CHECK_EQUAL(
        preloadProgressFrame( -5, zypp::callback::UserData() ).value("percent").asInt().value(),
        std::int64_t(0) );
    BOOST_CHECK_EQUAL(
        preloadProgressFrame( 150, zypp::callback::UserData() ).value("percent").asInt().value(),
        std::int64_t(100) );
}

BOOST_AUTO_TEST_CASE( preload_progress_frame_maps_byte_counts )
{
    // This is the actual regression guard: libzypp's own UserData keys are
    // camelCase ("bytesReceived"/"bytesRequired" — see
    // commitpackagepreloader.cc/ZYppCallbacks.h: CommitPreloadReport), while
    // this worker's JSON output uses snake_case ("bytes_received"/
    // "bytes_required" — the names the Go proxy's progressFrame struct
    // matches against). Mixing the two up would silently defeat that
    // mapping with no compiler error, since UserData::get() is keyed by
    // plain strings.
    zypp::callback::UserData ud;
    ud.set( "bytesReceived", double(512) );
    ud.set( "bytesRequired", double(2048) );

    const auto frame = preloadProgressFrame( 25, ud );
    BOOST_REQUIRE( frame.contains("bytes_received") );
    BOOST_REQUIRE( frame.contains("bytes_required") );
    BOOST_CHECK_EQUAL( frame.value("bytes_received").asNumber().value(), 512.0 );
    BOOST_CHECK_EQUAL( frame.value("bytes_required").asNumber().value(), 2048.0 );
}

BOOST_AUTO_TEST_CASE( preload_progress_frame_maps_dbps_fields )
{
    zypp::callback::UserData ud;
    ud.set( "dbps_avg", double(1234.5) );
    ud.set( "dbps_current", double(6789.0) );

    const auto frame = preloadProgressFrame( 10, ud );
    BOOST_REQUIRE( frame.contains("dbps_avg") );
    BOOST_REQUIRE( frame.contains("dbps_current") );
    BOOST_CHECK_EQUAL( frame.value("dbps_avg").asNumber().value(), 1234.5 );
    BOOST_CHECK_EQUAL( frame.value("dbps_current").asNumber().value(), 6789.0 );
}

BOOST_AUTO_TEST_CASE( preload_progress_frame_omits_negative_optional_fields )
{
    // UserData::get<double>(key, -1.0) is the "absent" sentinel used
    // throughout preloadProgressFrame() — a genuinely negative byte count
    // or dbps value is nonsensical, but confirm the sentinel itself never
    // leaks into the frame as if it were a real value.
    zypp::callback::UserData ud;
    ud.set( "bytesReceived", double(-1.0) );

    const auto frame = preloadProgressFrame( 5, ud );
    BOOST_CHECK( !frame.contains("bytes_received") );
}
