#include "transaction.h"
#include "tools.h"
#include "validate.h"
#include "../transport.h"

#include <zypp/Resolver.h>
#include <zypp/PoolItem.h>
#include <zypp/Capability.h>
#include <zypp/ResStatus.h>
#include <zypp/base/Algorithm.h>

using namespace zypp;

// ─── Shared schema property objects ──────────────────────────────────────────

const zypp::json::Object & installSchemaProperties()
{
    static const zypp::json::Object props = {
        { "package", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Package name or capability to install." }
        } } },
        { "type", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Type of resolvable to install." },
            { "enum",        zypp::json::Array{ "package", "patch", "pattern", "product" } },
            { "default",     "package" }
        } } },
        { "repo", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Install from this specific repository alias only." }
        } } },
        { "capability", zypp::json::Object{ {
            { "type",        "boolean" },
            { "description", "Match by capability rather than exact name (default: true)." },
            { "default",     true }
        } } },
        { "no_recommends", zypp::json::Object{ {
            { "type",        "boolean" },
            { "description", "Do not install recommended packages, only required ones (default: false)." },
            { "default",     false }
        } } },
        { "solver_options", zypp::json::Object{ {
            { "type",        "object" },
            { "description", "Fine-grained solver permissions." },
            { "properties",  zypp::json::Object{
                { "allow_downgrade",     zypp::json::Object{ { { "type", "boolean" } } } },
                { "allow_name_change",   zypp::json::Object{ { { "type", "boolean" } } } },
                { "allow_arch_change",   zypp::json::Object{ { { "type", "boolean" } } } },
                { "allow_vendor_change", zypp::json::Object{ { { "type", "boolean" } } } }
            } }
        } } }
    };
    return props;
}

const zypp::json::Object & removeSchemaProperties()
{
    static const zypp::json::Object props = {
        { "package", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Package name or capability to remove." }
        } } },
        { "type", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Type of resolvable to remove." },
            { "enum",        zypp::json::Array{ "package", "patch", "pattern", "product" } },
            { "default",     "package" }
        } } },
        { "repo", zypp::json::Object{ {
            { "type",        "string" },
            { "description", "Restrict removal to packages from this repository alias." }
        } } },
        { "capability", zypp::json::Object{ {
            { "type",        "boolean" },
            { "description", "Match by capability rather than exact name (default: false)." },
            { "default",     false }
        } } },
        { "clean_deps", zypp::json::Object{ {
            { "type",        "boolean" },
            { "description", "Automatically remove unneeded dependencies after removal (default: false)." },
            { "default",     false }
        } } }
    };
    return props;
}

// ─── setupInstall ─────────────────────────────────────────────────────────────

bool setupInstall( const zypp::json::Object & arg,
                   ZYpp::Ptr                  zypp,
                   const std::string &        toolName,
                   McpTransport &             t )
{
    const std::string pkg  = validate::requireNonEmpty( arg, "package" );
    const std::string type = validate::optionalEnum( arg, "type",
        { "package", "patch", "pattern", "product" } );
    const std::string repo = validate::optionalNonEmpty( arg, "repo" );

    // capability defaults true; forced false when repo is given (name-based lookup)
    const bool byCapability =
        repo.empty() && validate::optionalBool( arg, "capability", true );
    const bool noRecommends = validate::optionalBool( arg, "no_recommends" );

    Resolver_Ptr resolver = zypp->resolver();

    if ( noRecommends )
        resolver->setOnlyRequires( true );

    if ( arg.contains("solver_options") )
    {
        const auto & opts = arg.value("solver_options").asObject();
        if ( validate::optionalBool( opts, "allow_downgrade"    ) ) resolver->setAllowDowngrade( true );
        if ( validate::optionalBool( opts, "allow_name_change"  ) ) resolver->setAllowNameChange( true );
        if ( validate::optionalBool( opts, "allow_arch_change"  ) ) resolver->setAllowArchChange( true );
        if ( validate::optionalBool( opts, "allow_vendor_change") ) resolver->setAllowVendorChange( true );
    }

    const ResKind kind = kindFromString( type );

    if ( byCapability )
    {
        resolver->addRequire( Capability( pkg, kind ) );
    }
    else
    {
        PoolItem found;
        auto findAvailable = [&]( const PoolItem & pi ) -> bool {
            if ( pi.status().isInstalled() )                                 return true;
            if ( !repo.empty() &&
                 pi.satSolvable().repository().info().alias() != repo )      return true;
            found = pi;
            return false;
        };
        invokeOnEach( zypp->pool().byIdentBegin( kind, pkg ),
                      zypp->pool().byIdentEnd(   kind, pkg ),
                      std::ref(findAvailable) );

        if ( !found )
        {
            const std::string detail = repo.empty()
                ? "Package '" + pkg + "' not found in any repository."
                : "Package '" + pkg + "' not found in repository '" + repo + "'.";
            t.writeFrame( zypp::json::Object{ {
                { "type",   "error"    },
                { "code",   "NOT_FOUND"},
                { "tool",   toolName   },
                { "detail", detail     }
            } }.asJSON() );
            return false;
        }
        found.status().setToBeInstalled( ResStatus::USER );
    }
    return true;
}

// ─── setupRemove ─────────────────────────────────────────────────────────────

SetupRemoveResult setupRemove( const zypp::json::Object & arg,
                               ZYpp::Ptr                  zypp,
                               const std::string &        toolName,
                               McpTransport &             t )
{
    const std::string pkg  = validate::requireNonEmpty( arg, "package" );
    const std::string type = validate::optionalEnum( arg, "type",
        { "package", "patch", "pattern", "product" } );
    const std::string repo = validate::optionalNonEmpty( arg, "repo" );

    const bool byCapability = validate::optionalBool( arg, "capability", false );
    const bool cleanDeps    = validate::optionalBool( arg, "clean_deps" );

    Resolver_Ptr resolver = zypp->resolver();

    if ( cleanDeps )
        resolver->setCleandepsOnRemove( true );

    const ResKind kind = kindFromString( type );

    if ( byCapability )
    {
        resolver->addConflict( Capability( pkg, kind ) );
    }
    else
    {
        PoolItem found;
        auto findInstalled = [&]( const PoolItem & pi ) -> bool {
            if ( !pi.status().isInstalled() )                                return true;
            if ( !repo.empty() &&
                 pi.satSolvable().repository().info().alias() != repo )      return true;
            found = pi;
            return false;
        };
        invokeOnEach( zypp->pool().byIdentBegin( kind, pkg ),
                      zypp->pool().byIdentEnd(   kind, pkg ),
                      std::ref(findInstalled) );

        if ( !found )
        {
            // Not installed — empty plan result, not an error.
            t.writeFrame( zypp::json::Object{ {
                { "type", "result"  },
                { "tool", toolName  },
                { "plan", zypp::json::Object{ {
                    { "to_remove", zypp::json::Array{} }
                } } }
            } }.asJSON() );
            return SetupRemoveResult::NotInstalled;
        }
        found.status().setToBeUninstalled( ResStatus::USER );
    }
    return SetupRemoveResult::Ok;
}

// ─── planInstallToJson ────────────────────────────────────────────────────────

zypp::json::Object planInstallToJson( const std::string &   toolName,
                                      const ResPool &        pool )
{
    zypp::json::Array toInstall, toRemove, licenses;
    for ( const auto & pi : pool )
    {
        if ( pi.status().isToBeInstalled() )
        {
            toInstall.add( zypp::json::Object{ {
                { "name",    pi.name()               },
                { "edition", pi.edition().asString() },
                { "arch",    pi.arch().asString()    }
            } } );
            const std::string lic = pi.satSolvable().licenseToConfirm();
            if ( !lic.empty() && pi.satSolvable().needToAcceptLicense() )
                licenses.add( zypp::json::Object{ {
                    { "package", pi.name() },
                    { "text",    lic        }
                } } );
        }
        else if ( pi.status().isToBeUninstalled() )
        {
            toRemove.add( zypp::json::Object{ {
                { "name",    pi.name()               },
                { "edition", pi.edition().asString() },
                { "arch",    pi.arch().asString()    }
            } } );
        }
    }
    return zypp::json::Object{ {
        { "type", "result" },
        { "tool", toolName },
        { "plan", zypp::json::Object{ {
            { "to_install", std::move(toInstall) },
            { "to_remove",  std::move(toRemove)  },
            { "licenses",   std::move(licenses)  }
        } } }
    } };
}

// ─── planRemoveToJson ─────────────────────────────────────────────────────────

zypp::json::Object planRemoveToJson( const std::string &   toolName,
                                     const ResPool &        pool )
{
    zypp::json::Array toRemove;
    for ( const auto & pi : pool )
    {
        if ( !pi.status().isToBeUninstalled() ) continue;
        toRemove.add( zypp::json::Object{ {
            { "name",    pi.name()               },
            { "edition", pi.edition().asString() },
            { "arch",    pi.arch().asString()    }
        } } );
    }
    return zypp::json::Object{ {
        { "type", "result"  },
        { "tool", toolName  },
        { "plan", zypp::json::Object{ {
            { "to_remove", std::move(toRemove) }
        } } }
    } };
}
