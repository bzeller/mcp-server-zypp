#include "callbacks.h"

#include <algorithm>
#include <sstream>
#include <zypp-core/base/String.h>
#include <zypp-core/base/InputStream>
#include <zypp-core/parser/json.h>
#include <zypp-core/parser/json/JsonValue.h>

// ─── Answer parsing ──────────────────────────────────────────────────────────
// Uses the real JSON parser rather than substring scanning — correct on any
// valid JSON the proxy sends, not just the exact shape we expect.
namespace
{
    /// Parse a {"answer": "..."} frame. Returns the value or empty on any
    /// parse failure or missing field — callers must treat empty as "decline".
    std::string parseAnswer( const std::string & frame )
    {
        try
        {
            std::istringstream ss( frame );
            const auto val = zypp::json::parseDocument( zypp::InputStream(ss) );
            const auto & obj = val.asObject();
            if ( !obj.contains( "answer" ) )
                return {};
            return std::string( obj.value( "answer" ).asString() );
        }
        catch ( const std::exception & )
        {
            return {}; // fail closed on malformed input
        }
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
    zypp::json::Object data = {
        { "fingerprint", key.fingerprint()      },
        { "name",        key.name()             },
        { "created",     key.created().asString() },
        { "expires",     key.expiresAsString()  }
    };
    if ( !ctx.empty() )
        data.add( "repo", ctx.repoInfo().asUserString() );

    _t.writeFrame( zypp::json::Object{ {
        { "type",   "elicitation" },
        { "method", "trust_key"   },
        { "data",   std::move(data) }
    } }.asJSON() );

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
    zypp::json::Object data = { { "file", file } };
    if ( !ctx.empty() )
        data.add( "repo", ctx.repoInfo().asUserString() );

    _t.writeFrame( zypp::json::Object{ {
        { "type",   "elicitation"          },
        { "method", "accept_unsigned_file" },
        { "data",   std::move(data)        }
    } }.asJSON() );

    auto ans = _t.readFrame();
    return ans ? parseBoolAnswer( *ans ) : false;
}

bool McpKeyRingReceive::askUserToAcceptUnknownKey(
    const std::string & file,
    const std::string & id,
    const zypp::KeyContext & ctx )
{
    zypp::json::Object data = {
        { "file",  file },
        { "keyid", id   }
    };
    if ( !ctx.empty() )
        data.add( "repo", ctx.repoInfo().asUserString() );

    _t.writeFrame( zypp::json::Object{ {
        { "type",   "elicitation"        },
        { "method", "accept_unknown_key" },
        { "data",   std::move(data)      }
    } }.asJSON() );

    auto ans = _t.readFrame();
    return ans ? parseBoolAnswer( *ans ) : false;
}

bool McpKeyRingReceive::askUserToAcceptVerificationFailed(
    const std::string & file,
    const zypp::PublicKey & key,
    const zypp::KeyContext & ctx )
{
    zypp::json::Object data = {
        { "file",        file             },
        { "fingerprint", key.fingerprint()},
        { "name",        key.name()       }
    };
    if ( !ctx.empty() )
        data.add( "repo", ctx.repoInfo().asUserString() );

    _t.writeFrame( zypp::json::Object{ {
        { "type",   "elicitation"                 },
        { "method", "accept_verification_failed"  },
        { "data",   std::move(data)                }
    } }.asJSON() );

    auto ans = _t.readFrame();
    return ans ? parseBoolAnswer( *ans ) : false;
}

// ─── McpDigestReceive ────────────────────────────────────────────────────────
bool McpDigestReceive::askUserToAcceptNoDigest( const zypp::Pathname & file )
{
    _t.writeFrame( zypp::json::Object{ {
        { "type",   "elicitation"       },
        { "method", "accept_no_digest"  },
        { "data",   zypp::json::Object{ { { "file", file.asString() } } } }
    } }.asJSON() );

    auto ans = _t.readFrame();
    return ans ? parseBoolAnswer( *ans ) : false;
}

bool McpDigestReceive::askUserToAccepUnknownDigest(
    const zypp::Pathname & file,
    const std::string & name )
{
    _t.writeFrame( zypp::json::Object{ {
        { "type",   "elicitation"            },
        { "method", "accept_unknown_digest"  },
        { "data",   zypp::json::Object{ {
            { "file",   file.asString() },
            { "digest", name             }
        } } }
    } }.asJSON() );

    auto ans = _t.readFrame();
    return ans ? parseBoolAnswer( *ans ) : false;
}

bool McpDigestReceive::askUserToAcceptWrongDigest(
    const zypp::Pathname & file,
    const std::string & requested,
    const std::string & found )
{
    _t.writeFrame( zypp::json::Object{ {
        { "type",   "elicitation"          },
        { "method", "accept_wrong_digest"  },
        { "data",   zypp::json::Object{ {
            { "file",     file.asString() },
            { "expected", requested        },
            { "actual",   found            }
        } } }
    } }.asJSON() );

    auto ans = _t.readFrame();
    return ans ? parseBoolAnswer( *ans ) : false;
}

// ─── McpInstallReceive ───────────────────────────────────────────────────────
void McpInstallReceive::start( zypp::Resolvable::constPtr resolvable )
{
    _t.writeFrame( zypp::json::Object{ {
        { "type",    "progress" },
        { "action",  "install"  },
        { "package", resolvable->name()               },
        { "edition", resolvable->edition().asString()  },
        { "percent", std::int32_t(0)                   }
    } }.asJSON() );
}

bool McpInstallReceive::progress( int value, zypp::Resolvable::constPtr resolvable )
{
    _t.writeFrame( zypp::json::Object{ {
        { "type",    "progress" },
        { "action",  "install"  },
        { "package", resolvable->name() },
        { "percent", std::int32_t( std::max( 0, std::min( 100, value ) ) ) }
    } }.asJSON() );
    return true; // never abort from progress
}

void McpInstallReceive::finish( zypp::Resolvable::constPtr, Error error, const std::string &, RpmLevel )
{
    _t.writeFrame( zypp::json::Object{ {
        { "type",     "progress" },
        { "action",   "install"  },
        { "finished", true       },
        { "error",    error != NO_ERROR }
    } }.asJSON() );
}

// ─── McpRemoveReceive ────────────────────────────────────────────────────────
void McpRemoveReceive::start( zypp::Resolvable::constPtr resolvable )
{
    _t.writeFrame( zypp::json::Object{ {
        { "type",    "progress" },
        { "action",  "remove"   },
        { "package", resolvable->name()               },
        { "edition", resolvable->edition().asString()  },
        { "percent", std::int32_t(0)                   }
    } }.asJSON() );
}

bool McpRemoveReceive::progress( int value, zypp::Resolvable::constPtr resolvable )
{
    _t.writeFrame( zypp::json::Object{ {
        { "type",    "progress" },
        { "action",  "remove"   },
        { "package", resolvable->name() },
        { "percent", std::int32_t( std::max( 0, std::min( 100, value ) ) ) }
    } }.asJSON() );
    return true;
}

void McpRemoveReceive::finish( zypp::Resolvable::constPtr, Error error, const std::string & )
{
    _t.writeFrame( zypp::json::Object{ {
        { "type",     "progress" },
        { "action",   "remove"   },
        { "finished", true       },
        { "error",    error != NO_ERROR }
    } }.asJSON() );
}

// ─── McpDownloadReceive ──────────────────────────────────────────────────────
void McpDownloadReceive::infoInCache( zypp::Resolvable::constPtr resolvable, const zypp::Pathname & )
{
    _t.writeFrame( zypp::json::Object{ {
        { "type",    "progress" },
        { "action",  "download" },
        { "package", resolvable->name() },
        { "cached",  true              }
    } }.asJSON() );
}

void McpDownloadReceive::start( zypp::Resolvable::constPtr resolvable, const zypp::Url & )
{
    _t.writeFrame( zypp::json::Object{ {
        { "type",    "progress" },
        { "action",  "download" },
        { "package", resolvable->name()               },
        { "edition", resolvable->edition().asString()  },
        { "percent", std::int32_t(0)                   }
    } }.asJSON() );
}

bool McpDownloadReceive::progress( int value, zypp::Resolvable::constPtr resolvable )
{
    _t.writeFrame( zypp::json::Object{ {
        { "type",    "progress" },
        { "action",  "download" },
        { "package", resolvable->name() },
        { "percent", std::int32_t( std::max( 0, std::min( 100, value ) ) ) }
    } }.asJSON() );
    return true; // never abort from progress
}

void McpDownloadReceive::finish( zypp::Resolvable::constPtr resolvable, Error error, const std::string & )
{
    _t.writeFrame( zypp::json::Object{ {
        { "type",     "progress" },
        { "action",   "download" },
        { "package",  resolvable->name() },
        { "finished", true       },
        { "error",    error != NO_ERROR }
    } }.asJSON() );
}

// ─── McpCommitPreloadReceive ─────────────────────────────────────────────────
void McpCommitPreloadReceive::start( const zypp::callback::UserData & )
{
    _t.writeFrame( zypp::json::Object{ {
        { "type",    "progress" },
        { "action",  "preload"  },
        { "started", true       }
    } }.asJSON() );
}

bool McpCommitPreloadReceive::progress( int value, const zypp::callback::UserData & userData )
{
    zypp::json::Object frame = {
        { "type",    "progress" },
        { "action",  "preload"  },
        { "percent", std::int32_t( std::max( 0, std::min( 100, value ) ) ) }
    };

    // Documented optional fields (ZYppCallbacks.h: CommitPreloadReport::progress) —
    // only present when zypp actually knows them, so add conditionally.
    const double dbpsAvg = userData.get<double>( "dbps_avg", -1.0 );
    if ( dbpsAvg >= 0.0 )
        frame.add( "dbps_avg", dbpsAvg );

    const double dbpsCurrent = userData.get<double>( "dbps_current", -1.0 );
    if ( dbpsCurrent >= 0.0 )
        frame.add( "dbps_current", dbpsCurrent );

    const double bytesReceived = userData.get<double>( "bytesReceived", -1.0 );
    if ( bytesReceived >= 0.0 )
        frame.add( "bytes_received", bytesReceived );

    const double bytesRequired = userData.get<double>( "bytesRequired", -1.0 );
    if ( bytesRequired >= 0.0 )
        frame.add( "bytes_required", bytesRequired );

    _t.writeFrame( frame.asJSON() );
    return true; // never abort from progress
}

void McpCommitPreloadReceive::fileStart( const zypp::Pathname & localfile, const zypp::callback::UserData & userData )
{
    zypp::json::Object frame = {
        { "type",   "progress" },
        { "action", "preload"  },
        { "file",   localfile.asString() }
    };

    // Documented optional field (ZYppCallbacks.h: CommitPreloadReport::fileStart).
    const zypp::Url url = userData.get<zypp::Url>( "Url", zypp::Url() );
    if ( url.isValid() )
        frame.add( "url", url.asString() );

    _t.writeFrame( frame.asJSON() );
}

void McpCommitPreloadReceive::finish( Result res, const zypp::callback::UserData & )
{
    _t.writeFrame( zypp::json::Object{ {
        { "type",     "progress"            },
        { "action",   "preload"             },
        { "finished", true                  },
        { "error",    res != SUCCESS        }
    } }.asJSON() );
}

// ─── McpCallbackScope ────────────────────────────────────────────────────────
McpCallbackScope::McpCallbackScope( McpTransport & t )
    : _keyring( t ), _digest( t ), _install( t ), _remove( t ),
      _download( t ), _preload( t )
{
    _keyring.connect();
    _digest.connect();
    _install.connect();
    _remove.connect();
    _download.connect();
    _preload.connect();
}

McpCallbackScope::~McpCallbackScope()
{
    _keyring.disconnect();
    _digest.disconnect();
    _install.disconnect();
    _remove.disconnect();
    _download.disconnect();
    _preload.disconnect();
}
