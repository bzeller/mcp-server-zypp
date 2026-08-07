#include "transport.h"
#include "callbacks.h"
#include "tools/registry.h"

#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <array>

#include <zypp-core/base/LogControl.h>
#include <zypp-core/base/InputStream>
#include <zypp-core/parser/json.h>
#include <zypp-core/parser/json/JsonValue.h>
#include <zypp/ZYpp.h>
#include <zypp/ZYppFactory.h>

namespace
{
    // ─── Argument parsing ─────────────────────────────────────────────────────
    struct Args
    {
        std::string tool;
        std::string arg;
        bool        listTools = false;
    };

    Args parseArgs( int argc, char ** argv )
    {
        Args a;
        for ( int i = 1; i < argc; ++i )
        {
            std::string_view sv( argv[i] );
            if ( sv == "--tool" && i + 1 < argc )
                a.tool = argv[++i];
            else if ( sv == "--arg" && i + 1 < argc )
                a.arg = argv[++i];
            else if ( sv == "--list-tools" )
                a.listTools = true;
        }
        return a;
    }

    // ─── Single registry instance ─────────────────────────────────────────────
    const std::array<ToolDescriptor, 8> kRegistry = { {
        { "search_packages",  "Search for packages matching a pattern in name, description or dependency attributes.",
          &schema_search_packages(),  false, tool_search_packages  },
        { "find_providers",   "Find packages that provide a given capability.",
          &schema_find_providers(),   false, tool_find_providers   },
        { "find_dependents",  "Find packages that require, recommend, conflict with or obsolete a given package.",
          &schema_find_dependents(),  false, tool_find_dependents  },
        { "check_updates",    "List pending security patches (or all patches with applicable_only=false).",
          &schema_updates(),          false, tool_updates          },
        { "plan_install",     "Plan installation of a package. Returns the dependency plan for human approval before confirmation.",
          &schema_plan_install(),     false, tool_plan_install     },
        { "plan_remove",      "Plan removal of a package. Returns the list of packages that would be removed for human approval before confirmation.",
          &schema_plan_remove(),      false, tool_plan_remove      },
        { "confirm_install",  "Execute a previously planned installation. Requires root. Re-solves and commits the transaction. Cancellable during solving and download phases; once the RPM transaction begins it cannot be interrupted and will run to completion.",
          &schema_confirm_install(),  true,  tool_confirm_install  },
        { "confirm_remove",   "Execute a previously planned removal. Requires root. Re-solves and commits the transaction. Cancellable during solving and download phases; once the RPM transaction begins it cannot be interrupted and will run to completion.",
          &schema_confirm_remove(),   true,  tool_confirm_remove   },
    } };

    // ─── JSON helpers ─────────────────────────────────────────────────────────
    std::string jsonError( std::string_view code, std::string_view detail )
    {
        return zypp::json::Object{ {
            { "type",   "error" },
            { "code",   std::string(code)   },
            { "detail", std::string(detail) }
        } }.asJSON();
    }

    // ─── --list-tools handler ─────────────────────────────────────────────────
    // Plain stdout write — no framing. The Go proxy reads this with .Output()
    // like any other Unix tool. Only --tool invocations use Content-Length framing.
    int cmdListTools()
    {
        zypp::json::Array tools;
        for ( const auto & td : kRegistry )
        {
            tools.add( zypp::json::Object{ {
                { "name",          std::string(td.name)        },
                { "description",   std::string(td.description) },
                { "input_schema",  *td.inputSchema             },
                { "requires_root", td.requiresRoot             }
            } } );
        }
        const std::string out = tools.asJSON();
        ::fwrite( out.data(), 1, out.size(), stdout );
        ::fputc( '\n', stdout );
        ::fflush( stdout );
        return 0;
    }
}

int main( int argc, char ** argv )
{
    // ─── Logging to file — stdout is the JSON channel, never use it for logs ──
    const char * logfile = std::getenv( "ZYPP_LOGFILE" );
    if ( !logfile )
        logfile = "/var/log/zypp-mcp-tool.log";
    zypp::base::LogControl::instance().logfile( logfile );

    const Args args = parseArgs( argc, argv );
    McpTransport transport;

    // ─── --list-tools: static, no ZYpp lock needed, no framing ──────────────
    if ( args.listTools )
        return cmdListTools();

    if ( args.tool.empty() )
    {
        transport.writeFrame( jsonError( "USAGE",
            "Usage: zypp-mcp-tool --list-tools | --tool <name> [--arg <json>]" ) );
        return 2;
    }

    // ─── Parse --arg once before acquiring the ZYpp lock ─────────────────────
    zypp::json::Object parsedArg;
    if ( !args.arg.empty() )
    {
        try
        {
            std::istringstream ss( args.arg );
            auto val = zypp::json::parseDocument( zypp::InputStream(ss) );
            parsedArg = val.asObject();
        }
        catch ( const std::exception & e )
        {
            transport.writeFrame( jsonError( "INVALID_ARG", e.what() ) );
            return 2;
        }
    }

    // ─── Register callbacks for the lifetime of this process ─────────────────
    McpCallbackScope callbacks( transport );

    // ─── Dispatch via registry ───────────────────────────────────────────────
    // loadSystem() is called inside each tool — it acquires the ZYpp lock and
    // loads the pool. The LOCKED error is caught here if another process holds
    // the lock at the point loadSystem() calls getZYpp() or defaultLoadSystem().
    for ( const auto & td : kRegistry )
    {
        if ( td.name == args.tool )
        {
            try
            {
                return td.execute( parsedArg, transport );
            }
            catch ( const zypp::ZYppFactoryException & e )
            {
                transport.writeFrame( zypp::json::Object{ {
                    { "type",        "error"        },
                    { "code",        "LOCKED"       },
                    { "locker_pid",  e.lockerPid()  },
                    { "locker_name", e.lockerName() }
                } }.asJSON() );
                return 1;
            }
            catch ( const std::exception & e )
            {
                transport.writeFrame( jsonError( "EXCEPTION", e.what() ) );
                return 1;
            }
        }
    }

    transport.writeFrame( jsonError( "UNKNOWN_TOOL", args.tool ) );
    return 2;
}
