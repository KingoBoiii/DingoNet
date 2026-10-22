#include "DingoNet/Session.h"

#include "Internal/Log.h"
#include "SessionMessages.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

namespace Dingo::Net
{

	using namespace Internal;

	namespace
	{

		// A host hangs up on a peer after this many bad session packets.
		constexpr int MAX_BAD_PACKETS = 32;
		constexpr size_t MAX_BUILD_LENGTH = 64;

		double PositiveSeconds(double value, double fallback)
		{
			return std::isfinite(value) && value > 0.0 ? (std::min)(value, 1e6) : fallback;
		}

		std::string PrintableBuild(std::string_view build)
		{
			std::string out;
			for (const char c : build)
			{
				const auto code = static_cast<unsigned char>(c);
				if (out.size() < MAX_BUILD_LENGTH && code >= 0x20 && code <= 0x7E)
				{
					out += c;
				}
			}
			return out;
		}

		const char* ProblemText(int problem)
		{
			switch (problem)
			{
			case 0: return "malformed";
			case 1: return "unexpected";
			default: return "unknown";
			}
		}

		const std::string s_NoName;

		Session::Event MakeEvent(Session::Event::Kind type, int slot, std::string name, LeaveReason leave = LeaveReason::Bye, std::string detail = {})
		{
			Session::Event e;
			e.Type = type;
			e.Slot = slot;
			e.Name = std::move(name);
			e.Leave = leave;
			e.Detail = std::move(detail);
			return e;
		}

		Session::Event MakeRejected(RejectReason reason, std::string name, std::string detail)
		{
			Session::Event e = MakeEvent(Session::Event::Kind::Rejected, -1, std::move(name), LeaveReason::Bye, std::move(detail));
			e.Rejection = reason;
			return e;
		}

	}

	std::string DefaultNameFilter(std::string_view name)
	{
		constexpr size_t maxLength = 16;
		std::string out;
		for (const char c : name)
		{
			if (out.size() >= maxLength)
			{
				break;
			}
			const auto code = static_cast<unsigned char>(c);
			if (code < 0x20 || code > 0x7E || (code == ' ' && out.empty()))
			{
				continue;
			}
			out += c;
		}
		while (!out.empty() && out.back() == ' ')
		{
			out.pop_back();
		}
		return out.empty() ? std::string("Player") : out;
	}

	Session::Session(SessionParams params)
	{
		SetParams(std::move(params));
	}

	Session::~Session()
	{
		Stop();
	}

	void Session::SetParams(SessionParams params)
	{
		if (m_Transport)
		{
			LogError("session: SetParams on an active session is ignored; Stop it first");
			return;
		}
		if (params.MaxSlots < 1 || params.MaxSlots > MAX_SESSION_SLOTS)
		{
			LogWarn("session: MaxSlots {} is outside 1-{}; clamped", params.MaxSlots, MAX_SESSION_SLOTS);
			params.MaxSlots = std::clamp(params.MaxSlots, 1, MAX_SESSION_SLOTS);
		}
		params.Timeout = PositiveSeconds(params.Timeout, 5.0);
		params.PingInterval = PositiveSeconds(params.PingInterval, 1.0);
		params.HandshakeTimeout = PositiveSeconds(params.HandshakeTimeout, 5.0);
		if (params.PingInterval > params.Timeout / 2.0)
		{
			LogWarn("session: PingInterval {} s is over half the {} s Timeout; clamped to {} s", params.PingInterval, params.Timeout,
				params.Timeout / 2.0);
			params.PingInterval = params.Timeout / 2.0;
		}
		m_Params = std::move(params);
		m_Seats.assign(static_cast<size_t>(m_Params.MaxSlots), Seat{});
	}

	void Session::StartHost(std::unique_ptr<Transport> transport, std::string name)
	{
		Start(std::move(transport), std::move(name), true);
	}

	void Session::StartClient(std::unique_ptr<Transport> transport, std::string name)
	{
		Start(std::move(transport), std::move(name), false);
	}

	void Session::Start(std::unique_ptr<Transport> transport, std::string name, bool host)
	{
		Stop();
		m_Events.clear();
		m_Messages.clear();
		m_IsHost = host;
		m_Accepting = true;
		m_Name = FilterName(name);
		m_Now = 0.0;
		m_NextPing = 0.0;
		m_HandshakeStart = 0.0;
		m_RateStart = 0.0;
		m_InPerSec = 0.0f;
		m_OutPerSec = 0.0f;
		if (!transport)
		{
			LogError("session: started without a transport");
			return;
		}
		m_Transport = std::move(transport);
		m_StartTime = m_Params.Clock ? m_Params.Clock() : std::chrono::steady_clock::now();
		m_RateIn = m_Transport->BytesIn;
		m_RateOut = m_Transport->BytesOut;
		if (host)
		{
			m_LocalSlot = 0;
			m_Seats[0] = { true, m_Name, 0, 0.0f, false };
		}
	}

	void Session::Stop()
	{
		if (!m_Transport)
		{
			return;
		}
		const std::vector<uint8_t> bye = EncodeBye(LeaveReason::Bye);
		for (const Peer& p : m_Peers)
		{
			m_Transport->Send(p.Id, bye, Delivery::Reliable);
			m_Transport->Disconnect(p.Id);
		}
		Shutdown();
	}

	void Session::Shutdown()
	{
		if (m_Transport)
		{
			m_Transport->Close();
			m_Transport.reset();
		}
		m_Peers.clear();
		m_Seats.assign(static_cast<size_t>(m_Params.MaxSlots), Seat{});
		m_LocalSlot = -1;
	}

	void Session::Kick(int slot)
	{
		if (!m_Transport || !m_IsHost)
		{
			LogWarn("session: only an active host can kick");
			return;
		}
		if (slot < 1 || slot >= MaxSlots() || !m_Seats[slot].Taken)
		{
			LogWarn("session: no client in slot {} to kick", slot);
			return;
		}
		const PeerId id = m_Seats[slot].Connection;
		m_Transport->Send(id, EncodeBye(LeaveReason::Kicked), Delivery::Reliable);
		DropPeer(id, LeaveReason::Kicked);
	}

	double Session::Elapsed() const
	{
		const auto now = m_Params.Clock ? m_Params.Clock() : std::chrono::steady_clock::now();
		const double seconds = std::chrono::duration<double>(now - m_StartTime).count();
		return std::isfinite(seconds) ? seconds : 0.0;
	}

	uint32_t Session::Ms(double seconds) const
	{
		return static_cast<uint32_t>(static_cast<uint64_t>((std::max)(seconds, 0.0) * 1000.0));
	}

	std::string Session::FilterName(std::string_view name) const
	{
		return m_Params.NameFilter ? m_Params.NameFilter(name) : DefaultNameFilter(name);
	}

	std::string Session::DescribeVersions(uint16_t sessionProtocol, uint16_t protocol, uint32_t contentHash, std::string_view build) const
	{
		return std::format("theirs: session {} protocol {} content {:08X} build \"{}\"; ours: session {} protocol {} content {:08X} build \"{}\"",
			sessionProtocol, protocol, contentHash, PrintableBuild(build), k_SessionProtocol, m_Params.Protocol, m_Params.ContentHash,
			PrintableBuild(m_Params.Build));
	}

	Session::Peer* Session::FindPeer(PeerId id)
	{
		for (Peer& p : m_Peers)
		{
			if (p.Id == id)
			{
				return &p;
			}
		}
		return nullptr;
	}

	bool Session::ValidClientSlot(int slot) const
	{
		return slot >= 1 && slot < MaxSlots() && slot != m_LocalSlot;
	}

	void Session::Bad(Peer& peer, Problem problem, uint8_t id)
	{
		const int kind = static_cast<int>(problem);
		const uint64_t bit = uint64_t{ 1 } << (kind * 16 + (id - FIRST_SESSION_MESSAGE));
		peer.BadPackets++;
		if ((peer.Warned & bit) == 0)
		{
			peer.Warned |= bit;
			LogWarn("session: {} session message 0x{:02X} from peer {}; dropped (more like it are counted, not logged)", ProblemText(kind),
				id, peer.Id);
		}
		if (m_IsHost && peer.BadPackets >= MAX_BAD_PACKETS)
		{
			const PeerId bad = peer.Id;
			LogWarn("session: hung up on peer {} after {} bad session packets", bad, peer.BadPackets);
			m_Transport->Send(bad, EncodeBye(LeaveReason::Kicked), Delivery::Reliable);
			DropPeer(bad, LeaveReason::Kicked, std::format("{} bad session packets", MAX_BAD_PACKETS));
		}
	}

	void Session::OnConnected(PeerId id, double now)
	{
		if (FindPeer(id))
		{
			return;
		}
		if (!m_IsHost && !m_Peers.empty())
		{
			LogWarn("session: a client's transport reported a second peer ({}); ignored", id);
			return;
		}
		m_Peers.push_back({ id, -1, now, now, 0, 0 });
		if (!m_IsHost)
		{
			m_HandshakeStart = now;
			const HelloMessage hello{ k_SessionProtocol, m_Params.Protocol, m_Params.ContentHash, m_Name, m_Params.Build };
			m_Transport->Send(id, Encode(hello), Delivery::Reliable);
		}
	}

	bool Session::OnDisconnected(const PeerEvent& e)
	{
		if (m_IsHost)
		{
			DropPeer(e.Peer, LeaveReason::TransportLost, e.Detail, e.Reason);
			return true;
		}
		if (m_Peers.empty() || FindPeer(e.Peer))
		{
			Lose(LeaveReason::TransportLost, e.Detail, e.Reason);
			return false;
		}
		return true;
	}

	void Session::Update()
	{
		if (!m_Transport)
		{
			return;
		}
		m_Now = (std::max)(m_Now, Elapsed());
		const double now = m_Now;

		std::vector<Packet> packets;
		std::vector<PeerEvent> events;
		m_Transport->Poll(packets, events);

		// Events in the transport's order, except that a plain disconnect waits until the packets are read, so a
		// Reject or Bye that came with the hang-up still counts. A disconnect followed by a connect of the same id
		// (a SteamID coming back) is done at once, so the id starts over as a fresh peer.
		std::vector<const PeerEvent*> deferred;
		for (size_t i = 0; i < events.size(); i++)
		{
			const PeerEvent& e = events[i];
			if (e.Type == PeerEvent::Kind::Connected)
			{
				OnConnected(e.Peer, now);
				continue;
			}
			const bool back = std::any_of(events.begin() + static_cast<std::ptrdiff_t>(i) + 1, events.end(),
				[&e](const PeerEvent& later) { return later.Type == PeerEvent::Kind::Connected && later.Peer == e.Peer; });
			if (!back)
			{
				deferred.push_back(&e);
			}
			else if (!OnDisconnected(e))
			{
				return;
			}
		}

		std::vector<PeerId> strays;
		for (Packet& packet : packets)
		{
			Peer* peer = FindPeer(packet.From);
			if (!peer)
			{
				// A peer we never saw connect or already let go: make sure the transport lets it go too.
				if (m_IsHost && std::find(strays.begin(), strays.end(), packet.From) == strays.end())
				{
					strays.push_back(packet.From);
					m_Transport->Disconnect(packet.From);
				}
				continue;
			}
			peer->LastHeard = now;
			OnPacket(*peer, packet);
			if (!m_Transport)
			{
				return;
			}
		}

		for (const PeerEvent* e : deferred)
		{
			if (!OnDisconnected(*e))
			{
				return;
			}
		}

		if (now >= m_NextPing)
		{
			m_NextPing = now + m_Params.PingInterval;
			const std::vector<uint8_t> ping = EncodePing(Ms(now));
			for (const Peer& p : m_Peers)
			{
				if (p.Slot >= 0 || !m_IsHost)
				{
					m_Transport->Send(p.Id, ping, Delivery::Unreliable);
				}
			}
		}

		if (m_IsHost)
		{
			// Pings alone don't keep a connection that never said Hello.
			std::vector<PeerId> late;
			for (const Peer& p : m_Peers)
			{
				const bool gone = p.Slot >= 0 ? now - p.LastHeard > m_Params.Timeout : now - p.ConnectedAt > m_Params.HandshakeTimeout;
				if (gone)
				{
					late.push_back(p.Id);
				}
			}
			for (PeerId id : late)
			{
				if (FindPeer(id)->Slot < 0)
				{
					LogInfo("session: dropped peer {}: no Hello within {} s", id, m_Params.HandshakeTimeout);
				}
				// A transport whose Disconnect is silent on the wire (Steam) would otherwise leave the peer
				// thinking it is still in.
				m_Transport->Send(id, EncodeBye(LeaveReason::TimedOut), Delivery::Reliable);
				DropPeer(id, LeaveReason::TimedOut, std::format("silent for over {} s", m_Params.Timeout));
			}
		}
		else if (Connected() ? now - m_Peers.front().LastHeard > m_Params.Timeout : now - m_HandshakeStart > m_Params.HandshakeTimeout)
		{
			Lose(LeaveReason::TimedOut, Connected() ? std::format("the host was silent for over {} s", m_Params.Timeout)
				: std::format("no Welcome within {} s", m_Params.HandshakeTimeout));
			return;
		}

		if (now - m_RateStart >= 1.0)
		{
			const double span = now - m_RateStart;
			m_InPerSec = static_cast<float>(static_cast<double>(m_Transport->BytesIn - m_RateIn) / span);
			m_OutPerSec = static_cast<float>(static_cast<double>(m_Transport->BytesOut - m_RateOut) / span);
			m_RateIn = m_Transport->BytesIn;
			m_RateOut = m_Transport->BytesOut;
			m_RateStart = now;
		}
	}

	void Session::OnPacket(Peer& peer, Packet& packet)
	{
		const std::span<const uint8_t> data = packet.Data;
		if (data.empty() || !IsSessionMessage(data[0]))
		{
			if (m_IsHost && peer.Slot < 0)
			{
				OnUnseatedPacket(peer, data);
				return;
			}
			if (!m_IsHost && !Connected())
			{
				return;
			}
			m_Messages.push_back({ m_IsHost ? peer.Slot : 0, std::move(packet.Data) });
			return;
		}

		switch (static_cast<SessionMessage>(data[0]))
		{
		case SessionMessage::Hello:
			OnHello(peer, data);
			return;
		case SessionMessage::Welcome:
			OnWelcome(peer, data);
			return;
		case SessionMessage::Reject:
			OnReject(peer, data);
			return;
		case SessionMessage::Ping:
		{
			uint32_t time = 0;
			if (!DecodePing(data, SessionMessage::Ping, time))
			{
				Bad(peer, Problem::Malformed, data[0]);
				return;
			}
			// Over Steam a client we timed out comes back as a new, unseated connection: a Pong would keep it
			// believing it still has its seat.
			if (!m_IsHost || peer.Slot >= 0)
			{
				m_Transport->Send(peer.Id, EncodePong(time), Delivery::Unreliable);
			}
			return;
		}
		case SessionMessage::Pong:
			OnPong(peer, data);
			return;
		case SessionMessage::Bye:
			OnBye(peer, data);
			return;
		case SessionMessage::SeatJoined:
			OnSeatJoined(peer, data);
			return;
		case SessionMessage::SeatLeft:
			OnSeatLeft(peer, data);
			return;
		}
		Bad(peer, Problem::Unknown, data[0]);
	}

	void Session::OnUnseatedPacket(Peer& peer, std::span<const uint8_t> data)
	{
		if (!m_Params.UnseatedPacket)
		{
			return;
		}
		std::optional<std::vector<uint8_t>> reply = m_Params.UnseatedPacket(data);
		if (!reply)
		{
			return;
		}
		const PeerId id = peer.Id;
		LogInfo("session: answered peer {}'s packet before a Hello ({} bytes) and hung up", id, data.size());
		m_Transport->Send(id, *reply, Delivery::Reliable);
		m_Transport->Disconnect(id);
		std::erase_if(m_Peers, [id](const Peer& p) { return p.Id == id; });
	}

	void Session::OnHello(Peer& peer, std::span<const uint8_t> data)
	{
		if (!m_IsHost || peer.Slot >= 0)
		{
			Bad(peer, Problem::Unexpected, data[0]);
			return;
		}
		uint16_t sessionProtocol = 0;
		if (!DecodeHelloSessionProtocol(data, sessionProtocol))
		{
			Bad(peer, Problem::Malformed, data[0]);
			return;
		}
		if (sessionProtocol != k_SessionProtocol)
		{
			RejectPeer(peer.Id, RejectReason::ProtocolMismatch, {},
				std::format("theirs: session {}; ours: session {}", sessionProtocol, k_SessionProtocol));
			return;
		}
		HelloMessage hello;
		if (!Decode(data, hello))
		{
			Bad(peer, Problem::Malformed, data[0]);
			return;
		}

		std::string name = FilterName(hello.Name);
		int slot = -1;
		for (int i = 1; i < MaxSlots() && slot < 0; i++)
		{
			if (!m_Seats[i].Taken)
			{
				slot = i;
			}
		}
		std::optional<RejectReason> reason;
		if (hello.Protocol != m_Params.Protocol)
		{
			reason = RejectReason::ProtocolMismatch;
		}
		else if (hello.ContentHash != m_Params.ContentHash)
		{
			reason = RejectReason::ContentMismatch;
		}
		else if (!m_Accepting)
		{
			reason = RejectReason::NotAccepting;
		}
		else if (slot < 0)
		{
			reason = RejectReason::Full;
		}
		if (reason)
		{
			RejectPeer(peer.Id, *reason, std::move(name), DescribeVersions(hello.SessionProtocol, hello.Protocol, hello.ContentHash, hello.Build));
			return;
		}

		peer.Slot = slot;
		m_Seats[slot] = { true, name, peer.Id, 0.0f, false };
		WelcomeMessage welcome{ static_cast<uint8_t>(slot), static_cast<uint8_t>(MaxSlots()), m_Seats[0].Name, {} };
		for (int i = 1; i < MaxSlots(); i++)
		{
			if (i != slot && m_Seats[i].Taken)
			{
				welcome.Others.push_back({ static_cast<uint8_t>(i), m_Seats[i].Name });
			}
		}
		m_Transport->Send(peer.Id, Encode(welcome), Delivery::Reliable);
		SendToSeated(Encode(SeatEntry{ static_cast<uint8_t>(slot), name }), slot);
		m_Events.push_back(MakeEvent(Event::Kind::Joined, slot, std::move(name)));
	}

	void Session::OnWelcome(Peer& peer, std::span<const uint8_t> data)
	{
		if (m_IsHost || Connected())
		{
			Bad(peer, Problem::Unexpected, data[0]);
			return;
		}
		WelcomeMessage welcome;
		bool valid = Decode(data, welcome) && welcome.MaxSlots >= 2 && welcome.MaxSlots <= MAX_SESSION_SLOTS && welcome.Slot >= 1
			&& welcome.Slot < welcome.MaxSlots;
		uint32_t seen = valid ? 1u << welcome.Slot : 0u;
		for (size_t i = 0; valid && i < welcome.Others.size(); i++)
		{
			const uint8_t other = welcome.Others[i].Slot;
			valid = other >= 1 && other < welcome.MaxSlots && (seen & (1u << other)) == 0;
			seen |= valid ? 1u << other : 0u;
		}
		if (!valid)
		{
			Bad(peer, Problem::Malformed, data[0]);
			return;
		}
		if (welcome.MaxSlots > m_Params.MaxSlots)
		{
			m_Events.push_back(MakeRejected(RejectReason::ProtocolMismatch, {},
				std::format("the host has {} seats, ours at most {}", welcome.MaxSlots, m_Params.MaxSlots)));
			Stop();
			return;
		}

		m_Seats.assign(welcome.MaxSlots, Seat{});
		peer.Slot = 0;
		m_LocalSlot = welcome.Slot;
		m_Seats[0] = { true, FilterName(welcome.HostName), peer.Id, 0.0f, false };
		m_Seats[welcome.Slot] = { true, m_Name, 0, 0.0f, false };
		m_Events.push_back(MakeEvent(Event::Kind::Welcomed, welcome.Slot, m_Seats[0].Name));
		for (const SeatEntry& other : welcome.Others)
		{
			m_Seats[other.Slot] = { true, FilterName(other.Name), 0, 0.0f, false };
			Event joined = MakeEvent(Event::Kind::Joined, other.Slot, m_Seats[other.Slot].Name);
			joined.Existing = true;
			m_Events.push_back(std::move(joined));
		}
	}

	void Session::OnReject(Peer& peer, std::span<const uint8_t> data)
	{
		if (m_IsHost || Connected())
		{
			Bad(peer, Problem::Unexpected, data[0]);
			return;
		}
		RejectMessage reject;
		if (!Decode(data, reject))
		{
			Bad(peer, Problem::Malformed, data[0]);
			return;
		}
		m_Events.push_back(MakeRejected(reject.Reason, {}, DescribeVersions(reject.SessionProtocol, reject.Protocol, reject.ContentHash, reject.Build)));
		Shutdown();
	}

	void Session::OnPong(Peer& peer, std::span<const uint8_t> data)
	{
		uint32_t sent = 0;
		if (!DecodePing(data, SessionMessage::Pong, sent))
		{
			Bad(peer, Problem::Malformed, data[0]);
			return;
		}
		const int slot = m_IsHost ? peer.Slot : (Connected() ? 0 : -1);
		const uint32_t nowMs = Ms(m_Now);
		if (slot < 0 || sent > nowMs)
		{
			return;
		}
		Seat& seat = m_Seats[slot];
		const float rtt = static_cast<float>(nowMs - sent);
		seat.RttMs = seat.HasRtt ? seat.RttMs + (rtt - seat.RttMs) * 0.3f : rtt;
		seat.HasRtt = true;
	}

	void Session::OnBye(Peer& peer, std::span<const uint8_t> data)
	{
		LeaveReason reason = LeaveReason::Bye;
		if (!DecodeBye(data, reason) || reason == LeaveReason::TransportLost)
		{
			Bad(peer, Problem::Malformed, data[0]);
			return;
		}
		if (!m_IsHost)
		{
			Lose(reason, reason == LeaveReason::TimedOut ? std::string("the host stopped hearing from us") : std::string());
		}
		else if (peer.Slot >= 0)
		{
			DropPeer(peer.Id, LeaveReason::Bye);
		}
	}

	void Session::OnSeatJoined(Peer& peer, std::span<const uint8_t> data)
	{
		if (m_IsHost || !Connected())
		{
			Bad(peer, Problem::Unexpected, data[0]);
			return;
		}
		SeatEntry joined;
		if (!Decode(data, joined) || !ValidClientSlot(joined.Slot) || m_Seats[joined.Slot].Taken)
		{
			Bad(peer, Problem::Malformed, data[0]);
			return;
		}
		m_Seats[joined.Slot] = { true, FilterName(joined.Name), 0, 0.0f, false };
		m_Events.push_back(MakeEvent(Event::Kind::Joined, joined.Slot, m_Seats[joined.Slot].Name));
	}

	void Session::OnSeatLeft(Peer& peer, std::span<const uint8_t> data)
	{
		if (m_IsHost || !Connected())
		{
			Bad(peer, Problem::Unexpected, data[0]);
			return;
		}
		SeatLeftMessage left;
		if (!Decode(data, left) || !ValidClientSlot(left.Slot) || !m_Seats[left.Slot].Taken)
		{
			Bad(peer, Problem::Malformed, data[0]);
			return;
		}
		std::string name = std::move(m_Seats[left.Slot].Name);
		m_Seats[left.Slot] = {};
		m_Events.push_back(MakeEvent(Event::Kind::Left, left.Slot, std::move(name), left.Reason));
	}

	void Session::RejectPeer(PeerId id, RejectReason reason, std::string name, std::string detail)
	{
		const RejectMessage reject{ reason, k_SessionProtocol, m_Params.Protocol, m_Params.ContentHash, m_Params.Build };
		m_Transport->Send(id, Encode(reject), Delivery::Reliable);
		m_Transport->Disconnect(id);
		std::erase_if(m_Peers, [id](const Peer& p) { return p.Id == id; });
		m_Events.push_back(MakeRejected(reason, std::move(name), std::move(detail)));
	}

	void Session::DropPeer(PeerId id, LeaveReason reason, std::string detail, DisconnectReason transport)
	{
		const Peer* peer = FindPeer(id);
		if (!peer)
		{
			return;
		}
		const int slot = peer->Slot;
		// The transport already reported this one lost.
		if (reason != LeaveReason::TransportLost)
		{
			m_Transport->Disconnect(id);
		}
		std::erase_if(m_Peers, [id](const Peer& p) { return p.Id == id; });
		if (slot < 0)
		{
			return;
		}
		std::erase_if(m_Messages, [slot](const Message& m) { return m.Slot == slot; });
		std::string name = std::move(m_Seats[slot].Name);
		m_Seats[slot] = {};
		SendToSeated(Encode(SeatLeftMessage{ static_cast<uint8_t>(slot), reason }), -1);
		Event left = MakeEvent(Event::Kind::Left, slot, std::move(name), reason, std::move(detail));
		left.Transport = transport;
		m_Events.push_back(std::move(left));
	}

	void Session::Lose(LeaveReason reason, std::string detail, DisconnectReason transport)
	{
		Event lost = MakeEvent(Event::Kind::Lost, m_LocalSlot, Connected() ? m_Seats[0].Name : std::string(), reason, std::move(detail));
		lost.Transport = transport;
		m_Events.push_back(std::move(lost));
		Shutdown();
	}

	void Session::SendToSeated(std::span<const uint8_t> data, int exceptSlot)
	{
		for (int slot = 1; slot < MaxSlots(); slot++)
		{
			if (slot != exceptSlot && m_Seats[slot].Taken)
			{
				m_Transport->Send(m_Seats[slot].Connection, data, Delivery::Reliable);
			}
		}
	}

	void Session::Send(int slot, std::span<const uint8_t> data, Delivery delivery)
	{
		if (!m_Transport)
		{
			return;
		}
		if (!data.empty() && IsSessionMessage(data[0]))
		{
			LogError("session: message id 0x{:02X} is in the session's range (0xF0-0xFF); dropped", data[0]);
			return;
		}
		if (slot < 0 || slot >= MaxSlots())
		{
			return;
		}
		if (m_IsHost)
		{
			if (slot != 0 && m_Seats[slot].Taken)
			{
				m_Transport->Send(m_Seats[slot].Connection, data, delivery);
			}
		}
		else if (slot == 0 && Connected())
		{
			m_Transport->Send(m_Seats[0].Connection, data, delivery);
		}
	}

	void Session::Broadcast(std::span<const uint8_t> data, Delivery delivery, int exceptSlot)
	{
		if (!m_IsHost)
		{
			Send(0, data, delivery);
			return;
		}
		if (!m_Transport)
		{
			return;
		}
		if (!data.empty() && IsSessionMessage(data[0]))
		{
			LogError("session: message id 0x{:02X} is in the session's range (0xF0-0xFF); dropped", data[0]);
			return;
		}
		for (int slot = 1; slot < MaxSlots(); slot++)
		{
			if (slot != exceptSlot && m_Seats[slot].Taken)
			{
				m_Transport->Send(m_Seats[slot].Connection, data, delivery);
			}
		}
	}

	int Session::SeatedCount() const
	{
		return static_cast<int>(std::count_if(m_Seats.begin(), m_Seats.end(), [](const Seat& seat) { return seat.Taken; }));
	}

	bool Session::SlotTaken(int slot) const
	{
		return slot >= 0 && slot < MaxSlots() && m_Seats[slot].Taken;
	}

	const std::string& Session::NameOf(int slot) const
	{
		return slot >= 0 && slot < MaxSlots() ? m_Seats[slot].Name : s_NoName;
	}

	float Session::RttMs(int slot) const
	{
		return slot >= 0 && slot < MaxSlots() ? m_Seats[slot].RttMs : 0.0f;
	}

	std::string Session::Describe() const
	{
		if (!m_Transport)
		{
			return "offline";
		}
		std::string rtt;
		for (int slot = 0; slot < MaxSlots(); slot++)
		{
			if (m_Seats[slot].Taken && m_Seats[slot].HasRtt)
			{
				rtt += std::format(" {}:{:.0f}ms", slot, m_Seats[slot].RttMs);
			}
		}
		return std::format("{} | slot {} | {} seated | rtt{} | in {:.1f} KB/s out {:.1f} KB/s", m_Transport->Describe(), m_LocalSlot,
			SeatedCount(), rtt.empty() ? " -" : rtt, m_InPerSec / 1024.0f, m_OutPerSec / 1024.0f);
	}

}
