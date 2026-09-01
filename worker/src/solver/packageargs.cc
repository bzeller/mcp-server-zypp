#include "packageargs.h"

#include <iostream>
#include <iterator>

#include <zypp-core/base/Logger.h>
#include <zypp-core/base/String.h>
#include <zypp/sat/Pool.h>

#undef  ZYPP_BASE_LOGGER_LOGGROUP
#define ZYPP_BASE_LOGGER_LOGGROUP "solverequest"

namespace solverequest
{

PackageArgs::PackageArgs( const std::vector<std::string> & args, const zypp::ResKind & kind,
                          const Options & opts, const std::optional<StringSet> & knownRepoAliases )
: _opts( opts )
{
    preprocess( args );
    argsToCaps( kind, knownRepoAliases );
}

PackageArgs::PackageArgs( PackageSpecSet dos, PackageSpecSet donts, Options opts )
: _opts( std::move(opts) )
, _dos( std::move(dos) )
, _donts( std::move(donts) )
{}

// ---------------------------------------------------------------------------

void PackageArgs::preprocess( const std::vector<std::string> & args )
{
    // Preprocess asserts not to store empty strings in _args !
    auto storeIfNotEmpty = [this]( std::string && arg ) {
        if ( ! arg.empty() )
            this->_args.insert( std::move(arg) );
    };

    std::vector<std::string>::size_type argc = args.size();
    std::string arg;
    bool op = false;
    for ( unsigned i = 0; i < argc; ++i )
    {
        std::string tmp = args[i];

        if ( op )
        {
            arg += tmp;
            op = false;
            tmp.clear();
        }
        // standalone operator
        else if ( tmp == "=" || tmp == "==" || tmp == "<"
               || tmp == ">" || tmp == "<=" || tmp == ">=" )
        {
            // not at the start or the end
            if ( i && i < argc - 1 )
                op = true;
        }
        // operator at the end of a random string, e.g. 'zypper='
        else if ( tmp.find_last_of( "=<>" ) == tmp.size() - 1 && i < argc - 1 )
        {
            storeIfNotEmpty( std::move(arg) );
            arg = tmp;
            op = true;
            continue;
        }
        // operator at the start of a random string e.g. '>=3.2.1'
        else if ( i && tmp.find_first_of( "=<>" ) == 0 )
        {
            arg += tmp;
            tmp.clear();
            op = false;
        }

        if ( op )
            arg += tmp;
        else
        {
            storeIfNotEmpty( std::move(arg) );
            arg = tmp;
        }
    }

    storeIfNotEmpty( std::move(arg) );

    DBG << "args received: ";
    std::copy( args.begin(), args.end(), std::ostream_iterator<std::string>(DBG, " ") );
    DBG << std::endl;

    DBG << "args compiled: ";
    std::copy( _args.begin(), _args.end(), std::ostream_iterator<std::string>(DBG, " ") );
    DBG << std::endl;
}

// ---------------------------------------------------------------------------

namespace
{
    bool remove_duplicate( PackageSpecSet & set, const PackageSpec & obj )
    {
        auto match = set.find( obj );
        if ( match != set.end() )
        {
            DBG << "found dupe: '" << match->orig_str << "' : " << obj.orig_str << std::endl;
            set.erase( match );
            return true;
        }
        return false;
    }
}

// ---------------------------------------------------------------------------

void PackageArgs::argsToCaps( const zypp::ResKind & kind, const std::optional<StringSet> & knownRepoAliases )
{
    using namespace zypp;

    // See this header's point 2: an explicitly empty set means "match no
    // repo prefixes at all"; nullopt (the default) means "the caller
    // didn't say, fall back to whatever is actually loaded right now".
    StringSet fallbackAliases;
    if ( !knownRepoAliases )
        for ( const auto & repo : sat::Pool::instance().repos() )
            fallbackAliases.insert( repo.alias() );
    const StringSet & aliases = knownRepoAliases ? *knownRepoAliases : fallbackAliases;

    for ( std::string arg : _args )
    {
        bool dont;
        std::string repo;

        PackageSpec spec;
        spec.orig_str = arg;

        // For given arguments:
        //    +vim
        //    -emacs
        //    libdnet1.i586
        //    perl-devel:perl(Digest::MD5)
        //    ~non-oss:opera-2:10.1-1.2.gcc44.x86_64
        //    zypper>=1.2.15
        //
        // 1) check for and remove the install/remove modifiers
        //    vim                          (install)
        //    emacs                        (remove)
        //    perl-devel:perl(Digest::MD5) (install/remove according to command)
        //
        // 2) check for and remove the repo specifier at the beginning of the arg
        //    vim                           (no repo)
        //    libdnet1.i586                 (no repo)
        //    perl(Digest::MD5)             (perl-devel repo)
        //    opera-2:10.1-1.2.gcc44.x86_64 (non-oss repo)
        //    note: repo is matched by alias only (see packageargs.h header comment)
        //
        // 3) parse the rest of the string as a standard zypp package specifier
        //    into a Capability using Capability::guessPackageSpec
        //                                  name, arch, op, evr, kind
        //    vim                           'vim', '', '', '', 'package'
        //    libdnet1.i586                 'libdnet', 'i586', '', '', 'package'
        //    perl(Digest::MD5)             'perl(Digest::MD5)', '', '', '', 'package'
        //    opera-2:10.1-1.2.gcc44.x86_64 'opera', 'x86_64', '=', '2:10.1-1.2.gcc44', 'package'
        //    zypper>=1.2.15                'zypper', '', '>=', '1.2.15', 'package'

        // check for and remove the install/remove modifiers
        // sort as do/dont
        // bsc#1201576: Make sure a modifier precedes a not-empty argument.
        if ( arg[0] == '+' || arg[0] == '~' )
        {
            if ( arg[1] == '\0' )
                throw InvalidArgumentError(
                    str::form( "'%s' install modifier must not be used without a package name or capability.",
                               arg.c_str() ) );
            dont = false;
            arg.erase( 0, 1 );
        }
        else if ( arg[0] == '-' || arg[0] == '!' )
        {
            if ( arg[1] == '\0' )
                throw InvalidArgumentError(
                    str::form( "'%s' remove modifier must not be used without a package name or capability.",
                               arg.c_str() ) );
            dont = true;
            arg.erase( 0, 1 );
        }
        else if ( _opts.doByDefault )
            dont = false;
        else
            dont = true;

        // check for and remove the 'repo:' prefix
        // ignore colons coming after '(' or '=' (bnc #433679)
        // e.g. 'perl(Digest::MD5)', or 'opera=2:10.00-4102.gcc4.shared.qt3'
        bool hasRepo = false;
        std::string::size_type pos = arg.find( ':' );
        while ( pos != std::string::npos && arg.find_first_of( "(=" ) > pos )
        {
            repo = arg.substr( 0, pos );

            if ( aliases.count( repo ) )
            {
                hasRepo = true;
                arg = arg.substr( pos + 1 );
                DBG << "got repo '" << repo << "' for '" << arg << "'" << std::endl;
                break;
            }

            // handle the case of having one or multiple ":" in the repo alias (bsc #1041178)
            pos = arg.find( ':', pos + 1 );
        }

        // not a repo, continue as usual
        if ( !hasRepo )
            repo.clear();

        // check if we already have a package specifier in the arg; if we do,
        // use that one always
        Capability parsedcap;
        ResKind kindFromPosArg = ResKind::explicitBuiltin( arg );

        if ( kindFromPosArg == ResKind::nokind && kind != ResKind::nokind && kind != ResKind::package )
        {
            // prepend the kind for non-packages if not already there (bnc #640399)
            parsedcap = Capability::guessPackageSpec( kind.asString() + ":" + arg, spec.modified );
        }
        else
        {
            parsedcap = Capability::guessPackageSpec( arg, spec.modified );
        }

        if ( spec.modified )
        {
            DBG << "'" << arg << "' not found, trying '" << parsedcap <<  "'" << std::endl;
        }

        // recognize misplaced command line options given as packages (bnc#391644)
        if ( arg[0] == '-' )
            throw InvalidArgumentError(
                str::form( "'%s' is not a package name or capability.", arg.c_str() ) );

        MIL << "got " << (dont?"un":"") << "wanted '" << parsedcap << "'" << "; repo '" << repo << "'" << std::endl;

        // Store, but avoid duplicates in do and don't sets.
        spec.parsed_cap = parsedcap;
        spec.repo_alias = repo;
        if ( dont )
        {
            if ( !remove_duplicate( _dos, spec ) )
                _donts.insert( spec );
        }
        else if ( !remove_duplicate( _donts, spec ) )
            _dos.insert( spec );
    }
}

} // namespace solverequest

std::ostream & operator<<( std::ostream & out, const solverequest::PackageSpec & spec )
{
    out << spec.orig_str << " cap:" << spec.parsed_cap;
    if ( spec.modified )
        out << " (mod)";
    if ( !spec.repo_alias.empty() )
        out << " repo: " << spec.repo_alias;
    return out;
}
