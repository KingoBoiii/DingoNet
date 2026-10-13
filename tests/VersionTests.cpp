#include "TestFramework.h"

#include <DingoNet/BuildInfo.h>
#include <DingoNet/Version.h>

TEST(VersionMatchesBuildInfo)
{
	const Dingo::Net::Version version = Dingo::Net::GetVersion();
	CHECK(version.Major == DN_VERSION_MAJOR);
	CHECK(version.Minor == DN_VERSION_MINOR);
	CHECK(version.Patch == DN_VERSION_PATCH);
	CHECK(version.Build == DN_VERSION_BUILD);
}
