#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Dingo::Net
{

	// A connection on the other end: a TCP connection index, a loopback link, or a SteamID.
	using PeerId = uint64_t;

	enum class Delivery : uint8_t
	{
		Reliable,
		Unreliable,
		// May be lost; never delivered after a newer UnreliableSequenced packet from the same peer. Sequencing is
		// per peer across every such packet, so it suits one stream per peer, not several independent message types.
		// TCP, loopback and Steam deliver in order anyway, so there it behaves as Unreliable.
		UnreliableSequenced
	};

	constexpr bool IsUnreliable(Delivery delivery)
	{
		return delivery != Delivery::Reliable;
	}

	// Steam drops a whole fragmented unreliable message, so no transport carries a bigger one.
	inline constexpr size_t MAX_UNRELIABLE_BYTES = 1100;

	struct Packet
	{
		PeerId From = 0;
		std::vector<uint8_t> Data;
	};

	// Refused: the other end turned the connect away. ConnectFailed: it never connected for any
	// other reason (unreachable, no answer in time). Closed: an orderly close by the other end.
	enum class DisconnectReason : uint8_t
	{
		Closed,
		TimedOut,
		Refused,
		Reset,
		ConnectFailed,
		Error
	};

	struct PeerEvent
	{
		enum class Kind : uint8_t
		{
			Connected,
			Disconnected
		};

		Kind Type = Kind::Connected;
		PeerId Peer = 0;
		DisconnectReason Reason = DisconnectReason::Closed;
		// For logs only: players read text the game owns, keyed by Reason.
		std::string Detail;
	};

	// Moves bytes between machines. A host sees many peers; a client sees one, the host. Everything is
	// pumped by Poll() once a frame: no threads. A peer we Disconnect() raises no event on our side.
	class Transport
	{
	public:
		virtual ~Transport() = default;

		virtual void Send(PeerId to, std::span<const uint8_t> data, Delivery delivery) = 0;
		// Flushes what's queued, then appends what arrived and connection changes.
		virtual void Poll(std::vector<Packet>& packets, std::vector<PeerEvent>& events) = 0;
		// A packet already sent to the peer still goes out before the close.
		virtual void Disconnect(PeerId peer) = 0;
		virtual void Close() = 0;
		// For logs: "tcp host 127.0.0.1:27960", "loopback host".
		virtual std::string Describe() const = 0;

		uint64_t BytesIn = 0;
		uint64_t BytesOut = 0;
	};

}
