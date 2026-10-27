#pragma once

#include "DingoNet/Transport.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Dingo::Net
{

	namespace Internal
	{
		struct UdpTestAccess;
	}

	struct UdpParams
	{
		// A larger reliable packet is logged and dropped at the sender. ENet fragments and reassembles it.
		size_t MaxReliableBytes = 64 * 1024;
		// Live peers; a host past it never answers the connect. Room for a full Session (31 clients) and the 16
		// handshakes its host keeps at most.
		int MaxConnections = 64;
		// Payload taken from the socket per Poll; the rest waits for the next, so a flooding peer can't stall a frame.
		size_t ReadBudgetBytes = 256 * 1024;
		double ConnectTimeout = 5.0;
		// How long a connected peer may stay silent. Above SessionParams::Timeout, so the session's timeout fires first.
		double PeerTimeout = 8.0;
	};

	// UDP for LAN play without Steam, on ENet (bundled in the library, never in a header). Delivery::Reliable is
	// ordered (channel 0); Unreliable may be lost or reordered (channel 1); UnreliableSequenced may be lost but is
	// never delivered after a newer one from the same peer (channel 2). Unreliable packets stay within
	// MAX_UNRELIABLE_BYTES, below ENet's MTU, so none fragments. IPv4 only; no encryption, as with TcpTransport.
	// A connect that never completes is ConnectFailed (ENet has no explicit deny, so nothing is Refused); a peer
	// silent for PeerTimeout is TimedOut; its Disconnect() is Closed.
	// On Windows ENet raises the system timer resolution to 1 ms (timeBeginPeriod) for the whole process while a
	// UdpTransport is open.
	class UdpTransport final : public Transport
	{
	public:
		// Host: binds an IPv4 address (127.0.0.1 keeps the firewall quiet; 0.0.0.0 for a LAN). Port 0 lets the
		// system choose; GetPort() reports it.
		static std::unique_ptr<UdpTransport> Listen(const std::string& address, uint16_t port, std::string& error, const UdpParams& params = {});
		// Client: the connect finishes in a later Poll, as a Connected or Disconnected event.
		static std::unique_ptr<UdpTransport> Connect(const std::string& address, uint16_t port, std::string& error, const UdpParams& params = {});
		~UdpTransport() override;

		UdpTransport(const UdpTransport&) = delete;
		UdpTransport& operator=(const UdpTransport&) = delete;

		void Send(PeerId to, std::span<const uint8_t> data, Delivery delivery) override;
		void Poll(std::vector<Packet>& packets, std::vector<PeerEvent>& events) override;
		// What was queued for the peer still goes out; the peer is not told until then.
		void Disconnect(PeerId peer) override;
		// Disconnects everyone, services for at most 0.25 s so those messages leave, then the transport is inert.
		void Close() override;
		std::string Describe() const override { return m_Describe; }

		// The bound port for a host, the remote port for a client.
		uint16_t GetPort() const { return m_Port; }

	private:
		friend struct Internal::UdpTestAccess;

		struct Impl;

		explicit UdpTransport(const UdpParams& params);
		static std::unique_ptr<UdpTransport> Open(const std::string& address, uint16_t port, bool listen, std::string& error, const UdpParams& params);

	private:
		std::unique_ptr<Impl> m_Impl;
		uint16_t m_Port = 0;
		std::string m_Describe;
	};

}
