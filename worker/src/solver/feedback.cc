#include "feedback.h"

namespace solverequest
{

Feedback::Feedback( Id id, PackageSpec reqpkg, zypp::PoolItem selected, zypp::PoolItem installed )
: _id( id )
, _reqpkg( std::move(reqpkg) )
, _objsel( std::move(selected) )
, _objinst( std::move(installed) )
{}

Feedback::Feedback( Id id, PackageSpec reqpkg, std::string userdata )
: _id( id )
, _reqpkg( std::move(reqpkg) )
, _userdata( std::move(userdata) )
{}

} // namespace solverequest
