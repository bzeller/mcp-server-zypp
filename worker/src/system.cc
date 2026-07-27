#include "system.h"

#include <zypp/ZYppFactory.h>
#include <zypp/misc/DefaultLoadSystem.h>
#include <zypp/misc/LoadTestcase.h>
#include <zypp/misc/TestcaseSetup.h>
#include <zypp/RepoManager.h>
#include <zypp/sat/Pool.h>

zypp::ZYpp::Ptr loadSystem( const std::optional<zypp::Pathname> & testcase )
{
    if ( testcase )
    {
        // ── Testcase path ─────────────────────────────────────────────────────
        // Reject paths that are not recognised testcase directories — we do not
        // support loading arbitrary system roots other than "/".
        if ( zypp::misc::testcase::LoadTestcase::testcaseTypeAt( *testcase )
                 == zypp::misc::testcase::LoadTestcase::None )
        {
            ZYPP_THROW( zypp::Exception(
                "Path '" + testcase->asString() +
                "' is not a recognised solver testcase (Helix or YAML)." ) );
        }

        // Acquire ZYpp instance before applySetup — it needs the pool.
        zypp::ZYpp::Ptr zypp = zypp::getZYpp();

        zypp::misc::testcase::LoadTestcase loader;
        std::string err;
        if ( !loader.loadTestcaseAt( *testcase, &err ) )
            ZYPP_THROW( zypp::Exception( err ) );

        zypp::RepoManager tempMgr( zypp::RepoManagerOptions::makeTestSetup( *testcase ) );

        // AS_UNIVERSE: repos + locales + autoinstalled + vendor lists + modalias
        // + multiversion — but NOT AS_SOLVER_FLAGS. Solver opinions come from
        // the LLM's request, not from the captured testcase session.
        if ( !loader.setupInfo().applySetup( tempMgr,
                 zypp::misc::testcase::AS_UNIVERSE ) )
            ZYPP_THROW( zypp::Exception("Failed to apply testcase setup") );

        // Build the whatprovides index — required for sat::WhatProvides,
        // Pool::whatMatchesSolvable and PoolQuery dependency searches.
        // For live systems the solver triggers this via SATResolver::prepare().
        // For testcases loaded without a solver run it must be called explicitly.
        zypp::sat::Pool::instance().prepare();

        return zypp;
    }
    else
    {
        // ── Live system ───────────────────────────────────────────────────────
        // defaultLoadSystem acquires the ZYpp instance internally.
        // LS_NOREFRESH: never refresh repos — use whatever cache exists.
        // We do not use LS_READONLY: the readonly hack prevents cache writes
        // which breaks loadFromCache if the cache is stale or missing.
        zypp::misc::defaultLoadSystem( zypp::misc::LS_NOREFRESH );

        // Build the whatprovides index — required for sat::WhatProvides,
        // Pool::whatMatchesSolvable and PoolQuery dependency searches.
        // For live systems the solver triggers this via SATResolver::prepare().
        // For testcases loaded without a solver run it must be called explicitly.
        zypp::sat::Pool::instance().prepare();


        return zypp::getZYpp();  // safe — instance now exists
    }
}
