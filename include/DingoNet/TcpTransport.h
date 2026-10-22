#pragma once

#include "DingoNet/Transport.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Dingo::Net
{

	struct TcpParams
	{
		size_t MaxFrameBytes = 64 * 1024;
		// A peer that stops reading is dropped once this much waits for it.
		size_t MaxQueuedBytes = 4 * 1024 * 1024;
		// Live connections; those we Disconnect stop counting at once. Room for a full Session (31 clients) and
		// the 16 handshakes its host keeps at most; a session needs at least MaxSlots - 1 + 16, or a joiner past
		// it is hung up on instead of turned away as Full. Also caps the sockets still lingering after our Disconnect:
		// past it, the one lingering longest is closed at once.
		size_t MaxConnections = 64;
		// Read from one connection per Poll; the rest waits for the next, so a flooding peer can't stall a frame.
		size_t MaxReadBytesPerPoll = 256 * 1024;
		double ConnectTimeout = 5.0;
		// After our Disconnect: how long to flush what is queued and wait for the other side's FIN.
		double CloseLinger = 2.0;
	};

	// Plain TCP for LAN play and tests. Each packet is a frame behind a 4-byte little-endian length,
	// wire-compatible with Headstone's. "Unreliable" rides the same stream; wrap the transport in a
	// Conditioner to fake a lossy network.
	class TcpTransport final : public Transport
	{
	public:
		// Host: listens on an IPv4 address (127.0.0.1 keeps the firewall quiet; 0.0.0.0 for a LAN).
		// Port 0 lets the system choose; GetPort() reports it.
		static std::unique_ptr<TcpTransport> Listen(const std::string& address, uint16_t port, std::string& error, const TcpParams& params = {});
		// Client: the connect finishes in a later Poll, as a Connected or Disconnected event.
		static std::unique_ptr<TcpTransport> Connect(const std::string& address, uint16_t port, std::string& error, const TcpParams& params = {});
		~TcpTransport() override;

		TcpTransport(const TcpTransport&) = delete;
		TcpTransport& operator=(const TcpTransport&) = delete;

		void Send(PeerId to, std::span<const uint8_t> data, Delivery delivery) override;
		void Poll(std::vector<Packet>& packets, std::vector<PeerEvent>& events) override;
		void Disconnect(PeerId peer) override;
		void Close() override;
		std::string Describe() const override { return m_Describe; }

		// The listening port for a host, the remote port for a client.
		uint16_t GetPort() const { return m_Port; }

	private:
		struct Connection;

		explicit TcpTransport(const TcpParams& params);
		void AcceptPending(std::vector<PeerEvent>& events);
		void CloseConnection(Connection& c, DisconnectReason reason, std::string detail);
		// Sends what the socket takes; false when a hard error closed the connection.
		bool Flush(Connection& c);
		void PollClosing(Connection& c, std::chrono::steady_clock::time_point now);

	private:
		TcpParams m_Params;
		uint64_t m_Listen;
		bool m_Listening = false;
		bool m_LibraryAcquired = false;
		std::vector<std::unique_ptr<Connection>> m_Connections;
		PeerId m_NextId = 1;
		uint16_t m_Port = 0;
		std::string m_Describe;
	};

}
