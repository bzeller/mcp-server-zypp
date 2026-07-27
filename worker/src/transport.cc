#include "transport.h"

#include <cstdio>
#include <cstring>
#include <ostream>
#include <string>

#include <zypp-core/base/Logger.h>

McpTransport::McpTransport()
{
    // Disable buffering on stdout so each writeFrame reaches the proxy immediately.
    std::setvbuf( stdout, nullptr, _IONBF, 0 );
}

void McpTransport::writeFrame( std::string_view payload )
{
    // "Content-Length: <N>\r\n\r\n<payload>"
    char header[64];
    const int hlen = std::snprintf( header, sizeof(header),
                                    "Content-Length: %zu\r\n\r\n", payload.size() );
    // snprintf returns the number of chars that *would* have been written
    // (excluding the terminator), or a negative value on encoding error.
    // A return >= sizeof(header) means truncation — the buffer is too small
    // for the given payload size; a negative value is a formatting failure.
    // Casting either case to size_t and handing it to fwrite would read
    // past the end of the stack buffer, so bail out defensively instead.
    if ( hlen < 0 || static_cast<std::size_t>(hlen) >= sizeof(header) )
    {
        ERR << "McpTransport::writeFrame: header formatting failed or was "
               "truncated (hlen=" << hlen << ", payload.size()=" << payload.size()
            << "); dropping frame." << std::endl;
        return;
    }
    ::fwrite( header,          1, static_cast<std::size_t>(hlen), stdout );
    ::fwrite( payload.data(),  1, payload.size(),                 stdout );
    // No fflush needed — _IONBF set in ctor means every write goes through.
}

std::optional<std::string> McpTransport::readFrame()
{
    // ── Read headers until "\r\n\r\n" ────────────────────────────────────────
    // Bounded to prevent a peer from forcing unbounded memory growth / CPU
    // time by never sending the terminator (DoS). This only guards the
    // *header* line itself ("Content-Length: N\r\n\r\n"), which is always a
    // few dozen bytes regardless of payload size — 4 KiB is generous
    // headroom and unrelated to how large the framed payload may be.
    static constexpr std::size_t kMaxHeaderSize = 4096;

    std::string headers;
    headers.reserve( 64 );

    char c;
    while ( ::fread( &c, 1, 1, stdin ) == 1 )
    {
        headers += c;
        if ( headers.size() >= 4 &&
             headers[headers.size()-4] == '\r' &&
             headers[headers.size()-3] == '\n' &&
             headers[headers.size()-2] == '\r' &&
             headers[headers.size()-1] == '\n' )
            break;

        if ( headers.size() > kMaxHeaderSize )
        {
            ERR << "McpTransport::readFrame: header exceeded " << kMaxHeaderSize
                << " bytes without a terminator; aborting connection." << std::endl;
            return std::nullopt;
        }
    }

    if ( headers.empty() )
        return std::nullopt; // EOF before any data

    // ── Extract Content-Length ────────────────────────────────────────────────
    static constexpr std::string_view kHeader = "Content-Length: ";
    auto pos = headers.find( kHeader );
    if ( pos == std::string::npos )
        return std::nullopt;

    std::size_t len = 0;
    try { len = std::stoul( headers.substr( pos + kHeader.size() ) ); }
    catch ( ... ) { return std::nullopt; }

    if ( len == 0 )
        return std::nullopt;

    // ── Bound the payload size ────────────────────────────────────────────────
    // Unlike the header cap above, this DOES need headroom for legitimately
    // large frames — e.g. a full `zypp dup` transaction plan can easily run
    // into several MB of JSON. 256 MiB comfortably covers any realistic
    // solver result while still rejecting a hostile/malformed peer that
    // claims a multi-GB Content-Length and would otherwise force an
    // oversized allocation in `std::string body(len, '\0')` below.
    static constexpr std::size_t kMaxPayloadSize = 256u * 1024 * 1024;
    if ( len > kMaxPayloadSize )
    {
        ERR << "McpTransport::readFrame: Content-Length " << len
            << " exceeds max allowed payload size " << kMaxPayloadSize
            << "; aborting connection." << std::endl;
        return std::nullopt;
    }

    // ── Read exactly len bytes of payload ────────────────────────────────────
    std::string body( len, '\0' );
    if ( ::fread( body.data(), 1, len, stdin ) != len )
        return std::nullopt;

    return body;
}
