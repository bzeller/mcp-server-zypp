#include "callbacks.h"
#include "context.h"
#include "cancellation.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <sstream>
#include <string>
#include <vector>
#include <zypp-core/base/String.h>
#include <zypp-core/base/InputStream>
#include <zypp-core/base/Logger.h>
#include <zypp-core/parser/json.h>
#include <zypp-core/parser/json/JsonValue.h>
#include <zypp/sat/Solvable.h>

#undef  ZYPP_BASE_LOGGER_LOGGROUP
#define ZYPP_BASE_LOGGER_LOGGROUP "zypp-mcp-tool"

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
    /// Extract a contentRpmout "line" value from an SA (SingleTrans) report.
    /// TargetImpl.cc stores it as a plain std::string for this content type
    /// (sendRpmLineToReport). Returns false if absent or of an unexpected
    /// type. NOT the same encoding as the classic reports below — see
    /// classicRpmOutLine() for why two accessors exist.
    bool rpmOutLine( const zypp::callback::UserData & userData, std::string & out )
    {
        return userData.haskey( "line" ) && userData.get( "line", out );
    }

    /// Extract a contentRpmout "line" value from a classic (non-SA) report.
    /// InstallResolvableReport/RemoveResolvableReport encode it as a
    /// reference_wrapper (RpmDb.cc: std::cref(line)) — unlike the SA
    /// reports' plain std::string above, a real, documented difference
    /// (ZYppCallbacks.h) between the two encodings, not an arbitrary
    /// choice here. Mirrors McpSingleTransReceive::report()'s handling of
    /// contentLogline, which uses the same reference_wrapper encoding.
    bool classicRpmOutLine( const zypp::callback::UserData & userData, std::string & out )
    {
        static const std::string empty;
        std::reference_wrapper<const std::string> lineRef( empty );
        if ( !userData.get( "line", lineRef ) )
            return false;
        out = lineRef.get();
        return true;
    }

    /// Mirrors zypper's classic-mode scriptlet-failure detection
    /// (src/callbacks/rpm.h: regex `^(warning|error): %.* scriptlet
    /// failed, `) — the only signal available for a non-fatal %post/
    /// %posttrans failure in classic mode, which has no dedicated script
    /// report at all (unlike SingleTrans's CommitScriptReportSA). A plain
    /// prefix + substring check rather than constructing a std::regex on
    /// every output line.
    bool looksLikeScriptletFailure( const std::string & line )
    {
        return ( zypp::str::hasPrefix( line, "warning: " ) || zypp::str::hasPrefix( line, "error: " ) )
            && line.find( " scriptlet failed, " ) != std::string::npos;
    }

    /// The package a classic contentRpmout line belongs to. RpmDb.cc's
    /// output loop runs for exactly one package at a time (rpm installs/
    /// removes are never parallelized — a hard rpm limitation, not a
    /// libzypp choice), and TargetCallbackReceiver.cc's
    /// RpmInstallPackageReceiver::report()/RpmRemovePackageReceiver::report()
    /// both inject a "solvable" key into every UserData that doesn't
    /// already carry one before forwarding it — so this is always
    /// available, no per-receiver "current package" member needed the way
    /// the SA receivers need one (their contentRpmout carries no such key).
    std::string classicRpmOutPackage( const zypp::callback::UserData & userData )
    {
        zypp::sat::Solvable solvable;
        if ( userData.get( "solvable", solvable ) && solvable )
            return solvable.name(); // matches resolvableName() elsewhere — plain name, not NVRA
        return std::string();
    }

    /// Maximum output lines buffered per step before older ones are
    /// dropped — see pushPending()/flushPending() below. Much smaller than
    /// libzypp's own MAXRPMMESSAGELINES (10000, RpmDb.cc/TargetImpl.cc):
    /// that buffer is written to an append-only local history log, this
    /// one exists only to be replayed into a single MCP error frame if the
    /// step it belongs to turns out to have failed.
    constexpr std::size_t kMaxPendingLines = 100;

    /// Buffer one contentRpmout line for the step currently in progress,
    /// without yet deciding whether it is worth keeping (see
    /// flushPending()). Bounded the same way CommitFailureLog bounds
    /// itself: oldest dropped first, so the tail — nearest the eventual
    /// error, if any — survives.
    void pushPending( std::vector<std::string> & pending, std::string line )
    {
        if ( pending.size() >= kMaxPendingLines )
            pending.erase( pending.begin() );
        pending.push_back( std::move(line) );
    }

    /// Record every buffered line as Detail context, then clear — call
    /// only once the step this buffer belongs to is known to have failed.
    /// On a successful finish(), callers clear pending directly instead
    /// (discarding routine output from a package that installed cleanly),
    /// which is exactly why this class does not record Detail entries
    /// unconditionally at report() time: on the common, successful path,
    /// none of this output is ever worth keeping at all.
    void flushPending( CommitFailureLog & log, const std::string & package,
                       CommitPhase phase, std::vector<std::string> & pending )
    {
        for ( auto & line : pending )
            log.record( package, phase, CommitSeverity::Detail, std::move(line) );
        pending.clear();
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

    /// Truncate a description/line for a log entry. Text from
    /// Exception::asUserHistory() in particular can be multi-paragraph;
    /// a short prefix is enough to identify the failure in a log scan
    /// without flooding the logfile — the full text is still delivered to
    /// the MCP client via CommitFailureLog/the error frame, this is only
    /// about what belongs in ZYPP_LOGFILE.
    std::string logSnippet( const std::string & text )
    {
        constexpr std::size_t kMaxLen = 120;
        return text.size() > kMaxLen ? text.substr( 0, kMaxLen ) + "..." : text;
    }
}

// ─── McpKeyRingReceive ───────────────────────────────────────────────────────
zypp::KeyRingReport::KeyTrust McpKeyRingReceive::askUserToAcceptKey(
    const zypp::PublicKey & key,
    const zypp::KeyContext & ctx )
{
    McpTransport & t    = _ctx.transport();
    GpgKeyGate   & gate = _ctx.gpgKeys();

    const std::string repo = ctx.empty() ? std::string() : ctx.repoInfo().asUserString();
    if ( gate.isAccepted( key.fingerprint() ) )
    {
        DBG << "KeyRing: fingerprint " << key.fingerprint() << " pre-approved, trusting" << std::endl;
        return zypp::KeyRingReport::KEY_TRUST_AND_IMPORT;
    }

    if ( skipElicitation() )
    {
        WAR << "KeyRing: skipping elicitation (cancelled), recording rejection of "
            << key.fingerprint() << std::endl;
        gate.recordRejection( key.fingerprint(), key.name(), repo );
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

    DBG << "KeyRing: eliciting trust for fingerprint " << key.fingerprint() << std::endl;
    t.writeFrame( zypp::json::Object{ {
        { "type",   "elicitation" },
        { "method", "trust_key"   },
        { "data",   std::move(data) }
    } }.asJSON() );

    auto ans = t.readFrame();
    const std::string answer = ans ? parseAnswer( *ans ) : std::string();

    if ( answer == "import" || answer == "trust" )
    {
        MIL << "KeyRing: fingerprint " << key.fingerprint() << " trusted (answer="
            << answer << ")" << std::endl;
        return answer == "import" ? zypp::KeyRingReport::KEY_TRUST_AND_IMPORT
                                   : zypp::KeyRingReport::KEY_TRUST_TEMPORARILY;
    }

    // Declined, EOF, or a client without elicitation support (the proxy
    // answers "decline" in that case) — deny, and record so the tool can
    // report which key blocked the transaction.
    WAR << "KeyRing: fingerprint " << key.fingerprint() << " NOT trusted, recording rejection" << std::endl;
    gate.recordRejection( key.fingerprint(), key.name(), repo );
    return zypp::KeyRingReport::KEY_DONT_TRUST;
}

bool McpKeyRingReceive::askUserToAcceptUnsignedFile(
    const std::string & file,
    const zypp::KeyContext & ctx )
{
    if ( skipElicitation() )
        return false;

    McpTransport & t = _ctx.transport();

    zypp::json::Object data = { { "file", file } };
    if ( !ctx.empty() )
        data.add( "repo", ctx.repoInfo().asUserString() );

    t.writeFrame( zypp::json::Object{ {
        { "type",   "elicitation"          },
        { "method", "accept_unsigned_file" },
        { "data",   std::move(data)        }
    } }.asJSON() );

    auto ans = t.readFrame();
    return ans ? parseBoolAnswer( *ans ) : false;
}

bool McpKeyRingReceive::askUserToAcceptUnknownKey(
    const std::string & file,
    const std::string & id,
    const zypp::KeyContext & ctx )
{
    if ( skipElicitation() )
        return false;

    McpTransport & t = _ctx.transport();

    zypp::json::Object data = {
        { "file",  file },
        { "keyid", id   }
    };
    if ( !ctx.empty() )
        data.add( "repo", ctx.repoInfo().asUserString() );

    t.writeFrame( zypp::json::Object{ {
        { "type",   "elicitation"        },
        { "method", "accept_unknown_key" },
        { "data",   std::move(data)      }
    } }.asJSON() );

    auto ans = t.readFrame();
    return ans ? parseBoolAnswer( *ans ) : false;
}

bool McpKeyRingReceive::askUserToAcceptVerificationFailed(
    const std::string & file,
    const zypp::PublicKey & key,
    const zypp::KeyContext & ctx )
{
    McpTransport & t    = _ctx.transport();
    GpgKeyGate   & gate = _ctx.gpgKeys();

    const std::string repo = ctx.empty() ? std::string() : ctx.repoInfo().asUserString();
    if ( skipElicitation() )
    {
        WAR << "KeyRing: skipping verification-failed elicitation (cancelled), recording "
               "rejection of " << key.fingerprint() << std::endl;
        gate.recordRejection( key.fingerprint(), key.name(), repo );
        return false;
    }

    zypp::json::Object data = {
        { "file",        file             },
        { "fingerprint", key.fingerprint()},
        { "name",        key.name()       }
    };
    if ( !repo.empty() )
        data.add( "repo", repo );

    t.writeFrame( zypp::json::Object{ {
        { "type",   "elicitation"                 },
        { "method", "accept_verification_failed"  },
        { "data",   std::move(data)                }
    } }.asJSON() );

    auto ans = t.readFrame();
    const bool accepted = ans ? parseBoolAnswer( *ans ) : false;
    if ( !accepted )
    {
        ERR << "KeyRing: signature verification failed for file=" << file
            << " fingerprint=" << key.fingerprint() << std::endl;
        gate.recordRejection( key.fingerprint(), key.name(), repo );
    }
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

    McpTransport & t    = _ctx.transport();
    GpgKeyGate   & gate = _ctx.gpgKeys();

    zypp::PublicKey  key;
    zypp::KeyContext ctx;
    userData.get( "PublicKey",  key );
    userData.get( "KeyContext", ctx );

    const std::string repo = ctx.empty() ? std::string() : ctx.repoInfo().asUserString();

    if ( gate.isAccepted( key.fingerprint() ) )
    {
        DBG << "KeyRing: package key " << key.fingerprint() << " pre-approved" << std::endl;
        userData.set( "TrustKey", true );
        return;
    }

    if ( skipElicitation() )
    {
        WAR << "KeyRing: skipping package key elicitation (cancelled), recording rejection of "
            << key.fingerprint() << std::endl;
        gate.recordRejection( key.fingerprint(), key.name(), repo );
        userData.set( "TrustKey", false );
        return;
    }

    zypp::json::Object data = {
        { "fingerprint", key.fingerprint() },
        { "name",        key.name()        }
    };
    if ( !repo.empty() )
        data.add( "repo", repo );

    t.writeFrame( zypp::json::Object{ {
        { "type",   "elicitation"       },
        { "method", "trust_package_key" },
        { "data",   std::move(data)     }
    } }.asJSON() );

    auto ans = t.readFrame();
    const bool trust = ans ? parseBoolAnswer( *ans ) : false;

    if ( trust )
    {
        MIL << "KeyRing: package key " << key.fingerprint() << " trusted" << std::endl;
    }
    else
    {
        WAR << "KeyRing: package key " << key.fingerprint() << " NOT trusted" << std::endl;
        gate.recordRejection( key.fingerprint(), key.name(), repo );
    }

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

    McpTransport & t = _ctx.transport();
    t.writeFrame( zypp::json::Object{ {
        { "type",   "elicitation"       },
        { "method", "accept_no_digest"  },
        { "data",   zypp::json::Object{ { { "file", file.asString() } } } }
    } }.asJSON() );

    auto ans = t.readFrame();
    return ans ? parseBoolAnswer( *ans ) : false;
}

bool McpDigestReceive::askUserToAccepUnknownDigest(
    const zypp::Pathname & file,
    const std::string & name )
{
    if ( skipElicitation() )
        return false;

    McpTransport & t = _ctx.transport();
    t.writeFrame( zypp::json::Object{ {
        { "type",   "elicitation"            },
        { "method", "accept_unknown_digest"  },
        { "data",   zypp::json::Object{ {
            { "file",   file.asString() },
            { "digest", name             }
        } } }
    } }.asJSON() );

    auto ans = t.readFrame();
    return ans ? parseBoolAnswer( *ans ) : false;
}

bool McpDigestReceive::askUserToAcceptWrongDigest(
    const zypp::Pathname & file,
    const std::string & requested,
    const std::string & found )
{
    if ( skipElicitation() )
        return false;

    McpTransport & t = _ctx.transport();
    t.writeFrame( zypp::json::Object{ {
        { "type",   "elicitation"          },
        { "method", "accept_wrong_digest"  },
        { "data",   zypp::json::Object{ {
            { "file",     file.asString() },
            { "expected", requested        },
            { "actual",   found            }
        } } }
    } }.asJSON() );

    auto ans = t.readFrame();
    return ans ? parseBoolAnswer( *ans ) : false;
}

// ─── McpInstallReceive ───────────────────────────────────────────────────────
void McpInstallReceive::start( zypp::Resolvable::constPtr resolvable )
{
    writeStepStart( _ctx.transport(), "install", resolvableName( resolvable ), resolvableEdition( resolvable ) );
}

bool McpInstallReceive::progress( int value, zypp::Resolvable::constPtr resolvable )
{
    writeStepProgress( _ctx.transport(), "install", resolvableName( resolvable ), value );
    return true; // never abort from progress
}

zypp::target::rpm::InstallResolvableReport::Action
McpInstallReceive::problem( zypp::Resolvable::constPtr resolvable, Error /*error*/,
                            const std::string & description, RpmLevel /*level*/ )
{
    ERR << "Install: problem for package=" << resolvableName( resolvable ) << ": "
        << logSnippet( description ) << std::endl;
    _ctx.failures().record( resolvableName( resolvable ), CommitPhase::Install,
                            CommitSeverity::Error, description );
    return ABORT; // base class default, unchanged — see callbacks.h
}

void McpInstallReceive::finish( zypp::Resolvable::constPtr resolvable, Error error, const std::string & reason, RpmLevel )
{
    const std::string pkg = resolvableName( resolvable );
    if ( error != NO_ERROR )
    {
        WAR << "Install: step failed for package=" << pkg << std::endl;
        if ( !reason.empty() )
            _ctx.failures().record( pkg, CommitPhase::Install, CommitSeverity::Error, reason );
        flushPending( _ctx.failures(), pkg, CommitPhase::Install, _pending );
    }
    else
    {
        _pending.clear(); // routine output from a package that installed cleanly — discard
    }
    writeStepFinish( _ctx.transport(), "install", pkg, error != NO_ERROR );
}

void McpInstallReceive::report( const UserData & userData )
{
    std::string line;
    if ( userData.type() != ReportType::contentRpmout || !classicRpmOutLine( userData, line ) )
        return;

    // classicRpmOutPackage() reads the "solvable" key TargetCallbackReceiver.cc
    // injects into every contentRpmout UserData — reliable because rpm
    // installs are never parallelized, so there is exactly one package
    // "in progress" for the whole lifetime of this line's report() call.
    const std::string pkg = classicRpmOutPackage( userData );
    if ( looksLikeScriptletFailure( line ) )
    {
        WAR << "Install: scriptlet failure detected for package=" << pkg << ": "
            << logSnippet( line ) << std::endl;
        _ctx.failures().record( pkg, CommitPhase::Install, CommitSeverity::Warning, line );
    }
    else
    {
        pushPending( _pending, line );
    }
}

// ─── McpRemoveReceive ────────────────────────────────────────────────────────
void McpRemoveReceive::start( zypp::Resolvable::constPtr resolvable )
{
    writeStepStart( _ctx.transport(), "remove", resolvableName( resolvable ), resolvableEdition( resolvable ) );
}

bool McpRemoveReceive::progress( int value, zypp::Resolvable::constPtr resolvable )
{
    writeStepProgress( _ctx.transport(), "remove", resolvableName( resolvable ), value );
    return true;
}

zypp::target::rpm::RemoveResolvableReport::Action
McpRemoveReceive::problem( zypp::Resolvable::constPtr resolvable, Error /*error*/,
                          const std::string & description )
{
    ERR << "Remove: problem for package=" << resolvableName( resolvable ) << ": "
        << logSnippet( description ) << std::endl;
    _ctx.failures().record( resolvableName( resolvable ), CommitPhase::Remove,
                            CommitSeverity::Error, description );
    return ABORT; // base class default, unchanged — see callbacks.h
}

void McpRemoveReceive::finish( zypp::Resolvable::constPtr resolvable, Error error, const std::string & reason )
{
    const std::string pkg = resolvableName( resolvable );
    if ( error != NO_ERROR )
    {
        WAR << "Remove: step failed for package=" << pkg << std::endl;
        if ( !reason.empty() )
            _ctx.failures().record( pkg, CommitPhase::Remove, CommitSeverity::Error, reason );
        flushPending( _ctx.failures(), pkg, CommitPhase::Remove, _pending );
    }
    else
    {
        _pending.clear();
    }
    writeStepFinish( _ctx.transport(), "remove", pkg, error != NO_ERROR );
}

void McpRemoveReceive::report( const UserData & userData )
{
    std::string line;
    if ( userData.type() != ReportType::contentRpmout || !classicRpmOutLine( userData, line ) )
        return;

    // See McpInstallReceive::report() — same rationale for classicRpmOutPackage().
    const std::string pkg = classicRpmOutPackage( userData );
    if ( looksLikeScriptletFailure( line ) )
    {
        WAR << "Remove: scriptlet failure detected for package=" << pkg << ": "
            << logSnippet( line ) << std::endl;
        _ctx.failures().record( pkg, CommitPhase::Remove, CommitSeverity::Warning, line );
    }
    else
    {
        pushPending( _pending, line );
    }
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

    // dbg/msg are routine noise and not recorded at all. war is exactly
    // the level libzypp uses for a non-fatal %posttrans scriptlet failure
    // (RpmPostTransCollector.cc) — the SingleTrans analogue of classic
    // mode's looksLikeScriptletFailure() pattern match, except here
    // libzypp already tells us the severity directly via loglevel, no
    // pattern matching needed. No package is attributable at this level;
    // this report is transaction-wide.
    if ( level == ReportType::loglevel::war )
        _ctx.failures().record( "", CommitPhase::Transaction, CommitSeverity::Warning, lineRef.get() );
    else if ( level == ReportType::loglevel::err || level == ReportType::loglevel::crt )
        _ctx.failures().record( "", CommitPhase::Transaction, CommitSeverity::Error, lineRef.get() );

    _ctx.transport().writeFrame( zypp::json::Object{ {
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
    writeStepStart( _ctx.transport(), "install", _package );
}

void McpInstallSAReceive::progress( int value, zypp::Resolvable::constPtr resolvable, const UserData & )
{
    writeStepProgress( _ctx.transport(), "install", stepPackage( resolvable, _package ), value );
}

void McpInstallSAReceive::finish( zypp::Resolvable::constPtr resolvable, Error error, const UserData & )
{
    const std::string pkg = stepPackage( resolvable, _package );
    if ( error != NO_ERROR )
    {
        WAR << "InstallSA: step failed for package=" << pkg << std::endl;
        flushPending( _ctx.failures(), pkg, CommitPhase::Install, _pending );
    }
    else
    {
        _pending.clear(); // routine output from a package that installed cleanly — discard
    }
    writeStepFinish( _ctx.transport(), "install", pkg, error != NO_ERROR );
    _package.clear();
}

void McpInstallSAReceive::report( const UserData & userData )
{
    std::string line;
    if ( userData.type() != ReportType::contentRpmout || !rpmOutLine( userData, line ) )
        return;

    writeRpmOutput( _ctx.transport(), "install", _package, line );

    if ( looksLikeScriptletFailure( line ) )
    {
        WAR << "InstallSA: scriptlet failure detected for package=" << _package << ": "
            << logSnippet( line ) << std::endl;
        _ctx.failures().record( _package, CommitPhase::Install, CommitSeverity::Warning, line );
    }
    else
    {
        pushPending( _pending, line );
    }
}

void McpInstallSAReceive::reportend()
{
    _package.clear();
    _pending.clear();
}

// ─── McpRemoveSAReceive ──────────────────────────────────────────────────────
void McpRemoveSAReceive::start( zypp::Resolvable::constPtr resolvable, const UserData & )
{
    _package = resolvableName( resolvable );
    writeStepStart( _ctx.transport(), "remove", _package );
}

void McpRemoveSAReceive::progress( int value, zypp::Resolvable::constPtr resolvable, const UserData & )
{
    writeStepProgress( _ctx.transport(), "remove", stepPackage( resolvable, _package ), value );
}

void McpRemoveSAReceive::finish( zypp::Resolvable::constPtr resolvable, Error error, const UserData & )
{
    const std::string pkg = stepPackage( resolvable, _package );
    if ( error != NO_ERROR )
    {
        WAR << "RemoveSA: step failed for package=" << pkg << std::endl;
        flushPending( _ctx.failures(), pkg, CommitPhase::Remove, _pending );
    }
    else
    {
        _pending.clear();
    }
    writeStepFinish( _ctx.transport(), "remove", pkg, error != NO_ERROR );
    _package.clear();
}

void McpRemoveSAReceive::report( const UserData & userData )
{
    std::string line;
    if ( userData.type() != ReportType::contentRpmout || !rpmOutLine( userData, line ) )
        return;

    writeRpmOutput( _ctx.transport(), "remove", _package, line );

    if ( looksLikeScriptletFailure( line ) )
    {
        WAR << "RemoveSA: scriptlet failure detected for package=" << _package << ": "
            << logSnippet( line ) << std::endl;
        _ctx.failures().record( _package, CommitPhase::Remove, CommitSeverity::Warning, line );
    }
    else
    {
        pushPending( _pending, line );
    }
}

void McpRemoveSAReceive::reportend()
{
    _package.clear();
    _pending.clear();
}

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
    _ctx.transport().writeFrame( frame.asJSON() );
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
    _ctx.transport().writeFrame( frame.asJSON() );
}

void McpCommitScriptSAReceive::finish( zypp::Resolvable::constPtr, Error error, const UserData & )
{
    // Unlike the other SA reports this has three outcomes: a WARN script
    // failure is non-fatal (the package still installed), CRITICAL means it
    // prevented installation — surface which, rather than collapsing both
    // into a bare error flag. Buffered output is only worth keeping as
    // context for a genuine (CRITICAL) failure — the WARN case's own
    // scriptlet-failure line was already recorded immediately by report()
    // below via looksLikeScriptletFailure(), so nothing here would add
    // anything beyond routine noise.
    const char * severity = error == NO_ERROR ? "none"
                          : error == CRITICAL ? "critical"
                                              : "warning";

    if ( error == CRITICAL )
    {
        ERR << "Script: " << _scriptType << " CRITICAL for package=" << _package << std::endl;
        _ctx.failures().record( _package, CommitPhase::Script, CommitSeverity::Error,
            std::string( _scriptType ) + " script " + severity );
        flushPending( _ctx.failures(), _package, CommitPhase::Script, _pending );
    }
    else
    {
        if ( error == WARN )
        {
            WAR << "Script: " << _scriptType << " WARN for package=" << _package << std::endl;
            _ctx.failures().record( _package, CommitPhase::Script, CommitSeverity::Warning,
                std::string( _scriptType ) + " script " + severity );
        }
        _pending.clear();
    }

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
    _ctx.transport().writeFrame( frame.asJSON() );

    _scriptType.clear();
    _package.clear();
}

void McpCommitScriptSAReceive::report( const UserData & userData )
{
    std::string line;
    if ( userData.type() != ReportType::contentRpmout || !rpmOutLine( userData, line ) )
        return;

    writeRpmOutput( _ctx.transport(), "script", _package, line );

    if ( looksLikeScriptletFailure( line ) )
    {
        WAR << "Script: scriptlet failure detected for package=" << _package << ": "
            << logSnippet( line ) << std::endl;
        _ctx.failures().record( _package, CommitPhase::Script, CommitSeverity::Warning, line );
    }
    else
    {
        pushPending( _pending, line );
    }
}

void McpCommitScriptSAReceive::reportend()
{
    _scriptType.clear();
    _package.clear();
    _pending.clear();
}

// ─── McpTransactionSAReceive ─────────────────────────────────────────────────
void McpTransactionSAReceive::start( const std::string & name, const UserData & )
{
    _name = name;
    _ctx.transport().writeFrame( zypp::json::Object{ {
        { "type",    "progress"   },
        { "action",  "transaction" },
        { "name",    _name         },
        { "percent", std::int32_t(0) }
    } }.asJSON() );
}

void McpTransactionSAReceive::progress( int value, const UserData & )
{
    _ctx.transport().writeFrame( zypp::json::Object{ {
        { "type",    "progress"   },
        { "action",  "transaction" },
        { "name",    _name         },
        { "percent", std::int32_t( std::max( 0, std::min( 100, value ) ) ) }
    } }.asJSON() );
}

void McpTransactionSAReceive::finish( Error error, const UserData & )
{
    if ( error != NO_ERROR )
    {
        ERR << "TransactionSA: phase=" << _name << " failed" << std::endl;
        _ctx.failures().record( "", CommitPhase::Transaction, CommitSeverity::Error, _name + " failed" );
        flushPending( _ctx.failures(), "", CommitPhase::Transaction, _pending );
    }
    else
    {
        _pending.clear();
    }

    _ctx.transport().writeFrame( zypp::json::Object{ {
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
    if ( userData.type() != ReportType::contentRpmout || !rpmOutLine( userData, line ) )
        return;

    zypp::json::Object frame = {
        { "type",       "progress"   },
        { "action",     "transaction" },
        { "rpm_output", line          }
    };
    if ( !_name.empty() )
        frame.add( "name", _name );
    _ctx.transport().writeFrame( frame.asJSON() );

    if ( looksLikeScriptletFailure( line ) )
    {
        WAR << "TransactionSA: scriptlet failure detected in phase=" << _name << ": "
            << logSnippet( line ) << std::endl;
        _ctx.failures().record( "", CommitPhase::Transaction, CommitSeverity::Warning, line );
    }
    else
    {
        pushPending( _pending, line );
    }
}

void McpTransactionSAReceive::reportend()
{
    _name.clear();
    _pending.clear();
}

// ─── McpCleanupSAReceive ─────────────────────────────────────────────────────
void McpCleanupSAReceive::start( const std::string & nvra, const UserData & )
{
    _nvra = nvra;
    writeStepStart( _ctx.transport(), "cleanup", _nvra );
}

void McpCleanupSAReceive::progress( int value, const UserData & )
{
    writeStepProgress( _ctx.transport(), "cleanup", _nvra, value );
}

void McpCleanupSAReceive::finish( Error error, const UserData & )
{
    if ( error != NO_ERROR )
    {
        WAR << "CleanupSA: step failed for " << _nvra << std::endl;
        flushPending( _ctx.failures(), _nvra, CommitPhase::Cleanup, _pending );
    }
    else
    {
        _pending.clear();
    }
    writeStepFinish( _ctx.transport(), "cleanup", _nvra, error != NO_ERROR );
    _nvra.clear();
}

void McpCleanupSAReceive::report( const UserData & userData )
{
    std::string line;
    if ( userData.type() != ReportType::contentRpmout || !rpmOutLine( userData, line ) )
        return;

    writeRpmOutput( _ctx.transport(), "cleanup", _nvra, line );

    if ( looksLikeScriptletFailure( line ) )
    {
        WAR << "CleanupSA: scriptlet failure detected for " << _nvra << ": "
            << logSnippet( line ) << std::endl;
        _ctx.failures().record( _nvra, CommitPhase::Cleanup, CommitSeverity::Warning, line );
    }
    else
    {
        pushPending( _pending, line );
    }
}

void McpCleanupSAReceive::reportend()
{
    _nvra.clear();
    _pending.clear();
}

// ─── McpDownloadReceive ──────────────────────────────────────────────────────
void McpDownloadReceive::infoInCache( zypp::Resolvable::constPtr resolvable, const zypp::Pathname & )
{
    _ctx.transport().writeFrame( zypp::json::Object{ {
        { "type",    "progress" },
        { "action",  "download" },
        { "package", resolvableName( resolvable ) },
        { "cached",  true              }
    } }.asJSON() );
}

void McpDownloadReceive::start( zypp::Resolvable::constPtr resolvable, const zypp::Url & )
{
    writeStepStart( _ctx.transport(), "download", resolvableName( resolvable ), resolvableEdition( resolvable ) );
}

bool McpDownloadReceive::progress( int value, zypp::Resolvable::constPtr resolvable )
{
    writeStepProgress( _ctx.transport(), "download", resolvableName( resolvable ), value );

    // Returning false throws AbortRequestException out of the current
    // download (zypp/repo/PackageProvider.cc) — but only honor that while
    // shouldAbortNow() holds; see its definition above for why this
    // callback cannot assume it is always safe to abort here (DownloadAsNeeded
    // mode fires this same callback interleaved with already-applied
    // install steps, strictly after commit_active).
    return !shouldAbortNow();
}

zypp::repo::DownloadResolvableReport::Action
McpDownloadReceive::problem( zypp::Resolvable::constPtr resolvable, Error /*error*/,
                            const std::string & description )
{
    // description here is the richest failure text libzypp produces for a
    // download failure — built from Exception::asUserHistory() at the
    // throw site (see PackageProvider.cc) — and is otherwise dropped
    // entirely (the base implementation discards it). Must always return
    // ABORT unchanged: the return value genuinely drives libzypp's control
    // flow here (RETRY/IGNORE/ABORT), unlike a pure sink.
    ERR << "Download: problem for package=" << resolvableName( resolvable ) << ": "
        << logSnippet( description ) << std::endl;
    _ctx.failures().record( resolvableName( resolvable ), CommitPhase::Download,
                            CommitSeverity::Error, description );
    return ABORT;
}

void McpDownloadReceive::finish( zypp::Resolvable::constPtr resolvable, Error error, const std::string & reason )
{
    if ( error != NO_ERROR && !reason.empty() )
    {
        WAR << "Download: finished with error, package=" << resolvableName( resolvable )
            << ": " << logSnippet( reason ) << std::endl;
        _ctx.failures().record( resolvableName( resolvable ), CommitPhase::Download,
                                CommitSeverity::Error, reason );
    }
    writeStepFinish( _ctx.transport(), "download", resolvableName( resolvable ), error != NO_ERROR );
}

// ─── McpCommitPreloadReceive ─────────────────────────────────────────────────
void McpCommitPreloadReceive::start( const zypp::callback::UserData & )
{
    _ctx.transport().writeFrame( zypp::json::Object{ {
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

    _ctx.transport().writeFrame( frame.asJSON() );

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

    _ctx.transport().writeFrame( frame.asJSON() );
}

void McpCommitPreloadReceive::finish( Result res, const zypp::callback::UserData & )
{
    if ( res == MISS )
    {
        WAR << "Preload: some packages could not be provided" << std::endl;
        _ctx.failures().record( "", CommitPhase::Preload, CommitSeverity::Error,
                                "some packages could not be provided" );
    }

    _ctx.transport().writeFrame( zypp::json::Object{ {
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
    McpTransport & t = _ctx.transport();

    // Announce the point of no return and BLOCK until the proxy has
    // definitely applied its non-cancellable latch before returning. See
    // ZYppCallbacks.h: CommitActiveReport for the full race-condition
    // rationale — without this synchronous handshake, the proxy could still
    // kill() between writing this frame and processing it.
    t.writeFrame( zypp::json::Object{ {
        { "type",  "zypp_control"  },
        { "event", "commit_active" }
    } }.asJSON() );

    // Block for the proxy's ack. Unlike the elicitation/digest handshakes,
    // a missing or malformed reply here means "do not proceed" rather than
    // "fail closed to a safe default" — there is no safe default once the
    // caller might proceed into an irreversible RPM transaction, so we
    // require an explicit, well-formed {"ack":true}.
    auto ans = t.readFrame();
    if ( !ans )
    {
        WAR << "CommitActive: proxy died or pipe broken, aborting before transaction" << std::endl;
        return false; // proxy died / pipe broken — do not proceed uncontrolled
    }

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
        {
            pastPointOfNoReturn() = true;
            MIL << "CommitActive: proceeding into RPM transaction" << std::endl;
        }
        else
        {
            WAR << "CommitActive: ack declined, aborting before transaction" << std::endl;
        }
        return proceed;
    }
    catch ( const std::exception & )
    {
        WAR << "CommitActive: malformed ack, aborting before transaction" << std::endl;
        return false; // malformed ack — fail closed
    }
}

void McpCommitActiveReceive::reportend()
{
    MIL << "CommitActive: transaction finished" << std::endl;
    _ctx.transport().writeFrame( zypp::json::Object{ {
        { "type",  "zypp_control"    },
        { "event", "commit_finished" }
    } }.asJSON() );
}

// ─── McpCallbackScope ────────────────────────────────────────────────────────
McpCallbackScope::McpCallbackScope( ToolContext & ctx )
    : _keyring( ctx ), _digest( ctx ), _install( ctx ), _remove( ctx ),
      _download( ctx ), _preload( ctx ), _commitActive( ctx ),
      _singleTrans( ctx ), _installSA( ctx ), _removeSA( ctx ),
      _scriptSA( ctx ), _transactionSA( ctx ), _cleanupSA( ctx )
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
