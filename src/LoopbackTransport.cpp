#include "DingoNet/LoopbackTransport.h"

#include "Internal/SendLimits.h"

#include <deque>

namespace Dingo::Net
{

	namespace Internal
	{

		// Side 0 is the host's end, side 1 the client's.
		struct LoopbackLink
		{
			std::deque<std::vector<uint8_t>> Inbox[2];
			bool Closed[2] = { false, false };
			bool Refused = false;
		};

		struct LoopbackListener
		{
			bool Open = true;
			std::vector<std::shared_ptr<LoopbackLink>> Pending;
		};

	}

	namespace
	{

		int Side(bool isHost)
		{
			return isHost ? 0 : 1;
		}

	}

	std::unique_ptr<LoopbackTransport> LoopbackTransport::Listen()
	{
		std::unique_ptr<LoopbackTransport> t(new LoopbackTransport());
		t->m_IsHost = true;
		t->m_Listener = std::make_shared<Internal::LoopbackListener>();
		return t;
	}

	std::unique_ptr<LoopbackTransport> LoopbackTransport::Connect(LoopbackTransport& host)
	{
		std::unique_ptr<LoopbackTransport> t(new LoopbackTransport());
		auto link = std::make_shared<Internal::LoopbackLink>();
		if (host.m_Listener && host.m_Listener->Open)
		{
			host.m_Listener->Pending.push_back(link);
		}
		else
		{
			link->Refused = true;
		}
		t->m_Peers.push_back({ t->m_NextId++, std::move(link), false });
		return t;
	}

	LoopbackTransport::~LoopbackTransport()
	{
		Close();
	}

	std::string LoopbackTransport::Describe() const
	{
		return m_IsHost ? "loopback host" : "loopback client";
	}

	void LoopbackTransport::Send(PeerId to, std::span<const uint8_t> data, Delivery delivery)
	{
		if (!Internal::CheckUnreliableSize(data.size(), delivery, "loopback"))
		{
			return;
		}
		for (Peer& peer : m_Peers)
		{
			if (peer.Id != to || !peer.Announced || peer.Link->Closed[Side(m_IsHost)])
			{
				continue;
			}
			peer.Link->Inbox[Side(!m_IsHost)].emplace_back(data.begin(), data.end());
			BytesOut += data.size();
			return;
		}
	}

	void LoopbackTransport::Poll(std::vector<Packet>& packets, std::vector<PeerEvent>& events)
	{
		if (m_Listener)
		{
			for (std::shared_ptr<Internal::LoopbackLink>& link : m_Listener->Pending)
			{
				m_Peers.push_back({ m_NextId++, std::move(link), false });
			}
			m_Listener->Pending.clear();
		}

		const int mine = Side(m_IsHost);
		const int theirs = Side(!m_IsHost);
		for (Peer& peer : m_Peers)
		{
			Internal::LoopbackLink& link = *peer.Link;
			if (link.Refused)
			{
				link.Closed[mine] = true;
				events.push_back({ PeerEvent::Kind::Disconnected, peer.Id, DisconnectReason::Refused, "the loopback host is closed" });
				continue;
			}
			if (!peer.Announced)
			{
				peer.Announced = true;
				events.push_back({ PeerEvent::Kind::Connected, peer.Id, DisconnectReason::Closed, {} });
			}
			while (!link.Inbox[mine].empty())
			{
				BytesIn += link.Inbox[mine].front().size();
				packets.push_back({ peer.Id, std::move(link.Inbox[mine].front()) });
				link.Inbox[mine].pop_front();
			}
			if (link.Closed[theirs])
			{
				link.Closed[mine] = true;
				events.push_back({ PeerEvent::Kind::Disconnected, peer.Id, DisconnectReason::Closed, "closed by the other end" });
			}
		}

		std::erase_if(m_Peers, [mine](const Peer& peer) { return peer.Link->Closed[mine]; });
	}

	void LoopbackTransport::ClosePeer(Peer& peer)
	{
		peer.Link->Closed[Side(m_IsHost)] = true;
		peer.Link->Inbox[Side(m_IsHost)].clear();
	}

	void LoopbackTransport::Disconnect(PeerId peer)
	{
		for (Peer& p : m_Peers)
		{
			if (p.Id == peer)
			{
				ClosePeer(p);
			}
		}
		std::erase_if(m_Peers, [peer](const Peer& p) { return p.Id == peer; });
	}

	void LoopbackTransport::Close()
	{
		for (Peer& peer : m_Peers)
		{
			ClosePeer(peer);
		}
		m_Peers.clear();
		if (m_Listener)
		{
			m_Listener->Open = false;
			for (std::shared_ptr<Internal::LoopbackLink>& link : m_Listener->Pending)
			{
				link->Closed[0] = true;
			}
			m_Listener->Pending.clear();
		}
	}

	LoopbackPair CreateLoopbackPair()
	{
		LoopbackPair pair;
		pair.Host = LoopbackTransport::Listen();
		pair.Client = LoopbackTransport::Connect(*pair.Host);
		return pair;
	}

}
