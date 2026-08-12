#ifndef MCP_SERVER_ZYPP_VALIDATE_H
#define MCP_SERVER_ZYPP_VALIDATE_H

#include <string>
#include <set>
#include <initializer_list>
#include <zypp-core/base/Exception.h>
#include <zypp-core/base/String.h>
#include <zypp-core/base/Regex.h>
#include <zypp-core/parser/json/JsonValue.h>

/// Validation helpers for tool arguments.
/// All throw zypp::Exception with an actionable message on failure.
/// The message is designed so the LLM can self-correct and retry.
/// All validation should happen before loadSystem() — fail fast,
/// no ZYpp lock acquired for bad input.

namespace validate
{
    /// Throw a descriptive exception for an invalid argument value.
    [[ noreturn ]] inline void fail( const std::string & name,
                                     const std::string & value,
                                     const std::string & hint )
    {
        ZYPP_THROW( zypp::Exception(
            "Invalid value for argument '" + name + "': '" + value + "'. " + hint ) );
    }

    /// Require a string argument to be non-empty.
    inline std::string requireNonEmpty( const zypp::json::Object & arg,
                                        const std::string & name )
    {
        if ( !arg.contains(name) )
            fail( name, "", "Argument is required." );
        const std::string val =
            static_cast<std::string>( arg.value(name).asString() );
        if ( val.empty() )
            fail( name, val, "Value must not be empty." );
        return val;
    }

    /// Validate a string argument against a fixed set of allowed values.
    /// Comparison is case-insensitive.
    inline std::string requireEnum( const zypp::json::Object & arg,
                                    const std::string & name,
                                    std::initializer_list<const char *> allowed )
    {
        if ( !arg.contains(name) )
            fail( name, "", "Argument is required." );
        const std::string val =
            static_cast<std::string>( arg.value(name).asString() );
        for ( const char * a : allowed )
            if ( zypp::str::compareCI( val, std::string(a) ) == 0 )
                return val;

        std::string choices;
        for ( const char * a : allowed )
        {
            if ( !choices.empty() ) choices += ", ";
            choices += a;
        }
        fail( name, val, "Must be one of: " + choices + "." );
    }

    /// Validate a string argument against a regex pattern.
    inline std::string requirePattern( const zypp::json::Object & arg,
                                       const std::string & name,
                                       const std::string & pattern,
                                       const std::string & hint )
    {
        if ( !arg.contains(name) )
            fail( name, "", "Argument is required." );
        const std::string val =
            static_cast<std::string>( arg.value(name).asString() );
        // NOTE: intentionally not `static` — a function-local static regex
        // would be compiled once from the *first* caller's pattern and then
        // silently reused (wrong match rules) for every other pattern
        // argument passed on later calls. Every call site currently passes
        // a distinct literal pattern, so compiling per-call is correct;
        // it also keeps this validation helper trivially thread-safe.
        const zypp::str::regex rx( pattern );
        if ( !zypp::str::regex_match( val, rx ) )
            fail( name, val, hint );
        return val;
    }

    /// Validate an optional enum argument — returns empty string if absent.
    inline std::string optionalEnum( const zypp::json::Object & arg,
                                     const std::string & name,
                                     std::initializer_list<const char *> allowed )
    {
        if ( !arg.contains(name) )
            return {};
        return requireEnum( arg, name, allowed );
    }

    /// Get an optional string argument — returns empty string if absent.
    /// Validates it is non-empty if present.
    inline std::string optionalNonEmpty( const zypp::json::Object & arg,
                                         const std::string & name )
    {
        if ( !arg.contains(name) )
            return {};
        return requireNonEmpty( arg, name );
    }

    /// Get an optional boolean — returns default if absent.
    inline bool optionalBool( const zypp::json::Object & arg,
                               const std::string & name,
                               bool defaultValue = false )
    {
        if ( !arg.contains(name) )
            return defaultValue;
        return arg.value(name).asBool();
    }

    /// Get an optional array-of-strings argument — returns an empty set if
    /// absent. Used for e.g. confirm_install's accepted_licenses, where
    /// caller-supplied order/duplicates carry no meaning.
    inline std::set<std::string> optionalStringSet( const zypp::json::Object & arg,
                                                     const std::string & name )
    {
        std::set<std::string> result;
        if ( !arg.contains(name) )
            return result;
        for ( const auto & v : arg.value(name).asArray() )
            result.insert( std::string( v.asString() ) );
        return result;
    }
}

#endif // MCP_SERVER_ZYPP_VALIDATE_H
