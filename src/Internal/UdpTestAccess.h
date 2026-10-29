#pragma once

#include "DingoNet/UdpTransport.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace Dingo::Net::Internal
{

	// For tests only. A Conditioner never drops a reliable packet, so loss below ENet's own retransmits needs a hook
	// on the datagrams the transport receives.
	struct UdpTestAccess
	{
		// Drops a seeded share of the incoming datagrams (0 removes the hook).
		static void DropIncomingDatagrams(UdpTransport& transport, float share, uint32_t seed);
		// The next `count` incoming datagrams fail ENet's receive, as a socket error does.
		static void FailIncomingDatagrams(UdpTransport& transport, int count);
		// Closes ENet's socket under the transport, so every send and receive fails from then on.
		static void BreakSocket(UdpTransport& transport);
		static bool IsOpen(const UdpTransport& transport);
		// ENet peers in a connect that has raised no event yet.
		static int PendingPeers(const UdpTransport& transport);
		static size_t QueuedBytes(const UdpTransport& transport, PeerId peer);
		// A transport whose ENet host has `channels` channels (1 to DingoNet's 3), as a foreign end would.
		static std::unique_ptr<UdpTransport> Open(const std::string& address, uint16_t port, bool listen, size_t channels, std::string& error, const UdpParams& params = {});
	};

}
