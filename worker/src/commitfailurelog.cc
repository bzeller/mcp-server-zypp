#include "commitfailurelog.h"

#include <algorithm>

void CommitFailureLog::record( std::string package, CommitPhase phase,
                               CommitSeverity severity, std::string text )
{
    if ( _entries.size() >= kMaxEntries )
    {
        // Drop the oldest to make room — the tail of a failing transaction
        // is where the actual error is, and is what must survive.
        _entries.erase( _entries.begin() );
        _truncated = true;
    }
    _entries.push_back( { std::move(package), phase, severity, std::move(text) } );
}

bool CommitFailureLog::hasErrors() const
{
    return std::any_of( _entries.begin(), _entries.end(),
        []( const CommitFailureDetail & d ){ return d.severity == CommitSeverity::Error; } );
}

bool CommitFailureLog::hasWarnings() const
{
    return std::any_of( _entries.begin(), _entries.end(),
        []( const CommitFailureDetail & d ){ return d.severity == CommitSeverity::Warning; } );
}
