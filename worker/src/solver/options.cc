#include "options.h"

#include <zypp-core/base/Logger.h>

#undef  ZYPP_BASE_LOGGER_LOGGROUP
#define ZYPP_BASE_LOGGER_LOGGROUP "solverequest"

namespace solverequest
{

void Options::setForceByCap( bool value )
{
    _forceByCap = value;
    if ( value && _forceByName )
    {
        DBG << "resetting previously set forceByName" << std::endl;
        _forceByName = false;
    }
}

void Options::setForceByName( bool value )
{
    _forceByName = value;
    if ( value && _forceByCap )
    {
        DBG << "resetting previously set forceByCap" << std::endl;
        _forceByCap = false;
    }
}

} // namespace solverequest
