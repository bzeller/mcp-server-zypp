#ifndef MCP_SERVER_ZYPP_SOLVER_TEST_HELPERS_H
#define MCP_SERVER_ZYPP_SOLVER_TEST_HELPERS_H

// Tiny free-function replacements for zypper's old SolverRequester
// convenience methods (install(PackageArgs)/remove(...)/update(...)),
// used only by the surgically-edited call sites in
// solverrequester_test.cc — every other line in that file (fixtures,
// assertions, expected values) is untouched from upstream.
//
// Deliberately free functions, not a subclass wrapping Requester: a
// subclass mimicking the old method-call syntax would need its own
// PackageArgs-construction logic anyway, duplicating exactly what these
// four functions already do, while adding an inheritance layer whose only
// purpose is cosmetic call-site compatibility.
//
// Production code (worker/src/tools/transaction.cc) always calls
// Requester::submit() with an explicit zypp::ResPool — see requester.h's
// header comment. These helpers' use of getZYpp()->pool() is confined to
// this test file's own fixture design: BOOST_GLOBAL_FIXTURE (TestInit, in
// solverrequester_test.cc) sets up exactly one pool, once, for the whole
// test binary.

#include <vector>

#include <zypp/ResKind.h>
#include <zypp/ZYppFactory.h>

#include "../../src/solver/packageargs.h"
#include "../../src/solver/requester.h"

namespace solverequest_test
{

inline void submitArgs( solverequest::Requester & sr, solverequest::Operation op,
                        const solverequest::PackageArgs & args )
{
    sr.submit( zypp::getZYpp()->pool(), { op, args.dos(), args.donts() } );
}

inline void install( solverequest::Requester & sr, const std::vector<std::string> & rawargs )
{
    submitArgs( sr, solverequest::Operation::Install, solverequest::PackageArgs( rawargs ) );
}

inline void remove( solverequest::Requester & sr, const solverequest::PackageArgs & args )
{
    submitArgs( sr, solverequest::Operation::Remove, args );
}

/// Avoids the implicit vector<string> -> PackageArgs conversion picking
/// PackageArgs::Options' default doByDefault == true, which would be
/// wrong for remove — mirrors upstream's
/// SolverRequester::remove(const std::vector<std::string>&, const ResKind&).
inline void remove( solverequest::Requester & sr, const std::vector<std::string> & rawargs,
                    const zypp::ResKind & kind = zypp::ResKind::package )
{
    solverequest::PackageArgs::Options opts;
    opts.doByDefault = false;
    remove( sr, solverequest::PackageArgs( rawargs, kind, opts ) );
}

inline void update( solverequest::Requester & sr, const std::vector<std::string> & rawargs )
{
    submitArgs( sr, solverequest::Operation::Update, solverequest::PackageArgs( rawargs ) );
}

} // namespace solverequest_test

#endif // MCP_SERVER_ZYPP_SOLVER_TEST_HELPERS_H
