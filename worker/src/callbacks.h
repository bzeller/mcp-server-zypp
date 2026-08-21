#ifndef MCP_SERVER_ZYPP_CALLBACKS_H
#define MCP_SERVER_ZYPP_CALLBACKS_H

#include <zypp/ZYppCallbacks.h>
#include <zypp/KeyRing.h>
#include <zypp/Digest.h>

#include "transport.h"
#include "gpgkeygate.h"

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
/// synchronously from the pre-supplied GpgKeyGate — see gpgkeygate.h.
struct McpKeyRingReceive : public zypp::callback::ReceiveReport<zypp::KeyRingReport>
{
    McpKeyRingReceive( McpTransport & t, GpgKeyGate & gate ) : _t( t ), _gate( gate ) {}

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
    McpTransport & _t;
    GpgKeyGate   & _gate;
};

// ─── Digest callback ─────────────────────────────────────────────────────────
struct McpDigestReceive : public zypp::callback::ReceiveReport<zypp::DigestReport>
{
    explicit McpDigestReceive( McpTransport & t ) : _t( t ) {}

    bool askUserToAcceptNoDigest( const zypp::Pathname & file ) override;
    bool askUserToAccepUnknownDigest( const zypp::Pathname & file,
                                      const std::string & name ) override;
    bool askUserToAcceptWrongDigest( const zypp::Pathname & file,
                                     const std::string & requested,
                                     const std::string & found ) override;

private:
    McpTransport & _t;
};

// ─── Install progress ────────────────────────────────────────────────────────
/// Emits MCP notifications/progress frames. Never aborts (returns true always).
struct McpInstallReceive
    : public zypp::callback::ReceiveReport<zypp::target::rpm::InstallResolvableReport>
{
    explicit McpInstallReceive( McpTransport & t ) : _t( t ) {}

    void start( zypp::Resolvable::constPtr resolvable ) override;
    bool progress( int value, zypp::Resolvable::constPtr resolvable ) override;
    void finish( zypp::Resolvable::constPtr, Error, const std::string &, RpmLevel ) override;

private:
    McpTransport & _t;
};

// ─── Remove progress ─────────────────────────────────────────────────────────
struct McpRemoveReceive
    : public zypp::callback::ReceiveReport<zypp::target::rpm::RemoveResolvableReport>
{
    explicit McpRemoveReceive( McpTransport & t ) : _t( t ) {}

    void start( zypp::Resolvable::constPtr resolvable ) override;
    bool progress( int value, zypp::Resolvable::constPtr resolvable ) override;
    void finish( zypp::Resolvable::constPtr, Error, const std::string & ) override;

private:
    McpTransport & _t;
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
    explicit McpSingleTransReceive( McpTransport & t ) : _t( t ) {}

    void report( const UserData & userData ) override;

private:
    McpTransport & _t;
};

/// Per-package install during a single transaction — the SingleTrans
/// counterpart of McpInstallReceive. Emits the same "action":"install"
/// frames so consumers see one consistent stream regardless of backend,
/// plus "rpm_output" lines via report()/contentRpmout.
struct McpInstallSAReceive
    : public zypp::callback::ReceiveReport<zypp::target::rpm::InstallResolvableReportSA>
{
    explicit McpInstallSAReceive( McpTransport & t ) : _t( t ) {}

    void start( zypp::Resolvable::constPtr resolvable, const UserData & ) override;
    void progress( int value, zypp::Resolvable::constPtr resolvable, const UserData & ) override;
    void finish( zypp::Resolvable::constPtr resolvable, Error error, const UserData & ) override;
    void report( const UserData & userData ) override;
    void reportend() override;

private:
    McpTransport & _t;
    std::string    _package;   ///< current package, to attribute rpm output
};

/// Per-package removal during a single transaction — SingleTrans
/// counterpart of McpRemoveReceive, emitting "action":"remove".
struct McpRemoveSAReceive
    : public zypp::callback::ReceiveReport<zypp::target::rpm::RemoveResolvableReportSA>
{
    explicit McpRemoveSAReceive( McpTransport & t ) : _t( t ) {}

    void start( zypp::Resolvable::constPtr resolvable, const UserData & ) override;
    void progress( int value, zypp::Resolvable::constPtr resolvable, const UserData & ) override;
    void finish( zypp::Resolvable::constPtr resolvable, Error error, const UserData & ) override;
    void report( const UserData & userData ) override;
    void reportend() override;

private:
    McpTransport & _t;
    std::string    _package;
};

/// rpm scriptlet execution (%post, %posttrans, ...). The resolvable may be
/// null and the package name empty — e.g. for transaction-wide posttrans
/// scripts. Emits "action":"script".
struct McpCommitScriptSAReceive
    : public zypp::callback::ReceiveReport<zypp::target::rpm::CommitScriptReportSA>
{
    explicit McpCommitScriptSAReceive( McpTransport & t ) : _t( t ) {}

    void start( const std::string & scriptType,
                const std::string & packageName,
                zypp::Resolvable::constPtr resolvable,
                const UserData & ) override;
    void progress( int value, zypp::Resolvable::constPtr resolvable, const UserData & ) override;
    void finish( zypp::Resolvable::constPtr resolvable, Error error, const UserData & ) override;
    void report( const UserData & userData ) override;
    void reportend() override;

private:
    McpTransport & _t;
    std::string    _scriptType;
    std::string    _package;
};

/// Generic named transaction phase — libzypp uses this for rpm's own
/// "Verifying"/"Preparing" stages. Emits "action":"transaction".
struct McpTransactionSAReceive
    : public zypp::callback::ReceiveReport<zypp::target::rpm::TransactionReportSA>
{
    explicit McpTransactionSAReceive( McpTransport & t ) : _t( t ) {}

    void start( const std::string & name, const UserData & ) override;
    void progress( int value, const UserData & ) override;
    void finish( Error error, const UserData & ) override;
    void report( const UserData & userData ) override;
    void reportend() override;

private:
    McpTransport & _t;
    std::string    _name;
};

/// Removal of the superseded version after an upgrade. Emits
/// "action":"cleanup"; identifies the package by NVRA string only (no
/// Resolvable is available at this point).
struct McpCleanupSAReceive
    : public zypp::callback::ReceiveReport<zypp::target::rpm::CleanupPackageReportSA>
{
    explicit McpCleanupSAReceive( McpTransport & t ) : _t( t ) {}

    void start( const std::string & nvra, const UserData & ) override;
    void progress( int value, const UserData & ) override;
    void finish( Error error, const UserData & ) override;
    void report( const UserData & userData ) override;
    void reportend() override;

private:
    McpTransport & _t;
    std::string    _nvra;
};

// ─── Download progress ────────────────────────────────────────────────────────
/// Fires per-package during commit while zypp fetches packages from repos.
/// Never aborts (progress always returns true).
struct McpDownloadReceive
    : public zypp::callback::ReceiveReport<zypp::repo::DownloadResolvableReport>
{
    explicit McpDownloadReceive( McpTransport & t ) : _t( t ) {}

    void infoInCache( zypp::Resolvable::constPtr resolvable, const zypp::Pathname & localfile ) override;
    void start( zypp::Resolvable::constPtr resolvable, const zypp::Url & url ) override;
    bool progress( int value, zypp::Resolvable::constPtr resolvable ) override;
    void finish( zypp::Resolvable::constPtr resolvable, Error error, const std::string & reason ) override;

private:
    McpTransport & _t;
};

// ─── Commit preload progress ──────────────────────────────────────────────────
/// Fires for the overall concurrent preload of all commit downloads — a
/// single progress stream covering the whole batch, distinct from the
/// per-package McpDownloadReceive above. Never aborts (progress always
/// returns true).
struct McpCommitPreloadReceive
    : public zypp::callback::ReceiveReport<zypp::media::CommitPreloadReport>
{
    explicit McpCommitPreloadReceive( McpTransport & t ) : _t( t ) {}

    void start( const zypp::callback::UserData & userData = zypp::callback::UserData() ) override;
    bool progress( int value, const zypp::callback::UserData & userData = zypp::callback::UserData() ) override;
    void fileStart( const zypp::Pathname & localfile, const zypp::callback::UserData & userData = zypp::callback::UserData() ) override;
    void finish( Result res, const zypp::callback::UserData & userData = zypp::callback::UserData() ) override;

private:
    McpTransport & _t;
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
    explicit McpCommitActiveReceive( McpTransport & t ) : _t( t ) {}

    bool start( const UserData & ) override;
    void reportend() override;

private:
    McpTransport & _t;
};

// ─── RAII scope — connect/disconnect all receivers ───────────────────────────
/// Lifetime: create once in main(), destroyed on process exit.
class McpCallbackScope
{
public:
    McpCallbackScope( McpTransport & t, GpgKeyGate & gpgKeys );
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
