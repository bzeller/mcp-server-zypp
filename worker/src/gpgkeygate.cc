#include "gpgkeygate.h"

#include <algorithm>
#include <cctype>

namespace
{
    /// Fingerprints are hex; callers may echo them back in either case.
    /// Normalising avoids a rejection that would look arbitrary to the
    /// caller, without weakening the comparison itself.
    std::string normalize( std::string s )
    {
        std::transform( s.begin(), s.end(), s.begin(),
                        []( unsigned char c ){ return std::toupper( c ); } );
        return s;
    }
}

void GpgKeyGate::accept( const std::set<std::string> & fingerprints_r )
{
    _accepted.clear();
    for ( const auto & fp : fingerprints_r )
        _accepted.insert( normalize( fp ) );
}

bool GpgKeyGate::isAccepted( const std::string & fingerprint_r,
                             const std::string & name_r,
                             const std::string & repo_r )
{
    if ( _accepted.count( normalize( fingerprint_r ) ) )
        return true;

    // One key typically signs many packages — record it once.
    const auto seen = std::find_if( _rejected.begin(), _rejected.end(),
        [&]( const RejectedKey & r ){ return r.fingerprint == fingerprint_r; } );
    if ( seen == _rejected.end() )
        _rejected.push_back( { fingerprint_r, name_r, repo_r } );

    return false;
}
