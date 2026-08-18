#ifndef MCP_SERVER_ZYPP_GPGKEYGATE_H
#define MCP_SERVER_ZYPP_GPGKEYGATE_H

#include <set>
#include <string>
#include <vector>

/// A package signing key encountered during commit that was not trusted.
struct RejectedKey
{
    std::string fingerprint;
    std::string name;
    std::string repo;
};

/// Pre-approved package signing keys, plus a record of the keys that were
/// not trusted and therefore blocked a commit.
///
/// Trust decisions are NOT made here — they go through MCP elicitation in
/// McpKeyRingReceive (see callbacks.h). This class only answers "was this
/// fingerprint approved up front?" and collects the ones that were refused,
/// so the tool can report exactly which key(s) stopped the transaction.
///
/// Nothing populates the accepted set today: a key is normally trusted by an
/// explicit human answer to an elicitation prompt. The set exists so an
/// operator-supplied source (config file, environment) can be added later
/// without touching the callbacks — it is deliberately never filled from a
/// tool argument the model controls.
///
/// Deliberately free of libzypp types: plain logic, unit-tested as such.
class GpgKeyGate
{
public:
    /// Replace the set of pre-approved fingerprints.
    void accept( const std::set<std::string> & fingerprints_r );

    /// Whether fingerprint_r is pre-approved. Pure query — records nothing.
    bool isAccepted( const std::string & fingerprint_r ) const;

    /// Record a key that was not trusted. Deduplicated by fingerprint —
    /// one key typically signs many packages.
    void recordRejection( const std::string & fingerprint_r,
                          const std::string & name_r,
                          const std::string & repo_r );

    const std::vector<RejectedKey> & rejected() const { return _rejected; }
    bool hasRejections() const { return !_rejected.empty(); }

private:
    std::set<std::string>    _accepted;   ///< normalised (uppercase)
    std::vector<RejectedKey> _rejected;
};

#endif // MCP_SERVER_ZYPP_GPGKEYGATE_H
