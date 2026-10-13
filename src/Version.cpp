#include "DingoNet/Version.h"
#include "DingoNet/BuildInfo.h"

namespace Dingo::Net
{

	Version GetVersion()
	{
		return { DN_VERSION_MAJOR, DN_VERSION_MINOR, DN_VERSION_PATCH, DN_VERSION_BUILD };
	}

}
