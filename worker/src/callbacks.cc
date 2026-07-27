#include "callbacks.h"

#include <algorithm>
#include <sstream>
#include <zypp-core/base/String.h>

// ─── JSON helpers (minimal, no external dep) ─────────────────────────────────
namespace
{
    std::string jsonEscape( const std::string & s )
    {
        std::string out;
        out.reserve( s.size() + 8 );
        for ( char c : s )
        {
            switch ( c )
            {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
            }
        }
        return out;
    }

    /// Parse a simple {"answer": "..."} frame. Returns the value or empty on failure.
    std::string parseAnswer( const std::string & frame )
    {
        // Minimal extraction — does not handle nested objects.
        auto pos = frame.find( "\"answer\"" );
        if ( pos == std::string::npos )
            return {};
        pos = frame.find( '"', pos + 8 );
        if ( pos == std::string::npos )
            return {};
        ++pos; // skip opening quote
        auto end = frame.find( '"', pos );
        if ( end == std::string::npos )
            return {};
        return frame.substr( pos, end - pos );
    }

    bool parseBoolAnswer( const std::string & frame )
    {
        std::string val = parseAnswer( frame );
        return val == "true" || val == "yes" || val == "accept";
    }
}

// ─── McpKeyRingReceive ───────────────────────────────────────────────────────
zypp::KeyRingReport::KeyTrust McpKeyRingReceive::askUserToAcceptKey(
    const zypp::PublicKey & key,
    const zypp::KeyContext & ctx )
{
    std::ostringstream json;
    json << R"({"type":"elicitation","method":"trust_key","data":{)"
         << R"("fingerprint":")" << jsonEscape( key.fingerprint() ) << "\","
         << R"("name":")" << jsonEscape( key.name() ) << "\","
         << R"("created":")" << key.created().asString() << "\","
         << R"("expires":")" << key.expiresAsString() << "\"";
    if ( !ctx.empty() )
        json << R"(,"repo":")" << jsonEscape( ctx.repoInfo().asUserString() ) << "\"";
    json << "}}";

    _t.writeFrame( json.str() );
    auto ans = _t.readFrame();
    if ( !ans )
        return zypp::KeyRingReport::KEY_DONT_TRUST; // fail closed on EOF

    std::string answer = parseAnswer( *ans );
    if ( answer == "import" )
        return zypp::KeyRingReport::KEY_TRUST_AND_IMPORT;
    if ( answer == "trust" )
        return zypp::KeyRingReport::KEY_TRUST_TEMPORARILY;
    return zypp::KeyRingReport::KEY_DONT_TRUST;
}

bool McpKeyRingReceive::askUserToAcceptUnsignedFile(
    const std::string & file,
    const zypp::KeyContext & ctx )
{
    std::ostringstream json;
    json << R"({"type":"elicitation","method":"accept_unsigned_file","data":{)"
         << R"("file":")" << jsonEscape( file ) << "\"";
    if ( !ctx.empty() )
        json << R"(,"repo":")" << jsonEscape( ctx.repoInfo().asUserString() ) << "\"";
    json << "}}";

    _t.writeFrame( json.str() );
    auto ans = _t.readFrame();
    return ans ? parseBoolAnswer( *ans ) : false;
}

bool McpKeyRingReceive::askUserToAcceptUnknownKey(
    const std::string & file,
    const std::string & id,
    const zypp::KeyContext & ctx )
{
    std::ostringstream json;
    json << R"({"type":"elicitation","method":"accept_unknown_key","data":{)"
         << R"("file":")" << jsonEscape( file ) << "\","
         << R"("keyid":")" << jsonEscape( id ) << "\"";
    if ( !ctx.empty() )
        json << R"(,"repo":")" << jsonEscape( ctx.repoInfo().asUserString() ) << "\"";
    json << "}}";

    _t.writeFrame( json.str() );
    auto ans = _t.readFrame();
    return ans ? parseBoolAnswer( *ans ) : false;
}

bool McpKeyRingReceive::askUserToAcceptVerificationFailed(
    const std::string & file,
    const zypp::PublicKey & key,
    const zypp::KeyContext & ctx )
{
    std::ostringstream json;
    json << R"({"type":"elicitation","method":"accept_verification_failed","data":{)"
         << R"("file":")" << jsonEscape( file ) << "\","
         << R"("fingerprint":")" << jsonEscape( key.fingerprint() ) << "\","
         << R"("name":")" << jsonEscape( key.name() ) << "\"";
    if ( !ctx.empty() )
        json << R"(,"repo":")" << jsonEscape( ctx.repoInfo().asUserString() ) << "\"";
    json << "}}";

    _t.writeFrame( json.str() );
    auto ans = _t.readFrame();
    return ans ? parseBoolAnswer( *ans ) : false;
}

// ─── McpDigestReceive ────────────────────────────────────────────────────────
bool McpDigestReceive::askUserToAcceptNoDigest( const zypp::Pathname & file )
{
    std::ostringstream json;
    json << R"({"type":"elicitation","method":"accept_no_digest","data":{)"
         << R"("file":")" << jsonEscape( file.asString() ) << "\"}}";

    _t.writeFrame( json.str() );
    auto ans = _t.readFrame();
    return ans ? parseBoolAnswer( *ans ) : false;
}

bool McpDigestReceive::askUserToAccepUnknownDigest(
    const zypp::Pathname & file,
    const std::string & name )
{
    std::ostringstream json;
    json << R"({"type":"elicitation","method":"accept_unknown_digest","data":{)"
         << R"("file":")" << jsonEscape( file.asString() ) << "\","
         << R"("digest":")" << jsonEscape( name ) << "\"}}";

    _t.writeFrame( json.str() );
    auto ans = _t.readFrame();
    return ans ? parseBoolAnswer( *ans ) : false;
}

bool McpDigestReceive::askUserToAcceptWrongDigest(
    const zypp::Pathname & file,
    const std::string & requested,
    const std::string & found )
{
    std::ostringstream json;
    json << R"({"type":"elicitation","method":"accept_wrong_digest","data":{)"
         << R"("file":")" << jsonEscape( file.asString() ) << "\","
         << R"("expected":")" << jsonEscape( requested ) << "\","
         << R"("actual":")" << jsonEscape( found ) << "\"}}";

    _t.writeFrame( json.str() );
    auto ans = _t.readFrame();
    return ans ? parseBoolAnswer( *ans ) : false;
}

// ─── McpInstallReceive ───────────────────────────────────────────────────────
void McpInstallReceive::start( zypp::Resolvable::constPtr resolvable )
{
    std::ostringstream json;
    json << R"({"type":"progress","action":"install","package":")"
         << jsonEscape( resolvable->name() ) << "\","
         << R"("edition":")" << resolvable->edition().asString() << "\","
         << R"("percent":0})";
    _t.writeFrame( json.str() );
}

bool McpInstallReceive::progress( int value, zypp::Resolvable::constPtr resolvable )
{
    std::ostringstream json;
    json << R"({"type":"progress","action":"install","package":")"
         << jsonEscape( resolvable->name() ) << "\","
         << R"("percent":)" << std::max( 0, std::min( 100, value ) ) << "}";
    _t.writeFrame( json.str() );
    return true; // never abort from progress
}

void McpInstallReceive::finish( zypp::Resolvable::constPtr, Error error, const std::string &, RpmLevel )
{
    std::ostringstream json;
    json << R"({"type":"progress","action":"install","finished":true,"error":)"
         << ( error != NO_ERROR ? "true" : "false" ) << "}";
    _t.writeFrame( json.str() );
}

// ─── McpRemoveReceive ────────────────────────────────────────────────────────
void McpRemoveReceive::start( zypp::Resolvable::constPtr resolvable )
{
    std::ostringstream json;
    json << R"({"type":"progress","action":"remove","package":")"
         << jsonEscape( resolvable->name() ) << "\","
         << R"("edition":")" << resolvable->edition().asString() << "\","
         << R"("percent":0})";
    _t.writeFrame( json.str() );
}

bool McpRemoveReceive::progress( int value, zypp::Resolvable::constPtr resolvable )
{
    std::ostringstream json;
    json << R"({"type":"progress","action":"remove","package":")"
         << jsonEscape( resolvable->name() ) << "\","
         << R"("percent":)" << std::max( 0, std::min( 100, value ) ) << "}";
    _t.writeFrame( json.str() );
    return true;
}

void McpRemoveReceive::finish( zypp::Resolvable::constPtr, Error error, const std::string & )
{
    std::ostringstream json;
    json << R"({"type":"progress","action":"remove","finished":true,"error":)"
         << ( error != NO_ERROR ? "true" : "false" ) << "}";
    _t.writeFrame( json.str() );
}

// ─── McpCallbackScope ────────────────────────────────────────────────────────
McpCallbackScope::McpCallbackScope( McpTransport & t )
    : _keyring( t ), _digest( t ), _install( t ), _remove( t )
{
    _keyring.connect();
    _digest.connect();
    _install.connect();
    _remove.connect();
}

McpCallbackScope::~McpCallbackScope()
{
    _keyring.disconnect();
    _digest.disconnect();
    _install.disconnect();
    _remove.disconnect();
}
