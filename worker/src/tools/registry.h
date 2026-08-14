#ifndef MCP_SERVER_ZYPP_REGISTRY_H
#define MCP_SERVER_ZYPP_REGISTRY_H

#include <string_view>

#include <zypp/ZYpp.h>
#include <zypp-core/parser/json/JsonValue.h>

class ToolContext;

// ─── Tool descriptor ─────────────────────────────────────────────────────────
struct ToolDescriptor
{
    std::string_view            name;
    std::string_view            description;
    const zypp::json::Object *  inputSchema;
    bool                        requiresRoot = false; ///< If true, proxy only registers when uid==0
    int (*execute)( const zypp::json::Object & arg, ToolContext & );
};

// Forward declarations — one per tools/*.cc
int tool_search_packages( const zypp::json::Object &, ToolContext & );
int tool_find_providers(  const zypp::json::Object &, ToolContext & );
int tool_find_dependents( const zypp::json::Object &, ToolContext & );
int tool_updates(         const zypp::json::Object &, ToolContext & );
int tool_plan_install(    const zypp::json::Object &, ToolContext & );
int tool_plan_remove(     const zypp::json::Object &, ToolContext & );
int tool_confirm_install( const zypp::json::Object &, ToolContext & );
int tool_confirm_remove(  const zypp::json::Object &, ToolContext & );

// Schema accessors
const zypp::json::Object & schema_search_packages();
const zypp::json::Object & schema_find_providers();
const zypp::json::Object & schema_find_dependents();
const zypp::json::Object & schema_updates();
const zypp::json::Object & schema_plan_install();
const zypp::json::Object & schema_plan_remove();
const zypp::json::Object & schema_confirm_install();
const zypp::json::Object & schema_confirm_remove();

#endif // MCP_SERVER_ZYPP_REGISTRY_H
