#ifndef MCP_SERVER_ZYPP_TRANSPORT_H
#define MCP_SERVER_ZYPP_TRANSPORT_H

#include <optional>
#include <string>
#include <string_view>
#include <cstddef>

/// Content-Length framed transport over stdin/stdout.
///
/// Wire format (mirrors LSP / JSON-RPC):
///   "Content-Length: <N>\r\n\r\n<N bytes of payload>"
///
/// Payload may contain any byte including '\n' — framing is by length,
/// not by delimiter.
///
/// Note: --list-tools bypasses this transport entirely and writes plain
/// JSON to stdout. Only interactive --tool invocations use this class.
class McpTransport
{
public:
    McpTransport();

    /// Write a single JSON payload to stdout, preceded by a Content-Length header.
    void writeFrame( std::string_view payload );

    /// Blocking read of one Content-Length framed message from stdin.
    /// Returns nullopt on EOF, read error, or missing/malformed header.
    std::optional<std::string> readFrame();

private:
    McpTransport( const McpTransport & ) = delete;
    McpTransport & operator=( const McpTransport & ) = delete;
};

#endif // MCP_SERVER_ZYPP_TRANSPORT_H
