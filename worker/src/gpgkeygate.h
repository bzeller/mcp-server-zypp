#ifndef MCP_SERVER_ZYPP_GPGKEYGATE_H
#define MCP_SERVER_ZYPP_GPGKEYGATE_H

#include <set>
#include <string>
#include <vector>

/// A package signing key encountered during commit that was not pre-accepted.
struct RejectedKey
{
    std::string fingerprint;
    std::string name;
    std::string repo;
};

/// Deny-by-default gate for package signing keys — the same two-phase
/// token exchange used for licenses (see tools/transaction.h). A key is
/// trusted only if its fingerprint is explicitly present in the accepted
/// set; every other case (missing, empty set, unrecognised fingerprint)
/// is rejected, never silently allowed through.
///
/// libzypp asks whether to trust a package signing key via
/// KeyRing::askUserToAcceptPackageKey(), which must be answered synchronously
/// and cannot be deferred. Rather than blocking on an interactive
/// elicitation, confirm_install takes the fingerprints the caller already
/// reviewed and answers from that set alone. A key not in the set is
/// rejected and recorded, so the tool can report exactly what still needs
/// confirming.
///
/// Deliberately free of libzypp types: plain decision logic, unit-tested as such.
class GpgKeyGate
{
public:
    void accept( const std::set<std::string> & fingerprints_r );

    /// True if fingerprint_r was pre-accepted. On a miss the key is recorded
    /// (deduplicated by fingerprint) and false is returned — never blocks,
    /// never prompts.
    bool isAccepted( const std::string & fingerprint_r,
                     const std::string & name_r,
                     const std::string & repo_r );

    const std::vector<RejectedKey> & rejected() const { return _rejected; }
    bool hasRejections() const { return !_rejected.empty(); }

private:
    std::set<std::string>    _accepted;   ///< normalised (uppercase)
    std::vector<RejectedKey> _rejected;
};

#endif // MCP_SERVER_ZYPP_GPGKEYGATE_H
