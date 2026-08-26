// Pure unit tests for CommitFailureLog — no zypp, no fixture, no subprocess.
#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE CommitFailureLogTests
#include <boost/test/unit_test.hpp>

#include "../src/commitfailurelog.h"

BOOST_AUTO_TEST_CASE( empty_log_has_no_issues )
{
    CommitFailureLog log;
    BOOST_CHECK( !log.hasErrors() );
    BOOST_CHECK( !log.hasWarnings() );
    BOOST_CHECK( !log.wasTruncated() );
    BOOST_CHECK( log.entries().empty() );
}

BOOST_AUTO_TEST_CASE( recorded_entry_is_visible )
{
    CommitFailureLog log;
    log.record( "foo", CommitPhase::Download, CommitSeverity::Error, "connection timed out" );

    BOOST_CHECK( log.hasErrors() );
    BOOST_REQUIRE_EQUAL( log.entries().size(), 1u );
    const auto & e = log.entries()[0];
    BOOST_CHECK_EQUAL( e.package, "foo" );
    BOOST_CHECK( e.phase == CommitPhase::Download );
    BOOST_CHECK( e.severity == CommitSeverity::Error );
    BOOST_CHECK_EQUAL( e.text, "connection timed out" );
}

BOOST_AUTO_TEST_CASE( transaction_scoped_entry_has_empty_package )
{
    CommitFailureLog log;
    log.record( "", CommitPhase::Preload, CommitSeverity::Error, "some packages could not be provided" );

    BOOST_REQUIRE_EQUAL( log.entries().size(), 1u );
    BOOST_CHECK( log.entries()[0].package.empty() );
}

BOOST_AUTO_TEST_CASE( entries_preserve_arrival_order )
{
    CommitFailureLog log;
    log.record( "a", CommitPhase::Install, CommitSeverity::Error, "first" );
    log.record( "b", CommitPhase::Install, CommitSeverity::Error, "second" );
    log.record( "c", CommitPhase::Remove,  CommitSeverity::Error, "third" );

    BOOST_REQUIRE_EQUAL( log.entries().size(), 3u );
    BOOST_CHECK_EQUAL( log.entries()[0].text, "first" );
    BOOST_CHECK_EQUAL( log.entries()[1].text, "second" );
    BOOST_CHECK_EQUAL( log.entries()[2].text, "third" );
}

BOOST_AUTO_TEST_CASE( duplicate_looking_entries_are_not_deduplicated )
{
    // Unlike GpgKeyGate's rejection recording, repeated identical rpm
    // output lines are each meaningful (e.g. per-package repetition) and
    // must all be retained.
    CommitFailureLog log;
    log.record( "foo", CommitPhase::Script, CommitSeverity::Warning, "warning: %post scriptlet failed" );
    log.record( "foo", CommitPhase::Script, CommitSeverity::Warning, "warning: %post scriptlet failed" );

    BOOST_CHECK_EQUAL( log.entries().size(), 2u );
}

BOOST_AUTO_TEST_CASE( truncation_keeps_the_tail_not_the_head )
{
    CommitFailureLog log;
    for ( std::size_t i = 0; i < CommitFailureLog::kMaxEntries + 5; ++i )
        log.record( "pkg", CommitPhase::Install, CommitSeverity::Detail, "line " + std::to_string(i) );

    BOOST_CHECK( log.wasTruncated() );
    BOOST_CHECK_EQUAL( log.entries().size(), CommitFailureLog::kMaxEntries );

    // The oldest 5 entries ("line 0".."line 4") must have been dropped;
    // the most recent entry ("line <max+4>") must survive.
    BOOST_CHECK_EQUAL( log.entries().front().text, "line 5" );
    BOOST_CHECK_EQUAL( log.entries().back().text,
                      "line " + std::to_string( CommitFailureLog::kMaxEntries + 4 ) );
}

BOOST_AUTO_TEST_CASE( truncation_keeps_the_tail_with_mixed_severities )
{
    // Not just line content but severity itself must survive truncation
    // correctly — the oldest entries dropped here are Detail, the ones
    // that must survive include the Error planted near the end.
    CommitFailureLog log;
    for ( std::size_t i = 0; i < CommitFailureLog::kMaxEntries; ++i )
        log.record( "pkg", CommitPhase::Install, CommitSeverity::Detail, "line " + std::to_string(i) );
    log.record( "pkg", CommitPhase::Install, CommitSeverity::Error, "the actual error" );

    BOOST_CHECK( log.wasTruncated() );
    BOOST_CHECK( log.hasErrors() );
    BOOST_CHECK_EQUAL( log.entries().back().text, "the actual error" );
    BOOST_CHECK( log.entries().back().severity == CommitSeverity::Error );
}

BOOST_AUTO_TEST_CASE( not_truncated_below_the_cap )
{
    CommitFailureLog log;
    for ( std::size_t i = 0; i < CommitFailureLog::kMaxEntries; ++i )
        log.record( "pkg", CommitPhase::Install, CommitSeverity::Detail, "line " + std::to_string(i) );

    BOOST_CHECK( !log.wasTruncated() );
    BOOST_CHECK_EQUAL( log.entries().size(), CommitFailureLog::kMaxEntries );
}

// ─── Severity ────────────────────────────────────────────────────────────────
// The whole point of severity: Detail entries are recorded on every commit,
// successful ones included (they are the only source of detail in
// SingleTrans mode) — so entry count alone must never be read as "the
// commit failed". Only hasErrors()/hasWarnings() may be.

BOOST_AUTO_TEST_CASE( detail_only_log_has_no_errors_or_warnings )
{
    CommitFailureLog log;
    log.record( "foo", CommitPhase::Install, CommitSeverity::Detail, "Preparing...   [100%]" );
    log.record( "foo", CommitPhase::Install, CommitSeverity::Detail, "Installing foo-1.0" );

    BOOST_CHECK( !log.hasErrors() );
    BOOST_CHECK( !log.hasWarnings() );
    BOOST_CHECK_EQUAL( log.entries().size(), 2u ); // still recorded, just not an issue
}

BOOST_AUTO_TEST_CASE( warning_only_log_has_no_errors )
{
    CommitFailureLog log;
    log.record( "foo", CommitPhase::Script, CommitSeverity::Warning,
               "warning: %posttrans(foo) scriptlet failed, exit status 1" );

    BOOST_CHECK( !log.hasErrors() );
    BOOST_CHECK( log.hasWarnings() );
}

BOOST_AUTO_TEST_CASE( error_entry_sets_has_errors_not_has_warnings_alone )
{
    CommitFailureLog log;
    log.record( "foo", CommitPhase::Download, CommitSeverity::Error, "connection timed out" );

    BOOST_CHECK( log.hasErrors() );
    BOOST_CHECK( !log.hasWarnings() );
}

BOOST_AUTO_TEST_CASE( mixed_severities_set_both_flags_independently )
{
    CommitFailureLog log;
    log.record( "a", CommitPhase::Install, CommitSeverity::Detail,  "routine output" );
    log.record( "b", CommitPhase::Script,  CommitSeverity::Warning, "scriptlet warning" );
    log.record( "c", CommitPhase::Remove,  CommitSeverity::Error,   "remove failed" );

    BOOST_CHECK( log.hasErrors() );
    BOOST_CHECK( log.hasWarnings() );
}

BOOST_AUTO_TEST_CASE( phase_name_matches_expected_wire_strings )
{
    BOOST_CHECK_EQUAL( phaseName( CommitPhase::Download ),    "download" );
    BOOST_CHECK_EQUAL( phaseName( CommitPhase::Install ),     "install" );
    BOOST_CHECK_EQUAL( phaseName( CommitPhase::Remove ),      "remove" );
    BOOST_CHECK_EQUAL( phaseName( CommitPhase::Script ),      "script" );
    BOOST_CHECK_EQUAL( phaseName( CommitPhase::Cleanup ),     "cleanup" );
    BOOST_CHECK_EQUAL( phaseName( CommitPhase::Transaction ), "transaction" );
    BOOST_CHECK_EQUAL( phaseName( CommitPhase::Preload ),     "preload" );
}

BOOST_AUTO_TEST_CASE( severity_name_matches_expected_wire_strings )
{
    BOOST_CHECK_EQUAL( severityName( CommitSeverity::Detail ),  "detail" );
    BOOST_CHECK_EQUAL( severityName( CommitSeverity::Warning ), "warning" );
    BOOST_CHECK_EQUAL( severityName( CommitSeverity::Error ),   "error" );
}
