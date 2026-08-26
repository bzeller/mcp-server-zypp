#ifndef MCP_SERVER_ZYPP_COMMITFAILURELOG_H
#define MCP_SERVER_ZYPP_COMMITFAILURELOG_H

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

/// Which report family produced a captured detail. Closed set, one value
/// per connected report family (see callbacks.h) — deliberately an enum
/// rather than a free-form string: the wire format only needs this at
/// serialization time (see phaseName() below), and every other consumer
/// (recording, grouping, tests) benefits from the compiler catching an
/// exhaustiveness gap if a phase is ever added.
enum class CommitPhase
{
    Download,
    Install,
    Remove,
    Script,
    Cleanup,
    Transaction,
    Preload,
};

/// Wire-format name for a phase — "download", "install", etc. The single
/// place that maps CommitPhase to the string an MCP client sees; kept here
/// (not in the JSON builder) so this class stays self-describing and its
/// enum's full membership doesn't need to be known by callers. constexpr:
/// every case returns a string literal, so this costs nothing at runtime
/// and an unhandled enumerator becomes a compile-time-checkable switch
/// rather than a runtime fallback string.
constexpr std::string_view phaseName( CommitPhase phase )
{
    switch ( phase )
    {
        case CommitPhase::Download:    return "download";
        case CommitPhase::Install:     return "install";
        case CommitPhase::Remove:      return "remove";
        case CommitPhase::Script:      return "script";
        case CommitPhase::Cleanup:     return "cleanup";
        case CommitPhase::Transaction: return "transaction";
        case CommitPhase::Preload:     return "preload";
    }
    return "unknown"; // unreachable for a valid CommitPhase; not UB on a bad cast
}

/// How much a captured entry actually means. Needed because raw rpm output
/// is recorded unconditionally (it is the only source of detail in
/// SingleTrans mode), so entry count alone cannot distinguish "a commit
/// failed" from "a commit succeeded and rpm was chatty".
enum class CommitSeverity
{
    /// Supporting context — raw rpm output. Not itself evidence of a
    /// problem; only worth showing alongside something that is.
    Detail,
    /// Something went wrong but the step still completed. Mirrors zypper's
    /// ZYPPER_EXIT_INF_RPM_SCRIPT_FAILED (exit code 107, in the
    /// informational band): the canonical case is a %posttrans scriptlet
    /// failing with WARN, which leaves every transaction step DONE.
    Warning,
    /// Something went wrong and the step did not complete.
    Error,
};

/// Wire-format name for a severity. constexpr for the same reasons as
/// phaseName().
constexpr std::string_view severityName( CommitSeverity severity )
{
    switch ( severity )
    {
        case CommitSeverity::Detail:  return "detail";
        case CommitSeverity::Warning: return "warning";
        case CommitSeverity::Error:   return "error";
    }
    return "unknown"; // unreachable for a valid CommitSeverity
}

/// One captured diagnostic: a problem()/finish() description, or a
/// transaction-wide log line. package is empty for transaction-scoped
/// entries (no single package caused it, or none could be identified —
/// e.g. a preload failure before any package-specific callback fired).
struct CommitFailureDetail
{
    std::string    package;   ///< empty: not attributable to one package
    CommitPhase    phase;
    CommitSeverity severity;
    std::string    text;
};

/// Captures the diagnostic text libzypp's report callbacks carry during a
/// commit — problem()/finish() descriptions and, in SingleTrans mode,
/// contentRpmout lines and err/crt contentLogline entries — none of which
/// ZYppCommitResult retains afterward (sat::Transaction::Step has no
/// message field at all; see sat/Transaction.h). Recorded during commit,
/// queried after, exactly like GpgKeyGate's accept/reject shape.
///
/// Capturing must never influence control flow: every problem() override
/// that feeds this class still returns the base class's default Action
/// unchanged (see callbacks.h) — this class is purely a sink.
///
/// Bounded: rpm output on a failing transaction can be arbitrarily large,
/// and this ends up serialized into a single MCP error frame. Once
/// kMaxEntries is reached, the *oldest* entry is dropped to make room —
/// the tail of a failing transaction is where the actual error is, and is
/// what must survive truncation — and wasTruncated() becomes true.
///
/// Deliberately free of libzypp types: plain logic, unit-tested as such,
/// independent of the zypp object library (mirrors GpgKeyGate).
class CommitFailureLog
{
public:
    /// Maximum entries retained in total. Chosen generously for a genuine
    /// failure report while still bounding worst case: a multi-package
    /// SingleTrans transaction streaming rpm scriptlet output per package.
    static constexpr std::size_t kMaxEntries = 200;

    /// Record a problem()/finish() description or a captured log line.
    /// package may be empty (see CommitFailureDetail).
    void record( std::string package, CommitPhase phase,
                 CommitSeverity severity, std::string text );

    /// Any Error-severity entry — i.e. something that actually prevented a
    /// step from completing. Deliberately NOT "any entry at all": Detail
    /// entries are recorded on every commit, successful ones included, so
    /// entry count alone cannot signal failure.
    bool hasErrors() const;

    /// Any Warning-severity entry — the commit may still have completed
    /// fully (see CommitSeverity::Warning).
    bool hasWarnings() const;

    bool wasTruncated() const { return _truncated; }

    const std::vector<CommitFailureDetail> & entries() const { return _entries; }

private:
    std::vector<CommitFailureDetail> _entries;
    bool                              _truncated = false;
};

#endif // MCP_SERVER_ZYPP_COMMITFAILURELOG_H
