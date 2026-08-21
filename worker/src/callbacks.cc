#include "callbacks.h"
#include "cancellation.h"

#include <algorithm>
#include <functional>
#include <sstream>
#include <string>
#include <zypp-core/base/String.h>
#include <zypp-core/base/InputStream>
#include <zypp-core/parser/json.h>
#include <zypp-core/parser/json/JsonValue.h>

// ─── Answer parsing ──────────────────────────────────────────────────────────
// Uses the real JSON parser rather than substring scanning — correct on any
// valid JSON the proxy sends, not just the exact shape we expect.
namespace
{
    /// Set once McpCommitActiveReceive::start() has confirmed the RPM
    /// transaction is actually proceeding — the "point of no return" the
    /// registry's tool descriptions (main.cc) already promise. A
    /// function-local static, not a namespace-scope global, to avoid
    /// static-initialization-order concerns entirely — mirrors
    /// cancellation.h's cancellationRequested() shape. Deliberately a plain
    /// bool, not signal-safe state: set and read entirely from ordinary
    /// callback code running on this process's single thread, unlike
    /// cancellation.h's sig_atomic_t-backed flag, which specifically has to
    /// survive being written from a signal handler.
    ///
    /// Why this exists at all: DownloadResolvableReport/CommitPreloadReport
    /// (McpDownloadReceive/McpCommitPreloadReceive below) are not
    /// guaranteed to fire only before commit_active. In
    /// TargetImpl::commit()'s DownloadAsNeeded mode, the whole
    /// pre-commit_active preload block is skipped entirely, and the *same*
    /// per-package download callback fires later instead, interleaved with
    /// already-applied install steps — i.e. strictly after commit_active.
    /// Honoring cancellation there would abort a partially-applied
    /// transaction, not a clean no-op — the opposite of what the
    /// registry's "cannot be interrupted" promise requires. Gating on this
    /// explicit flag (rather than assuming which download mode is in play,
    /// or that this callback is confined to the preload phase) keeps that
    /// promise regardless of which internal libzypp path a given commit()
    /// call happens to take.
    bool & pastPointOfNoReturn()
    {
        static bool flag = false;
        return flag;
    }

    /// True only when a download/preload progress callback should actually
    /// abort right now: cancellation was requested (SIGTERM) AND the point
    /// of no return has not been crossed yet. Not a capability query — it
    /// reflects an actual decision, not merely whether aborting would be
    /// possible.
    bool shouldAbortNow()
    {
        return mcp::cancellationRequested() && !pastPointOfNoReturn();
    }

    /// True when an elicitation must be skipped rather than sent: the exact
    /// same condition as shouldAbortNow(), reused under a name that reads
    /// correctly at these call sites. Skipping is deny-equivalent, never an
    /// approval shortcut — the caller applies the identical fail-closed
    /// outcome a genuine decline would produce, it just avoids the round
    /// trip and (critically) the indefinite block on McpTransport::readFrame()
    /// (no timeout by design — see transport.h) that a SIGTERM cannot
    /// interrupt (our handler uses SA_RESTART deliberately, see
    /// cancellation.cc). Past the point of no return this is always false,
    /// same as shouldAbortNow() — trust/security decisions are never
    /// short-circuited once the transaction may have started, only
    /// discretionary cancellation is.
    bool skipElicitation()
    {
        return shouldAbortNow();
    }

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

// ─── Progress-frame helpers ───────────────────────────────────────────────────
// Shared between the classic per-package receivers (McpInstallReceive,
// McpRemoveReceive, McpDownloadReceive) and the SingleTrans-backend
// receivers further below (McpInstallSAReceive and friends) — libzypp picks
// exactly one of the two backends per commit (ZYppCommitPolicy::
// singleTransModeEnabled(), see callbacks.h), so both families need to
// produce the same frame shapes for a client to see one consistent stream
// regardless of which one actually ran.
namespace
{
    /// Extract a contentRpmout "line" value. TargetImpl.cc stores it as a
    /// plain std::string for this content type (sendRpmLineToReport), unlike
    /// contentLogline which stores a reference_wrapper — hence two different
    /// accessors are needed (see McpSingleTransReceive::report() below for
    /// the other one). Returns false if absent or of an unexpected type.
    bool rpmOutLine( const zypp::callback::UserData & userData, std::string & out )
    {
        return userData.haskey( "line" ) && userData.get( "line", out );
    }

    /// Emit an {"action": <action>, "rpm_output": <line>} progress frame,
    /// optionally attributed to a package. SingleTrans-only — the classic
    /// reports have no raw-output channel at all.
    void writeRpmOutput( McpTransport & t,
                         const char * action,
                         const std::string & package,
                         const std::string & line )
    {
        zypp::json::Object frame = {
            { "type",       "progress" },
            { "action",     std::string( action ) },
            { "rpm_output", line }
        };
        if ( !package.empty() )
            frame.add( "package", package );
        t.writeFrame( frame.asJSON() );
    }

    /// Shared shape for install/remove/cleanup start frames. edition is a
    /// separate parameter rather than folded into package because
    /// cleanup's start() has no edition at all (its nvra string already
    /// encodes it), so callers without one simply omit the argument.
    void writeStepStart( McpTransport & t, const char * action,
                         const std::string & package, const std::string & edition = std::string() )
    {
        zypp::json::Object frame = {
            { "type",    "progress" },
            { "action",  std::string( action ) },
            { "percent", std::int32_t(0) }
        };
        if ( !package.empty() )
            frame.add( "package", package );
        if ( !edition.empty() )
            frame.add( "edition", edition );
        t.writeFrame( frame.asJSON() );
    }

    /// Shared shape for install/remove/download/cleanup/preload progress
    /// frames.
    void writeStepProgress( McpTransport & t, const char * action,
                            const std::string & package, int value )
    {
        zypp::json::Object frame = {
            { "type",    "progress" },
            { "action",  std::string( action ) },
            { "percent", std::int32_t( std::max( 0, std::min( 100, value ) ) ) }
        };
        if ( !package.empty() )
            frame.add( "package", package );
        t.writeFrame( frame.asJSON() );
    }

    /// Shared shape for install/remove/download/cleanup finish frames.
    /// package is included unconditionally where known — McpInstallReceive/
    /// McpRemoveReceive's finish() previously omitted it despite libzypp
    /// passing the resolvable in; that was an incidental omission (the
    /// parameter was simply unused), not a deliberate difference from
    /// McpDownloadReceive::finish(), which already included it.
    void writeStepFinish( McpTransport & t, const char * action,
                          const std::string & package, bool error )
    {
        zypp::json::Object frame = {
            { "type",     "progress" },
            { "action",   std::string( action ) },
            { "finished", true },
            { "error",    error }
        };
        if ( !package.empty() )
            frame.add( "package", package );
        t.writeFrame( frame.asJSON() );
    }

    /// Human-readable name for a resolvable, matching the classic receivers'
    /// "package" field. Guards against a null Resolvable, which
    /// CommitScriptReportSA explicitly permits — and which nothing in the
    /// report interfaces rules out for the others either, since every one
    /// of them takes a plain Resolvable::constPtr with no documented
    /// non-null guarantee.
    std::string resolvableName( const zypp::Resolvable::constPtr & r )
    {
        return r ? r->name() : std::string();
    }

    /// Edition string for a resolvable, with the same null guard as
    /// resolvableName(). Callers must not dereference the pointer directly:
    /// writeStepStart() omits an empty edition, so a null resolvable
    /// degrades to a frame without the field rather than a crash.
    std::string resolvableEdition( const zypp::Resolvable::constPtr & r )
    {
        return r ? r->edition().asString() : std::string();
    }

    /// Package name to attribute a step frame to: prefer the resolvable
    /// libzypp passed for this specific callback, falling back to the name
    /// cached at start(). The SA reports pass a resolvable to every
    /// callback, but nothing guarantees it is non-null on the later ones —
    /// without the fallback a progress/finish frame could silently lose the
    /// package its own start() frame carried.
    std::string stepPackage( const zypp::Resolvable::constPtr & r, const std::string & cached )
    {
        std::string name = resolvableName( r );
        return name.empty() ? cached : name;
    }
}

// ─── McpKeyRingReceive ───────────────────────────────────────────────────────
zypp::KeyRingReport::KeyTrust McpKeyRingReceive::askUserToAcceptKey(
    const zypp::PublicKey & key,
    const zypp::KeyContext & ctx )
{
    const std::string repo = ctx.empty() ? std::string() : ctx.repoInfo().asUserString();
    if ( _gate.isAccepted( key.fingerprint() ) )
        return zypp::KeyRingReport::KEY_TRUST_AND_IMPORT;

    if ( skipElicitation() )
    {
        _gate.recordRejection( key.fingerprint(), key.name(), repo );
        return zypp::KeyRingReport::KEY_DONT_TRUST;
    }

    zypp::json::Object data = {
        { "fingerprint", key.fingerprint()      },
        { "name",        key.name()             },
        { "created",     key.created().asString() },
        { "expires",     key.expiresAsString()  }
    };
    if ( !repo.empty() )
        data.add( "repo", repo );

    _t.writeFrame( zypp::json::Object{ {
        { "type",   "elicitation" },
        { "method", "trust_key"   },
        { "data",   std::move(data) }
    } }.asJSON() );

    auto ans = _t.readFrame();
    const std::string answer = ans ? parseAnswer( *ans ) : std::string();

    if ( answer == "import" )
        return zypp::KeyRingReport::KEY_TRUST_AND_IMPORT;
    if ( answer == "trust" )
        return zypp::KeyRingReport::KEY_TRUST_TEMPORARILY;

    // Declined, EOF, or a client without elicitation support (the proxy
    // answers "decline" in that case) — deny, and record so the tool can
    // report which key blocked the transaction.
    _gate.recordRejection( key.fingerprint(), key.name(), repo );
    return zypp::KeyRingReport::KEY_DONT_TRUST;
}

bool McpKeyRingReceive::askUserToAcceptUnsignedFile(
    const std::string & file,
    const zypp::KeyContext & ctx )
{
    if ( skipElicitation() )
        return false;

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
    if ( skipElicitation() )
        return false;

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
    const std::string repo = ctx.empty() ? std::string() : ctx.repoInfo().asUserString();
    if ( skipElicitation() )
    {
        _gate.recordRejection( key.fingerprint(), key.name(), repo );
        return false;
    }

    zypp::json::Object data = {
        { "file",        file             },
        { "fingerprint", key.fingerprint()},
        { "name",        key.name()       }
    };
    if ( !repo.empty() )
        data.add( "repo", repo );

    _t.writeFrame( zypp::json::Object{ {
        { "type",   "elicitation"                 },
        { "method", "accept_verification_failed"  },
        { "data",   std::move(data)                }
    } }.asJSON() );

    auto ans = _t.readFrame();
    const bool accepted = ans ? parseBoolAnswer( *ans ) : false;
    if ( !accepted )
        _gate.recordRejection( key.fingerprint(), key.name(), repo );
    return accepted;
}

// KeyRing::askUserToAcceptPackageKey() (per-package signing key trust, asked
// during commit for every distinct key) is non-virtual and communicates
// solely through this generic report() with an
// ACCEPT_PACKAGE_KEY_REQUEST-typed UserData — overriding report() is the only
// available interception point.
void McpKeyRingReceive::report( const UserData & userData )
{
    if ( userData.type() != zypp::ContentType( zypp::KeyRingReport::ACCEPT_PACKAGE_KEY_REQUEST ) )
        return; // not a request this override answers — leave to the base default

    zypp::PublicKey  key;
    zypp::KeyContext ctx;
    userData.get( "PublicKey",  key );
    userData.get( "KeyContext", ctx );

    const std::string repo = ctx.empty() ? std::string() : ctx.repoInfo().asUserString();

    if ( _gate.isAccepted( key.fingerprint() ) )
    {
        userData.set( "TrustKey", true );
        return;
    }

    if ( skipElicitation() )
    {
        _gate.recordRejection( key.fingerprint(), key.name(), repo );
        userData.set( "TrustKey", false );
        return;
    }

    zypp::json::Object data = {
        { "fingerprint", key.fingerprint() },
        { "name",        key.name()        }
    };
    if ( !repo.empty() )
        data.add( "repo", repo );

    _t.writeFrame( zypp::json::Object{ {
        { "type",   "elicitation"       },
        { "method", "trust_package_key" },
        { "data",   std::move(data)     }
    } }.asJSON() );

    auto ans = _t.readFrame();
    const bool trust = ans ? parseBoolAnswer( *ans ) : false;

    if ( !trust )
        _gate.recordRejection( key.fingerprint(), key.name(), repo );

    // TrustKey is unset at this point — UserData::set() on a const reference
    // is only permitted for a currently-empty value (see UserData.h), so this
    // always takes effect and is visible to the caller after report() returns
    // (same underlying shared map, not a copy).
    userData.set( "TrustKey", trust );
}

// ─── McpDigestReceive ────────────────────────────────────────────────────────
bool McpDigestReceive::askUserToAcceptNoDigest( const zypp::Pathname & file )
{
    if ( skipElicitation() )
        return false;

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
    if ( skipElicitation() )
        return false;

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
    if ( skipElicitation() )
        return false;

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
    writeStepStart( _t, "install", resolvableName( resolvable ), resolvableEdition( resolvable ) );
}

bool McpInstallReceive::progress( int value, zypp::Resolvable::constPtr resolvable )
{
    writeStepProgress( _t, "install", resolvableName( resolvable ), value );
    return true; // never abort from progress
}

void McpInstallReceive::finish( zypp::Resolvable::constPtr resolvable, Error error, const std::string &, RpmLevel )
{
    writeStepFinish( _t, "install", resolvableName( resolvable ), error != NO_ERROR );
}

// ─── McpRemoveReceive ────────────────────────────────────────────────────────
void McpRemoveReceive::start( zypp::Resolvable::constPtr resolvable )
{
    writeStepStart( _t, "remove", resolvableName( resolvable ), resolvableEdition( resolvable ) );
}

bool McpRemoveReceive::progress( int value, zypp::Resolvable::constPtr resolvable )
{
    writeStepProgress( _t, "remove", resolvableName( resolvable ), value );
    return true;
}

void McpRemoveReceive::finish( zypp::Resolvable::constPtr resolvable, Error error, const std::string & )
{
    writeStepFinish( _t, "remove", resolvableName( resolvable ), error != NO_ERROR );
}

// ─── SingleTrans-backend receivers ────────────────────────────────────────────
// See callbacks.h for why these exist alongside the classic receivers above.
// None can abort (all progress() return void) and all are reached only after
// the commit_active point of no return, so no shouldAbortNow() gating applies.

// ─── McpSingleTransReceive ───────────────────────────────────────────────────
void McpSingleTransReceive::report( const UserData & userData )
{
    if ( userData.type() != ReportType::contentLogline )
        return;

    // contentLogline stores the line as a reference_wrapper (see
    // TargetImpl.cc: SendSingleTransReport::sendLogline). Use the
    // non-throwing get() overload rather than get<T>(key), which throws
    // bad_AnyType_cast on a missing/mistyped key.
    static const std::string empty;
    std::reference_wrapper<const std::string> lineRef( empty );
    if ( !userData.get( "line", lineRef ) )
        return;

    ReportType::loglevel level = ReportType::loglevel::msg;
    userData.get( "level", level ); // leaves the default if absent

    const char * levelName = "info";
    switch ( level )
    {
        case ReportType::loglevel::dbg: levelName = "debug";    break;
        case ReportType::loglevel::msg: levelName = "info";     break;
        case ReportType::loglevel::war: levelName = "warning";  break;
        case ReportType::loglevel::err: levelName = "error";    break;
        case ReportType::loglevel::crt: levelName = "critical"; break;
    }

    _t.writeFrame( zypp::json::Object{ {
        { "type",   "progress"            },
        { "action", "rpm_log"             },
        { "level",  std::string(levelName) },
        { "line",   lineRef.get()          }
    } }.asJSON() );
}

// ─── McpInstallSAReceive ─────────────────────────────────────────────────────
void McpInstallSAReceive::start( zypp::Resolvable::constPtr resolvable, const UserData & )
{
    _package = resolvableName( resolvable );
    writeStepStart( _t, "install", _package );
}

void McpInstallSAReceive::progress( int value, zypp::Resolvable::constPtr resolvable, const UserData & )
{
    writeStepProgress( _t, "install", stepPackage( resolvable, _package ), value );
}

void McpInstallSAReceive::finish( zypp::Resolvable::constPtr resolvable, Error error, const UserData & )
{
    writeStepFinish( _t, "install", stepPackage( resolvable, _package ), error != NO_ERROR );
    _package.clear();
}

void McpInstallSAReceive::report( const UserData & userData )
{
    std::string line;
    if ( userData.type() == ReportType::contentRpmout && rpmOutLine( userData, line ) )
        writeRpmOutput( _t, "install", _package, line );
}

void McpInstallSAReceive::reportend()
{ _package.clear(); }

// ─── McpRemoveSAReceive ──────────────────────────────────────────────────────
void McpRemoveSAReceive::start( zypp::Resolvable::constPtr resolvable, const UserData & )
{
    _package = resolvableName( resolvable );
    writeStepStart( _t, "remove", _package );
}

void McpRemoveSAReceive::progress( int value, zypp::Resolvable::constPtr resolvable, const UserData & )
{
    writeStepProgress( _t, "remove", stepPackage( resolvable, _package ), value );
}

void McpRemoveSAReceive::finish( zypp::Resolvable::constPtr resolvable, Error error, const UserData & )
{
    writeStepFinish( _t, "remove", stepPackage( resolvable, _package ), error != NO_ERROR );
    _package.clear();
}

void McpRemoveSAReceive::report( const UserData & userData )
{
    std::string line;
    if ( userData.type() == ReportType::contentRpmout && rpmOutLine( userData, line ) )
        writeRpmOutput( _t, "remove", _package, line );
}

void McpRemoveSAReceive::reportend()
{ _package.clear(); }

// ─── McpCommitScriptSAReceive ────────────────────────────────────────────────
void McpCommitScriptSAReceive::start( const std::string & scriptType,
                                      const std::string & packageName,
                                      zypp::Resolvable::constPtr resolvable,
                                      const UserData & )
{
    _scriptType = scriptType;
    // Prefer the resolvable's name, fall back to the plain package name
    // string; both may be absent for transaction-wide scripts (posttrans).
    _package = resolvableName( resolvable );
    if ( _package.empty() )
        _package = packageName;

    zypp::json::Object frame = {
        { "type",        "progress" },
        { "action",      "script"   },
        { "script_type", _scriptType },
        { "percent",     std::int32_t(0) }
    };
    if ( !_package.empty() )
        frame.add( "package", _package );
    _t.writeFrame( frame.asJSON() );
}

void McpCommitScriptSAReceive::progress( int value, zypp::Resolvable::constPtr, const UserData & )
{
    zypp::json::Object frame = {
        { "type",        "progress" },
        { "action",      "script"   },
        { "script_type", _scriptType },
        { "percent",     std::int32_t( std::max( 0, std::min( 100, value ) ) ) }
    };
    if ( !_package.empty() )
        frame.add( "package", _package );
    _t.writeFrame( frame.asJSON() );
}

void McpCommitScriptSAReceive::finish( zypp::Resolvable::constPtr, Error error, const UserData & )
{
    // Unlike the other SA reports this has three outcomes: a WARN script
    // failure is non-fatal (the package still installed), CRITICAL means it
    // prevented installation — surface which, rather than collapsing both
    // into a bare error flag.
    const char * severity = error == NO_ERROR ? "none"
                          : error == CRITICAL ? "critical"
                                              : "warning";

    zypp::json::Object frame = {
        { "type",        "progress" },
        { "action",      "script"   },
        { "script_type", _scriptType },
        { "finished",    true       },
        { "error",       error != NO_ERROR },
        { "severity",    std::string(severity) }
    };
    if ( !_package.empty() )
        frame.add( "package", _package );
    _t.writeFrame( frame.asJSON() );

    _scriptType.clear();
    _package.clear();
}

void McpCommitScriptSAReceive::report( const UserData & userData )
{
    std::string line;
    if ( userData.type() == ReportType::contentRpmout && rpmOutLine( userData, line ) )
        writeRpmOutput( _t, "script", _package, line );
}

void McpCommitScriptSAReceive::reportend()
{
    _scriptType.clear();
    _package.clear();
}

// ─── McpTransactionSAReceive ─────────────────────────────────────────────────
void McpTransactionSAReceive::start( const std::string & name, const UserData & )
{
    _name = name;
    _t.writeFrame( zypp::json::Object{ {
        { "type",    "progress"   },
        { "action",  "transaction" },
        { "name",    _name         },
        { "percent", std::int32_t(0) }
    } }.asJSON() );
}

void McpTransactionSAReceive::progress( int value, const UserData & )
{
    _t.writeFrame( zypp::json::Object{ {
        { "type",    "progress"   },
        { "action",  "transaction" },
        { "name",    _name         },
        { "percent", std::int32_t( std::max( 0, std::min( 100, value ) ) ) }
    } }.asJSON() );
}

void McpTransactionSAReceive::finish( Error error, const UserData & )
{
    _t.writeFrame( zypp::json::Object{ {
        { "type",     "progress"   },
        { "action",   "transaction" },
        { "name",     _name         },
        { "finished", true          },
        { "error",    error != NO_ERROR }
    } }.asJSON() );
    _name.clear();
}

void McpTransactionSAReceive::report( const UserData & userData )
{
    std::string line;
    if ( userData.type() == ReportType::contentRpmout && rpmOutLine( userData, line ) )
    {
        zypp::json::Object frame = {
            { "type",       "progress"   },
            { "action",     "transaction" },
            { "rpm_output", line          }
        };
        if ( !_name.empty() )
            frame.add( "name", _name );
        _t.writeFrame( frame.asJSON() );
    }
}

void McpTransactionSAReceive::reportend()
{ _name.clear(); }

// ─── McpCleanupSAReceive ─────────────────────────────────────────────────────
void McpCleanupSAReceive::start( const std::string & nvra, const UserData & )
{
    _nvra = nvra;
    writeStepStart( _t, "cleanup", _nvra );
}

void McpCleanupSAReceive::progress( int value, const UserData & )
{
    writeStepProgress( _t, "cleanup", _nvra, value );
}

void McpCleanupSAReceive::finish( Error error, const UserData & )
{
    writeStepFinish( _t, "cleanup", _nvra, error != NO_ERROR );
    _nvra.clear();
}

void McpCleanupSAReceive::report( const UserData & userData )
{
    std::string line;
    if ( userData.type() == ReportType::contentRpmout && rpmOutLine( userData, line ) )
        writeRpmOutput( _t, "cleanup", _nvra, line );
}

void McpCleanupSAReceive::reportend()
{ _nvra.clear(); }

// ─── McpDownloadReceive ──────────────────────────────────────────────────────
void McpDownloadReceive::infoInCache( zypp::Resolvable::constPtr resolvable, const zypp::Pathname & )
{
    _t.writeFrame( zypp::json::Object{ {
        { "type",    "progress" },
        { "action",  "download" },
        { "package", resolvableName( resolvable ) },
        { "cached",  true              }
    } }.asJSON() );
}

void McpDownloadReceive::start( zypp::Resolvable::constPtr resolvable, const zypp::Url & )
{
    writeStepStart( _t, "download", resolvableName( resolvable ), resolvableEdition( resolvable ) );
}

bool McpDownloadReceive::progress( int value, zypp::Resolvable::constPtr resolvable )
{
    writeStepProgress( _t, "download", resolvableName( resolvable ), value );

    // Returning false throws AbortRequestException out of the current
    // download (zypp/repo/PackageProvider.cc) — but only honor that while
    // shouldAbortNow() holds; see its definition above for why this
    // callback cannot assume it is always safe to abort here (DownloadAsNeeded
    // mode fires this same callback interleaved with already-applied
    // install steps, strictly after commit_active).
    return !shouldAbortNow();
}

void McpDownloadReceive::finish( zypp::Resolvable::constPtr resolvable, Error error, const std::string & )
{
    writeStepFinish( _t, "download", resolvableName( resolvable ), error != NO_ERROR );
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

    // Returning false marks the preload dispatcher's downloads as missed
    // and cancels it (commitpackagepreloader.cc) — only honor that while
    // shouldAbortNow() holds. This report only ever fires before
    // commit_active in practice (see TargetImpl::commit()'s
    // DownloadAsNeeded branch, which skips this whole preloader entirely),
    // but gate on the same explicit flag as McpDownloadReceive regardless,
    // rather than relying on that being true.
    return !shouldAbortNow();
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

// ─── McpCommitActiveReceive ──────────────────────────────────────────────────
/// Both hooks use "zypp_control", not "progress": CommitActiveReport carries
/// no domain data (no package name, no percent) — it is a pure lifecycle
/// marker for this worker's internal protocol, not a user-facing progress
/// update. "zypp_control" is namespaced separately from MCP's own types
/// since it is specific to zypp-mcp-tool's wire protocol. The proxy's frame
/// loop has no case for "zypp_control", so both frames fall through
/// untouched by any user-facing notification path. The proxy instead treats
/// the specific combination type=="zypp_control" && event=="commit_active"
/// as the sole, authoritative signal to latch cancellation off — no
/// separate generic field is needed since this is the only place that
/// signal is ever emitted.
bool McpCommitActiveReceive::start( const UserData & )
{
    // Announce the point of no return and BLOCK until the proxy has
    // definitely applied its non-cancellable latch before returning. See
    // ZYppCallbacks.h: CommitActiveReport for the full race-condition
    // rationale — without this synchronous handshake, the proxy could still
    // kill() between writing this frame and processing it.
    _t.writeFrame( zypp::json::Object{ {
        { "type",  "zypp_control"  },
        { "event", "commit_active" }
    } }.asJSON() );

    // Block for the proxy's ack. Unlike the elicitation/digest handshakes,
    // a missing or malformed reply here means "do not proceed" rather than
    // "fail closed to a safe default" — there is no safe default once the
    // caller might proceed into an irreversible RPM transaction, so we
    // require an explicit, well-formed {"ack":true}.
    auto ans = _t.readFrame();
    if ( !ans )
        return false; // proxy died / pipe broken — do not proceed uncontrolled

    try
    {
        std::istringstream ss( *ans );
        const auto val = zypp::json::parseDocument( zypp::InputStream(ss) );
        const auto & obj = val.asObject();
        const bool proceed = obj.contains( "ack" ) && bool( obj.value( "ack" ).asBool() );

        // Latch the point of no return only on genuine proceed — a decline
        // here means TargetImpl::commit() throws TargetAbortedException and
        // never enters the transaction, so cancellation must remain honored
        // (McpDownloadReceive/McpCommitPreloadReceive's shouldAbortNow()).
        if ( proceed )
            pastPointOfNoReturn() = true;
        return proceed;
    }
    catch ( const std::exception & )
    {
        return false; // malformed ack — fail closed
    }
}

void McpCommitActiveReceive::reportend()
{
    _t.writeFrame( zypp::json::Object{ {
        { "type",  "zypp_control"    },
        { "event", "commit_finished" }
    } }.asJSON() );
}

// ─── McpCallbackScope ────────────────────────────────────────────────────────
McpCallbackScope::McpCallbackScope( McpTransport & t, GpgKeyGate & gpgKeys )
    : _keyring( t, gpgKeys ), _digest( t ), _install( t ), _remove( t ),
      _download( t ), _preload( t ), _commitActive( t ),
      _singleTrans( t ), _installSA( t ), _removeSA( t ),
      _scriptSA( t ), _transactionSA( t ), _cleanupSA( t )
{
    _keyring.connect();
    _digest.connect();
    _install.connect();
    _remove.connect();
    _download.connect();
    _preload.connect();
    _commitActive.connect();

    // Connected as a set: any single one of these suppresses libzypp's
    // SingleTransReportLegacyWrapper, so connecting only some would silently
    // drop the rest (see callbacks.h).
    _singleTrans.connect();
    _installSA.connect();
    _removeSA.connect();
    _scriptSA.connect();
    _transactionSA.connect();
    _cleanupSA.connect();
}

McpCallbackScope::~McpCallbackScope()
{
    _keyring.disconnect();
    _digest.disconnect();
    _install.disconnect();
    _remove.disconnect();
    _download.disconnect();
    _preload.disconnect();
    _commitActive.disconnect();

    _singleTrans.disconnect();
    _installSA.disconnect();
    _removeSA.disconnect();
    _scriptSA.disconnect();
    _transactionSA.disconnect();
    _cleanupSA.disconnect();
}
