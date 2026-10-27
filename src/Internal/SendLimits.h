#pragma once

#include "DingoNet/Transport.h"
#include "Internal/Log.h"

#include <string_view>

namespace Dingo::Net::Internal
{

	inline bool CheckUnreliableSize(size_t size, Delivery delivery, std::string_view transport)
	{
		if (!IsUnreliable(delivery) || size <= MAX_UNRELIABLE_BYTES)
		{
			return true;
		}
		LogError("{}: dropped an unreliable packet of {} bytes (the limit is {})", transport, size, MAX_UNRELIABLE_BYTES);
		return false;
	}

}
