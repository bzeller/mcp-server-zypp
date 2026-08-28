#ifndef MCP_SERVER_ZYPP_CALLBACKS_H
#define MCP_SERVER_ZYPP_CALLBACKS_H

#include <string>
#include <vector>

#include <zypp/ZYppCallbacks.h>
#include <zypp/KeyRing.h>
#include <zypp/Digest.h>
#include <zypp-core/parser/json/JsonValue.h>

// Forward-declared, not included: ToolContext (context.h) owns
// McpCallbackScope and includes this header, so a full include here would
// cycle. Every receiver below only stores a ToolContext& and dereferences it
// in callbacks.cc, which does include context.h — a forward declaration is
// sufficient for that.
class ToolContext;

// ─── Callback context access ─────────────────────────────────────────────────
// Every receiver below takes a ToolContext&, not the individual McpTransport/
// GpgKeyGate/(future CommitFailureLog etc.) references it happens to need
// today — with 13 receivers, a shared dependency added later (as
// CommitFailureLog will be) would otherwise mean touching every one of their
// constructors again.
//
// Off-limits from callback context: ToolContext::loadLiveSystem() and
// loadSystemFromArg(). Calling either from inside a callback would be
// reentrancy into libzypp mid-commit — nothing enforces this at compile
// time, it is a rule to observe, not a capability the type withholds.
//
// ─── KeyRing callback ────────────────────────────────────────────────────────
/// Mirrors zypper/src/callbacks/keyring.h — each virtual override emits an
/// elicitation frame and blocks on the proxy's stdin for the human answer.
/// Default on any transport failure: fail closed (KEY_DONT_TRUST / false).
///
/// report() is the exception: KeyRing::askUserToAcceptPackageKey() (the
/// per-package signing key trust decision made during commit) is non-virtual
/// and communicates solely through ReportBase::report()'s UserData —
/// overriding report() is the only available interception point. Rather than
/// blocking on an elicitation like the other members here, this answers
/// synchronously from the ToolContext-owned GpgKeyGate — see gpgkeygate.h.
struct McpKeyRingReceive : public zypp::callback::ReceiveReport<zypp::KeyRingReport>
{
    explicit McpKeyRingReceive( ToolContext & ctx ) : _ctx( ctx ) {}

    zypp::KeyRingReport::KeyTrust askUserToAcceptKey(
        const zypp::PublicKey & key,
        const zypp::KeyContext & ctx ) override;

    bool askUserToAcceptUnsignedFile(
        const std::string & file,
        const zypp::KeyContext & ctx ) override;

    bool askUserToAcceptUnknownKey(
        const std::string & file,
        const std::string & id,
        const zypp::KeyContext & ctx ) override;

    bool askUserToAcceptVerificationFailed(
        const std::string & file,
        const zypp::PublicKey & key,
        const zypp::KeyContext & ctx ) override;

    void report( const UserData & userData ) override;

private:
    ToolContext & _ctx;
};

// ─── Digest callback ─────────────────────────────────────────────────────────
struct McpDigestReceive : public zypp::callback::ReceiveReport<zypp::DigestReport>
{
    explicit McpDigestReceive( ToolContext & ctx ) : _ctx( ctx ) {}

    bool askUserToAcceptNoDigest( const zypp::Pathname & file ) override;
    bool askUserToAccepUnknownDigest( const zypp::Pathname & file,
                                      const std::string & name ) override;
    bool askUserToAcceptWrongDigest( const zypp::Pathname & file,
                                     const std::string & requested,
                                     const std::string & found ) override;

private:
    ToolContext & _ctx;
};

// ─── Install progress ────────────────────────────────────────────────────────
/// Emits MCP notifications/progress frames. Never aborts (returns true always).
struct McpInstallReceive
    : public zypp::callback::ReceiveReport<zypp::target::rpm::InstallResolvableReport>
{
    explicit McpInstallReceive( ToolContext & ctx ) : _ctx( ctx ) {}

    void start( zypp::Resolvable::constPtr resolvable ) override;
    bool progress( int value, zypp::Resolvable::constPtr resolvable ) override;

    /// Captures description into the commit failure log; must always
    /// return the base default (ABORT) unchanged — see
    /// McpDownloadReceive::problem() for why.
    Action problem( zypp::Resolvable::constPtr resolvable, Error error,
                    const std::string & description, RpmLevel level ) override;

    void finish( zypp::Resolvable::constPtr, Error, const std::string &, RpmLevel ) override;

    /// Classic mode has no dedicated script report (unlike SingleTrans's
    /// CommitScriptReportSA) — a non-fatal %post/%posttrans scriptlet
    /// failure is only visible as a specific line in contentRpmout. Lines
    /// matching that pattern are recorded as Warning immediately; all other
    /// lines are only buffered in _pending (see callbacks.cc:
    /// pushPending()/flushPending()) and recorded as Detail if finish()
    /// reports an error — routine output from a package that installed
    /// cleanly is not worth keeping, and would otherwise risk evicting an
    /// earlier package's actual failure once CommitFailureLog::kMaxEntries
    /// is hit.
    void report( const UserData & userData ) override;

private:
    ToolContext & _ctx;
    std::vector<std::string> _pending;   ///< buffered contentRpmout, this package only
};

// ─── Remove progress ─────────────────────────────────────────────────────────
struct McpRemoveReceive
    : public zypp::callback::ReceiveReport<zypp::target::rpm::RemoveResolvableReport>
{
    explicit McpRemoveReceive( ToolContext & ctx ) : _ctx( ctx ) {}

    void start( zypp::Resolvable::constPtr resolvable ) override;
    bool progress( int value, zypp::Resolvable::constPtr resolvable ) override;

    /// Captures description into the commit failure log; must always
    /// return the base default (ABORT) unchanged — see
    /// McpDownloadReceive::problem() for why.
    Action problem( zypp::Resolvable::constPtr resolvable, Error error,
                    const std::string & description ) override;

    void finish( zypp::Resolvable::constPtr, Error, const std::string & ) override;

    /// See McpInstallReceive::report() — same rationale.
    void report( const UserData & userData ) override;

private:
    ToolContext & _ctx;
    std::vector<std::string> _pending;
};

// ─── Single-transaction (SingleTrans) reports ────────────────────────────────
/// libzypp runs a commit through one of two backends: the classic
/// per-package loop, or — when ZYppCommitPolicy::singleTransModeEnabled()
/// (see ZYppCommitPolicy.cc; the default on several build flavors) — a
/// single rpm transaction driven by the zypp-rpm helper. The two emit
/// *different* report families: the classic one uses
/// Install/RemoveResolvableReport (McpInstallReceive/McpRemoveReceive
/// above), the SingleTrans one uses the *ReportSA family below plus the
/// transaction-wide SingleTransReport.
///
/// Both must be handled. If nothing listens to the SA reports, libzypp
/// activates SingleTransReportLegacyWrapper (TargetImpl.cc) which bridges
/// only install/remove start/progress/finish onto the classic reports and
/// logs a warning — script execution, cleanup, transaction phases
/// (verify/prepare) and all raw rpm output are silently lost. Connecting
/// any one of the SA receivers suppresses that wrapper
/// (singleTransReportsConnected()), so they are connected as a set.
///
/// None of these can abort: every progress() returns void, unlike the
/// classic reports. They are also reached only after the commit_active
/// point of no return, so no cancellation gating applies (see
/// callbacks.cc: shouldAbortNow()).

/// Transaction-wide report. Only carries contentLogline — the raw rpm log
/// stream for the whole transaction, not tied to any one step.
struct McpSingleTransReceive
    : public zypp::callback::ReceiveReport<zypp::target::rpm::SingleTransReport>
{
    explicit McpSingleTransReceive( ToolContext & ctx ) : _ctx( ctx ) {}

    void report( const UserData & userData ) override;

private:
    ToolContext & _ctx;
};

/// Per-package install during a single transaction — the SingleTrans
/// counterpart of McpInstallReceive. Emits the same "action":"install"
/// frames so consumers see one consistent stream regardless of backend,
/// plus "rpm_output" lines via report()/contentRpmout.
struct McpInstallSAReceive
    : public zypp::callback::ReceiveReport<zypp::target::rpm::InstallResolvableReportSA>
{
    explicit McpInstallSAReceive( ToolContext & ctx ) : _ctx( ctx ) {}

    void start( zypp::Resolvable::constPtr resolvable, const UserData & ) override;
    void progress( int value, zypp::Resolvable::constPtr resolvable, const UserData & ) override;
    void finish( zypp::Resolvable::constPtr resolvable, Error error, const UserData & ) override;
    void report( const UserData & userData ) override;
    void reportend() override;

private:
    ToolContext & _ctx;
    std::string   _package;   ///< current package, to attribute rpm output
    std::vector<std::string> _pending;   ///< buffered contentRpmout, see McpInstallReceive::report()
};

/// Per-package removal during a single transaction — SingleTrans
/// counterpart of McpRemoveReceive, emitting "action":"remove".
struct McpRemoveSAReceive
    : public zypp::callback::ReceiveReport<zypp::target::rpm::RemoveResolvableReportSA>
{
    explicit McpRemoveSAReceive( ToolContext & ctx ) : _ctx( ctx ) {}

    void start( zypp::Resolvable::constPtr resolvable, const UserData & ) override;
    void progress( int value, zypp::Resolvable::constPtr resolvable, const UserData & ) override;
    void finish( zypp::Resolvable::constPtr resolvable, Error error, const UserData & ) override;
    void report( const UserData & userData ) override;
    void reportend() override;

private:
    ToolContext & _ctx;
    std::string   _package;
    std::vector<std::string> _pending;
};

/// rpm scriptlet execution (%post, %posttrans, ...). The resolvable may be
/// null and the package name empty — e.g. for transaction-wide posttrans
/// scripts. Emits "action":"script".
struct McpCommitScriptSAReceive
    : public zypp::callback::ReceiveReport<zypp::target::rpm::CommitScriptReportSA>
{
    explicit McpCommitScriptSAReceive( ToolContext & ctx ) : _ctx( ctx ) {}

    void start( const std::string & scriptType,
                const std::string & packageName,
                zypp::Resolvable::constPtr resolvable,
                const UserData & ) override;
    void progress( int value, zypp::Resolvable::constPtr resolvable, const UserData & ) override;
    void finish( zypp::Resolvable::constPtr resolvable, Error error, const UserData & ) override;
    void report( const UserData & userData ) override;
    void reportend() override;

private:
    ToolContext & _ctx;
    std::string   _scriptType;
    std::string   _package;
    std::vector<std::string> _pending;
};

/// Generic named transaction phase — libzypp uses this for rpm's own
/// "Verifying"/"Preparing" stages. Emits "action":"transaction".
struct McpTransactionSAReceive
    : public zypp::callback::ReceiveReport<zypp::target::rpm::TransactionReportSA>
{
    explicit McpTransactionSAReceive( ToolContext & ctx ) : _ctx( ctx ) {}

    void start( const std::string & name, const UserData & ) override;
    void progress( int value, const UserData & ) override;
    void finish( Error error, const UserData & ) override;
    void report( const UserData & userData ) override;
    void reportend() override;

private:
    ToolContext & _ctx;
    std::string   _name;
    std::vector<std::string> _pending;
};

/// Removal of the superseded version after an upgrade. Emits
/// "action":"cleanup"; identifies the package by NVRA string only (no
/// Resolvable is available at this point).
struct McpCleanupSAReceive
    : public zypp::callback::ReceiveReport<zypp::target::rpm::CleanupPackageReportSA>
{
    explicit McpCleanupSAReceive( ToolContext & ctx ) : _ctx( ctx ) {}

    void start( const std::string & nvra, const UserData & ) override;
    void progress( int value, const UserData & ) override;
    void finish( Error error, const UserData & ) override;
    void report( const UserData & userData ) override;
    void reportend() override;

private:
    ToolContext & _ctx;
    std::string   _nvra;
    std::vector<std::string> _pending;
};

// ─── Download progress ────────────────────────────────────────────────────────
/// Fires per-package during commit while zypp fetches packages from repos.
/// Never aborts (progress always returns true).
struct McpDownloadReceive
    : public zypp::callback::ReceiveReport<zypp::repo::DownloadResolvableReport>
{
    explicit McpDownloadReceive( ToolContext & ctx ) : _ctx( ctx ) {}

    void infoInCache( zypp::Resolvable::constPtr resolvable, const zypp::Pathname & localfile ) override;
    void start( zypp::Resolvable::constPtr resolvable, const zypp::Url & url ) override;
    bool progress( int value, zypp::Resolvable::constPtr resolvable ) override;
    void finish( zypp::Resolvable::constPtr resolvable, Error error, const std::string & reason ) override;

    /// Overridden purely to capture description — otherwise the richest
    /// available failure text (built from Exception::asUserHistory(), see
    /// PackageProvider.cc) is dropped on the floor; the base implementation
    /// discards it entirely. The return value genuinely matters to libzypp
    /// (PackageProvider.cc switches on it: RETRY/IGNORE/ABORT), so this must
    /// always return ABORT — the exact base-class default — unchanged.
    /// Capturing text must never alter control flow.
    Action problem( zypp::Resolvable::constPtr resolvable, Error error,
                    const std::string & description ) override;

private:
    ToolContext & _ctx;
};

// ─── Commit preload progress ──────────────────────────────────────────────────
/// Builds the JSON payload for one McpCommitPreloadReceive::progress() call.
/// Deliberately a free function taking only the same inputs progress()
/// itself is given, decoupled from ToolContext/McpTransport, specifically so
/// the deterministic bytesReceived/bytesRequired (libzypp's UserData key
/// spelling) -> bytes_received/bytes_required (this worker's JSON key
/// spelling) mapping can be unit-tested directly against a UserData the test
/// constructs itself — see worker/tests/preloadprogress_test.cc. Whether a
/// *real* download's UserData ever actually contains these keys is a
/// separate, genuinely non-deterministic question of network timing, and is
/// deliberately not asserted there or in e2e beyond "did progress() fire at
/// all" (see tests/e2e/scenarios/commit_failure.py).
zypp::json::Object preloadProgressFrame( int value, const zypp::callback::UserData & userData );

/// Fires for the overall concurrent preload of all commit downloads — a
/// single progress stream covering the whole batch, distinct from the
/// per-package McpDownloadReceive above. Never aborts (progress always
/// returns true).
struct McpCommitPreloadReceive
    : public zypp::callback::ReceiveReport<zypp::media::CommitPreloadReport>
{
    explicit McpCommitPreloadReceive( ToolContext & ctx ) : _ctx( ctx ) {}

    void start( const zypp::callback::UserData & userData = zypp::callback::UserData() ) override;
    bool progress( int value, const zypp::callback::UserData & userData = zypp::callback::UserData() ) override;
    void fileStart( const zypp::Pathname & localfile, const zypp::callback::UserData & userData = zypp::callback::UserData() ) override;
    void finish( Result res, const zypp::callback::UserData & userData = zypp::callback::UserData() ) override;

private:
    ToolContext & _ctx;
};

// ─── Commit-active barrier ───────────────────────────────────────────────────
/// Fires via CommitActiveReport's start() — called synchronously by
/// TargetImpl::commit (see ZYppCallbacks.h: CommitActiveReport for the full
/// rationale). This is the single, backend-agnostic point at which the RPM
/// transaction becomes irreversible.
///
/// start() blocks synchronously, writing a "zypp_control" frame
/// (event: "commit_active") and then waiting for the proxy's ack, before
/// returning. This guarantees the proxy has definitely applied its
/// non-cancellable latch before commit() proceeds into the actual
/// transaction — closing the race where a kill() could otherwise land
/// between the frame being written and the proxy processing it. This is
/// the sole, authoritative source of that latch — no separate generic
/// field is used, since no other callback emits this signal.
///
/// Returning false (on a missing/malformed/declining ack, or on EOF —
/// e.g. the proxy died) aborts the commit before anything has been
/// touched, rather than proceeding uncontrolled.
///
/// On a genuine proceed (true), start() also latches a process-local
/// "point of no return" flag (see callbacks.cc: pastPointOfNoReturn())
/// that McpDownloadReceive/McpCommitPreloadReceive's progress() check
/// before honoring a SIGTERM-driven cancellation — those callbacks are not
/// guaranteed to fire only before this point (see their definitions), so
/// this flag, not the proxy handshake alone, is what keeps the "cannot be
/// interrupted once the transaction begins" promise (main.cc's registry
/// descriptions) regardless of which download mode a given commit() uses.
struct McpCommitActiveReceive
    : public zypp::callback::ReceiveReport<zypp::target::CommitActiveReport>
{
    explicit McpCommitActiveReceive( ToolContext & ctx ) : _ctx( ctx ) {}

    bool start( const UserData & ) override;
    void reportend() override;

private:
    ToolContext & _ctx;
};

// ─── RAII scope — connect/disconnect all receivers ───────────────────────────
/// Owned by ToolContext (see context.h) as its final member, so receivers
/// are disconnected before any other ToolContext state is torn down.
/// Uniqueness, not ownership legitimacy, is the actual constraint here:
/// callback::DistributeReport<T>::instance() holds exactly one raw
/// Receiver* per report type, so at most one McpCallbackScope — and
/// therefore at most one ToolContext — may be alive at a time. ToolContext
/// is already non-copyable, and the worker constructs exactly one.
class McpCallbackScope
{
public:
    explicit McpCallbackScope( ToolContext & ctx );
    ~McpCallbackScope();

    McpCallbackScope( const McpCallbackScope & ) = delete;
    McpCallbackScope & operator=( const McpCallbackScope & ) = delete;

private:
    McpKeyRingReceive        _keyring;
    McpDigestReceive         _digest;
    McpInstallReceive        _install;
    McpRemoveReceive         _remove;
    McpDownloadReceive       _download;
    McpCommitPreloadReceive  _preload;
    McpCommitActiveReceive   _commitActive;
    // SingleTrans backend counterparts — connected as a set alongside the
    // classic receivers above, since which pair actually fires depends on
    // ZYppCommitPolicy::singleTransModeEnabled() at commit time.
    McpSingleTransReceive     _singleTrans;
    McpInstallSAReceive       _installSA;
    McpRemoveSAReceive        _removeSA;
    McpCommitScriptSAReceive  _scriptSA;
    McpTransactionSAReceive   _transactionSA;
    McpCleanupSAReceive       _cleanupSA;
};

#endif // MCP_SERVER_ZYPP_CALLBACKS_H
