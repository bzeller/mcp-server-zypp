#ifndef MCP_SERVER_ZYPP_SYSTEM_H
#define MCP_SERVER_ZYPP_SYSTEM_H

#include <optional>
#include <zypp/ZYpp.h>
#include <zypp-core/Pathname.h>

/// Acquire the ZYpp instance and load the package universe into the pool.
///
/// If \a testcase is nullopt, the live system at "/" is loaded via
/// defaultLoadSystem() which handles the readonly-hack, LS_NOREFRESH for
/// non-root, and proper repo cache management. defaultLoadSystem() also
/// acquires the ZYpp instance internally.
///
/// If \a testcase is set, the path must be a recognised solver testcase
/// directory (Helix or YAML format). AS_UNIVERSE is applied: repos, locales,
/// autoinstalled, vendor lists, modalias, multiversion — but NOT solver flags.
/// If the path exists but is not a testcase, an exception is thrown.
///
/// In both cases the ZYpp instance is acquired here — callers must NOT call
/// getZYpp() before this function.
///
/// \returns the acquired ZYpp::Ptr
/// \throws zypp::ZYppFactoryException if another process holds the lock
/// \throws zypp::Exception on any load failure or on an unrecognised path.
zypp::ZYpp::Ptr loadSystem(
    const std::optional<zypp::Pathname> & testcase = std::nullopt );

#endif // MCP_SERVER_ZYPP_SYSTEM_H
