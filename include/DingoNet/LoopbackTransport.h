#pragma once

#include "DingoNet/Transport.h"

#include <memory>
#include <string>
#include <vector>

namespace Dingo::Net
{

	namespace Internal
	{
		struct LoopbackLink;
		struct LoopbackListener;
	}

	// In-process transport for tests and for playing the whole net pipeline solo. Packets arrive in
	// the order sent, at the receiver's next Poll; nothing is lost (wrap it in a Conditioner for that).
	class LoopbackTransport final : public Transport
	{
	public:
		static std::unique_ptr<LoopbackTransport> Listen();
		// The host raises Connected at its next Poll, the client at its own; a closed host refuses.
		static std::unique_ptr<LoopbackTransport> Connect(LoopbackTransport& host);
		~LoopbackTransport() override;

		LoopbackTransport(const LoopbackTransport&) = delete;
		LoopbackTransport& operator=(const LoopbackTransport&) = delete;

		void Send(PeerId to, std::span<const uint8_t> data, Delivery delivery) override;
		void Poll(std::vector<Packet>& packets, std::vector<PeerEvent>& events) override;
		void Disconnect(PeerId peer) override;
		void Close() override;
		std::string Describe() const override;

	private:
		struct Peer
		{
			PeerId Id = 0;
			std::shared_ptr<Internal::LoopbackLink> Link;
			bool Announced = false;
		};

		LoopbackTransport() = default;
		void ClosePeer(Peer& peer);

	private:
		std::shared_ptr<Internal::LoopbackListener> m_Listener;
		bool m_IsHost = false;
		std::vector<Peer> m_Peers;
		PeerId m_NextId = 1;
	};

	struct LoopbackPair
	{
		std::unique_ptr<LoopbackTransport> Host;
		std::unique_ptr<LoopbackTransport> Client;
	};

	LoopbackPair CreateLoopbackPair();

}
