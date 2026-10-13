#pragma once

#include <cstdint>

namespace Dingo::Net
{

	struct Version
	{
		uint32_t Major;
		uint32_t Minor;
		uint32_t Patch;
		uint32_t Build;
	};

	Version GetVersion();

}
