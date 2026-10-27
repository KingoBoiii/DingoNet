#pragma once

#include "DingoNet/UdpTransport.h"

#include <cstdint>

namespace Dingo::Net::Internal
{

	// For tests only. A Conditioner never drops a reliable packet, so loss below ENet's own retransmits needs a hook
	// on the datagrams the transport receives.
	struct UdpTestAccess
	{
		// Drops a seeded share of the incoming datagrams (0 removes the hook).
		static void DropIncomingDatagrams(UdpTransport& transport, float share, uint32_t seed);
	};

}
