#include "TestFramework.h"
#include "TransportTestUtils.h"

#include <DingoNet/Bytes.h>
#include <DingoNet/LoopbackTransport.h>
#include <DingoNet/Session.h>
#include <DingoNet/TcpTransport.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Dingo::Net;
using TransportTest::Endpoint;
using TransportTest::LogCapture;

namespace
{

	using Kind = Session::Event::Kind;
	using Bytes = std::vector<uint8_t>;

	constexpr uint16_t PROTOCOL = 20;
	constexpr uint32_t CONTENT = 0x11223344;
	constexpr int ANY_SLOT = -2;

	struct FakeClock
	{
		std::chrono::steady_clock::time_point Now{};

		void Advance(std::chrono::milliseconds step)
		{
			Now += step;
		}
	};

	SessionParams MakeParams(FakeClock* clock, int maxSlots = 4)
	{
		SessionParams params;
		params.MaxSlots = maxSlots;
		params.Protocol = PROTOCOL;
		params.ContentHash = CONTENT;
		params.Build = "test";
		if (clock)
		{
			params.Clock = [clock] { return clock->Now; };
		}
		return params;
	}

	struct Node
	{
		explicit Node(SessionParams params) : S(std::move(params)) {}

		Session S;
		std::vector<Session::Event> Events;
		std::vector<Session::Message> Messages;

		void Update()
		{
			S.Update();
			for (Session::Event& e : S.TakeEvents())
			{
				Events.push_back(std::move(e));
			}
			for (Session::Message& m : S.TakeMessages())
			{
				Messages.push_back(std::move(m));
			}
		}

		const Session::Event* Find(Kind kind, int slot = ANY_SLOT) const
		{
			for (const Session::Event& e : Events)
			{
				if (e.Type == kind && (slot == ANY_SLOT || e.Slot == slot))
				{
					return &e;
				}
			}
			return nullptr;
		}

		int Count(Kind kind) const
		{
			return static_cast<int>(std::count_if(Events.begin(), Events.end(), [kind](const Session::Event& e) { return e.Type == kind; }));
		}

		void Clear()
		{
			Events.clear();
			Messages.clear();
		}
	};

	// A host session over loopback and the clients that join it.
	struct Lan
	{
		explicit Lan(const SessionParams& params) : Params(params), Host(std::make_unique<Node>(params))
		{
			std::unique_ptr<LoopbackTransport> listen = LoopbackTransport::Listen();
			Link = listen.get();
			Host->S.StartHost(std::move(listen), "Host");
		}

		Node& Join(std::string name, const SessionParams& params)
		{
			Clients.push_back(std::make_unique<Node>(params));
			Clients.back()->S.StartClient(LoopbackTransport::Connect(*Link), std::move(name));
			return *Clients.back();
		}

		Node& Join(std::string name)
		{
			return Join(std::move(name), Params);
		}

		void Pump(int rounds = 4)
		{
			for (int i = 0; i < rounds; i++)
			{
				Host->Update();
				for (std::unique_ptr<Node>& client : Clients)
				{
					client->Update();
				}
			}
		}

		SessionParams Params;
		std::unique_ptr<Node> Host;
		LoopbackTransport* Link = nullptr;
		std::vector<std::unique_ptr<Node>> Clients;
	};

	Bytes Hello(uint16_t sessionProtocol, uint16_t protocol, uint32_t content, std::string_view name)
	{
		ByteWriter w;
		w.U8(0xF0);
		w.U16(sessionProtocol);
		w.U16(protocol);
		w.U32(content);
		w.Str(name);
		w.Str("raw");
		return w.Data();
	}

	Bytes Welcome(uint8_t slot, uint8_t maxSlots, std::vector<std::pair<uint8_t, std::string>> others = {})
	{
		ByteWriter w;
		w.U8(0xF1);
		w.U8(slot);
		w.U8(maxSlots);
		w.Str("Host");
		w.U8(static_cast<uint8_t>(others.size()));
		for (const auto& [otherSlot, name] : others)
		{
			w.U8(otherSlot);
			w.Str(name);
		}
		return w.Data();
	}

	const Packet* FindPacket(const Endpoint& endpoint, uint8_t id)
	{
		for (const Packet& p : endpoint.Packets)
		{
			if (!p.Data.empty() && p.Data[0] == id)
			{
				return &p;
			}
		}
		return nullptr;
	}

	void SendAll(Transport& transport, PeerId to, const std::vector<Bytes>& packets)
	{
		for (const Bytes& packet : packets)
		{
			transport.Send(to, packet, Delivery::Reliable);
		}
	}

	PeerEvent Connect(PeerId peer)
	{
		PeerEvent e;
		e.Type = PeerEvent::Kind::Connected;
		e.Peer = peer;
		return e;
	}

	PeerEvent Disconnect(PeerId peer, DisconnectReason reason, std::string detail)
	{
		PeerEvent e;
		e.Type = PeerEvent::Kind::Disconnected;
		e.Peer = peer;
		e.Reason = reason;
		e.Detail = std::move(detail);
		return e;
	}

	// What the next Poll returns, and what the session did; the test keeps it while the session owns the transport.
	struct Script
	{
		std::vector<Packet> Packets;
		std::vector<PeerEvent> Events;
		std::vector<std::pair<PeerId, Bytes>> Sent;
		std::vector<PeerId> Disconnects;

		int DisconnectsOf(PeerId peer) const
		{
			return static_cast<int>(std::count(Disconnects.begin(), Disconnects.end(), peer));
		}

		bool SentTo(PeerId peer, const Bytes& data) const
		{
			return std::find(Sent.begin(), Sent.end(), std::make_pair(peer, data)) != Sent.end();
		}
	};

	class ScriptedTransport final : public Transport
	{
	public:
		explicit ScriptedTransport(std::shared_ptr<Script> script) : m_Script(std::move(script)) {}

		void Send(PeerId to, std::span<const uint8_t> data, Delivery) override
		{
			m_Script->Sent.push_back({ to, Bytes(data.begin(), data.end()) });
		}

		void Poll(std::vector<Packet>& packets, std::vector<PeerEvent>& events) override
		{
			for (Packet& p : std::exchange(m_Script->Packets, {}))
			{
				packets.push_back(std::move(p));
			}
			for (PeerEvent& e : std::exchange(m_Script->Events, {}))
			{
				events.push_back(std::move(e));
			}
		}

		void Disconnect(PeerId peer) override
		{
			m_Script->Disconnects.push_back(peer);
		}

		void Close() override {}

		std::string Describe() const override
		{
			return "scripted";
		}

	private:
		std::shared_ptr<Script> m_Script;
	};

	struct SteamWire
	{
		std::deque<Bytes> ToHost;
		std::deque<Bytes> ToClient;
		bool DropByesToClient = false;
	};

	// SteamTransport's semantics between one host and one client: Disconnect is silent on the wire, a
	// disconnected peer is ignored for 1 s, then whatever it sends makes it a new connection.
	class FakeSteam final : public Transport
	{
	public:
		static constexpr PeerId HOST = 100;
		static constexpr PeerId CLIENT = 200;

		FakeSteam(SteamWire& wire, bool host, FakeClock& clock) : m_Wire(wire), m_Host(host), m_Clock(clock) {}

		void Send(PeerId, std::span<const uint8_t> data, Delivery) override
		{
			if (m_Host && m_Wire.DropByesToClient && !data.empty() && data[0] == 0xF5)
			{
				return;
			}
			(m_Host ? m_Wire.ToClient : m_Wire.ToHost).emplace_back(data.begin(), data.end());
		}

		void Poll(std::vector<Packet>& packets, std::vector<PeerEvent>& events) override
		{
			if (!m_Host && !m_Announced)
			{
				m_Announced = true;
				m_Peers.insert(HOST);
				events.push_back(Connect(HOST));
			}
			std::erase_if(m_Ignored, [this](const auto& entry) { return entry.second <= m_Clock.Now; });
			std::deque<Bytes>& in = m_Host ? m_Wire.ToHost : m_Wire.ToClient;
			const PeerId from = m_Host ? CLIENT : HOST;
			for (; !in.empty(); in.pop_front())
			{
				if (m_Ignored.contains(from))
				{
					continue;
				}
				if (m_Peers.insert(from).second)
				{
					events.push_back(Connect(from));
				}
				packets.push_back({ from, std::move(in.front()) });
			}
		}

		void Disconnect(PeerId peer) override
		{
			m_Peers.erase(peer);
			m_Ignored[peer] = m_Clock.Now + std::chrono::seconds(1);
		}

		void Close() override
		{
			m_Peers.clear();
		}

		std::string Describe() const override
		{
			return "fake steam";
		}

	private:
		SteamWire& m_Wire;
		bool m_Host;
		FakeClock& m_Clock;
		bool m_Announced = false;
		std::set<PeerId> m_Peers;
		std::map<PeerId, std::chrono::steady_clock::time_point> m_Ignored;
	};

	// A host session over a scripted transport with "Ann" seated in slot 1 as peer 7.
	std::shared_ptr<Script> HostWithAnn(Node& host)
	{
		auto script = std::make_shared<Script>();
		host.S.StartHost(std::make_unique<ScriptedTransport>(script), "Host");
		script->Events.push_back(Connect(7));
		script->Packets.push_back({ 7, Hello(k_SessionProtocol, PROTOCOL, CONTENT, "Ann") });
		host.Update();
		return script;
	}

}

TEST(SessionDefaultNameFilterKeepsPrintableAscii)
{
	CHECK(DefaultNameFilter("  Gr\xC3\xA4vekeeper the 3rd!  ") == "Grvekeeper the 3");
	CHECK(DefaultNameFilter(" Ann ") == "Ann");
	CHECK(DefaultNameFilter("\x01\x02 ") == "Player");
	CHECK(DefaultNameFilter("") == "Player");
}

TEST(SessionHandshakeSeatsEveryoneAndAnnouncesThem)
{
	FakeClock clock;
	Lan lan(MakeParams(&clock));
	Node& a = lan.Join(" An\x01n ");
	lan.Pump();
	Node& b = lan.Join("Bo");
	Node& c = lan.Join("Cy");
	lan.Pump();

	Node& host = *lan.Host;
	CHECK(host.S.IsHost() && host.S.Connected() && host.S.LocalSlot() == 0);
	CHECK(host.Count(Kind::Joined) == 3);
	CHECK(host.S.SeatedCount() == 4);
	CHECK(host.S.NameOf(0) == "Host");
	CHECK(host.S.NameOf(1) == "Ann");
	CHECK(a.S.LocalSlot() == 1);

	const std::set<int> slots = { a.S.LocalSlot(), b.S.LocalSlot(), c.S.LocalSlot() };
	CHECK(slots == std::set<int>({ 1, 2, 3 }));
	for (Node* client : { &a, &b, &c })
	{
		CHECK(client->S.Active() && client->S.Connected() && !client->S.IsHost());
		const Session::Event* welcomed = client->Find(Kind::Welcomed);
		CHECK(welcomed && welcomed->Slot == client->S.LocalSlot() && welcomed->Name == "Host");
		CHECK(client->Count(Kind::Joined) == 2);
		CHECK(client->S.SeatedCount() == 4);
		CHECK(client->S.MaxSlots() == 4);
		CHECK(client->S.NameOf(0) == "Host");
		CHECK(client->S.NameOf(a.S.LocalSlot()) == "Ann");
		CHECK(client->S.NameOf(b.S.LocalSlot()) == "Bo");
		CHECK(client->S.NameOf(c.S.LocalSlot()) == "Cy");
		const Session::Event* joined = host.Find(Kind::Joined, client->S.LocalSlot());
		CHECK(joined && joined->Name == client->S.NameOf(client->S.LocalSlot()));
	}
	CHECK(a.Find(Kind::Joined, b.S.LocalSlot()) && a.Find(Kind::Joined, c.S.LocalSlot()));
	CHECK(host.Count(Kind::Rejected) == 0 && host.Count(Kind::Left) == 0);

	auto existing = [](const Node& node)
	{
		return static_cast<int>(std::count_if(node.Events.begin(), node.Events.end(),
			[](const Session::Event& e) { return e.Type == Kind::Joined && e.Existing; }));
	};
	CHECK(existing(a) == 0);
	CHECK(existing(b) == 1 && b.Find(Kind::Joined, 1) && b.Find(Kind::Joined, 1)->Existing);
	CHECK(existing(c) == 2);
	CHECK(existing(host) == 0);
}

TEST(SessionAppliesTheGamesNameFilter)
{
	SessionParams params = MakeParams(nullptr);
	params.NameFilter = [](std::string_view name)
	{
		std::string out;
		for (const char ch : name)
		{
			out += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
		}
		return out;
	};
	Lan lan(params);
	Node& a = lan.Join("ann");
	lan.Pump();
	CHECK(lan.Host->S.NameOf(0) == "HOST");
	CHECK(lan.Host->S.NameOf(1) == "ANN");
	CHECK(a.S.NameOf(1) == "ANN");
	CHECK(a.Find(Kind::Welcomed) && a.Find(Kind::Welcomed)->Name == "HOST");
}

TEST(SessionTurnsAwayAClientPastMaxSlots)
{
	Lan lan(MakeParams(nullptr, 2));
	Node& a = lan.Join("Ann");
	lan.Pump();
	Node& b = lan.Join("Bo");
	lan.Pump();

	CHECK(a.S.LocalSlot() == 1);
	const Session::Event* rejected = b.Find(Kind::Rejected);
	CHECK(rejected && rejected->Rejection == RejectReason::Full && rejected->Slot == -1);
	CHECK(!b.S.Active() && !b.S.Connected());
	const Session::Event* hostSide = lan.Host->Find(Kind::Rejected);
	CHECK(hostSide && hostSide->Rejection == RejectReason::Full && hostSide->Name == "Bo");
	CHECK(lan.Host->S.SeatedCount() == 2);
	CHECK(lan.Host->S.PeerCount() == 1);
	CHECK(a.Count(Kind::Joined) == 0);
}

TEST(SessionRejectsAProtocolMismatch)
{
	Lan lan(MakeParams(nullptr));
	SessionParams old = MakeParams(nullptr);
	old.Protocol = PROTOCOL - 1;
	Node& a = lan.Join("Ann", old);
	lan.Pump();

	const Session::Event* rejected = a.Find(Kind::Rejected);
	CHECK(rejected && rejected->Rejection == RejectReason::ProtocolMismatch);
	CHECK(rejected && rejected->Detail.find("protocol 20") != std::string::npos);
	CHECK(!a.S.Active());
	const Session::Event* hostSide = lan.Host->Find(Kind::Rejected);
	CHECK(hostSide && hostSide->Rejection == RejectReason::ProtocolMismatch && hostSide->Detail.find("protocol 19") != std::string::npos);
	CHECK(lan.Host->S.SeatedCount() == 1 && lan.Host->S.PeerCount() == 0);
}

TEST(SessionRejectsAContentMismatch)
{
	Lan lan(MakeParams(nullptr));
	SessionParams other = MakeParams(nullptr);
	other.ContentHash = CONTENT + 1;
	Node& a = lan.Join("Ann", other);
	lan.Pump();

	const Session::Event* rejected = a.Find(Kind::Rejected);
	CHECK(rejected && rejected->Rejection == RejectReason::ContentMismatch);
	CHECK(!a.S.Active());
	CHECK(lan.Host->Find(Kind::Rejected) && lan.Host->Find(Kind::Rejected)->Rejection == RejectReason::ContentMismatch);
}

TEST(SessionRejectsJoinsWhileNotAccepting)
{
	Lan lan(MakeParams(nullptr));
	lan.Host->S.SetAccepting(false);
	Node& a = lan.Join("Ann");
	lan.Pump();
	CHECK(a.Find(Kind::Rejected) && a.Find(Kind::Rejected)->Rejection == RejectReason::NotAccepting);
	CHECK(!a.S.Active());

	lan.Host->S.SetAccepting(true);
	Node& b = lan.Join("Bo");
	lan.Pump();
	CHECK(b.Find(Kind::Welcomed, 1) != nullptr);
	CHECK(lan.Host->S.SeatedCount() == 2);
}

TEST(SessionRejectsAnotherSessionProtocol)
{
	Lan lan(MakeParams(nullptr));
	std::unique_ptr<LoopbackTransport> raw = LoopbackTransport::Connect(*lan.Link);
	Endpoint e{ raw.get() };
	e.Poll();
	lan.Pump(1);
	CHECK(e.Has(PeerEvent::Kind::Connected));
	if (!e.Has(PeerEvent::Kind::Connected))
	{
		return;
	}
	raw->Send(e.Find(PeerEvent::Kind::Connected)->Peer, Hello(k_SessionProtocol + 1, 7, 0, "Zed"), Delivery::Reliable);
	e.Poll();
	lan.Pump(1);
	e.Poll();

	const Packet* reject = FindPacket(e, 0xF2);
	CHECK(reject != nullptr);
	if (reject)
	{
		ByteReader r(reject->Data);
		CHECK(r.U8() == 0xF2);
		CHECK(r.U8() == static_cast<uint8_t>(RejectReason::ProtocolMismatch));
		CHECK(r.U16() == k_SessionProtocol);
		CHECK(r.U16() == PROTOCOL);
		CHECK(r.U32() == CONTENT);
		CHECK(r.Str() == "test");
		CHECK(r.Ok() && r.Remaining() == 0);
	}
	CHECK(e.Has(PeerEvent::Kind::Disconnected));
	const Session::Event* hostSide = lan.Host->Find(Kind::Rejected);
	CHECK(hostSide && hostSide->Rejection == RejectReason::ProtocolMismatch);
	CHECK(lan.Host->S.PeerCount() == 0);
}

TEST(SessionClientSurvivesMalformedPacketsFromTheHost)
{
	LogCapture log;
	std::unique_ptr<LoopbackTransport> listen = LoopbackTransport::Listen();
	Endpoint h{ listen.get() };
	Node a(MakeParams(nullptr));
	a.S.StartClient(LoopbackTransport::Connect(*listen), "Ann");
	a.Update();
	h.Poll();
	CHECK(h.Has(PeerEvent::Kind::Connected));
	CHECK(!h.Packets.empty());
	if (!h.Has(PeerEvent::Kind::Connected) || h.Packets.empty())
	{
		return;
	}
	const PeerId toClient = h.Find(PeerEvent::Kind::Connected)->Peer;
	const Bytes golden = { 0xF0, k_SessionProtocol, 0, PROTOCOL, 0, 0x44, 0x33, 0x22, 0x11, 3, 'A', 'n', 'n', 4, 't', 'e', 's', 't' };
	CHECK(h.Packets[0].Data == golden);

	Bytes trailing = Welcome(1, 4);
	trailing.push_back(0);
	SendAll(*listen, toClient, {
		{ 0xF1 }, Welcome(0, 4), Welcome(4, 4), Welcome(1, 33), Welcome(1, 1), Welcome(1, 4, { { 1, "Me" } }),
		Welcome(1, 4, { { 2, "Bo" }, { 2, "Bo" } }), Welcome(1, 4, { { 0, "Host" } }), trailing, { 0xF6, 2, 0 }, { 0xF7, 2, 0 },
		{ 0xF2 }, { 0xF2, 9, 1, 0, 20, 0, 0, 0, 0, 0, 0 }, { 0xF3 }, { 0xF4, 1 }, { 0xFA }, { 0xF5 }, { 0xF5, 9 }, { 0xF5, 3 },
		{ 0x10, 1 }, {} });
	a.Update();
	CHECK(a.Events.empty() && a.Messages.empty());
	CHECK(a.S.Active() && !a.S.Connected());

	SendAll(*listen, toClient, { Welcome(2, 3, { { 1, "Cy" } }) });
	a.Update();
	CHECK(a.Find(Kind::Welcomed, 2) && a.Find(Kind::Welcomed, 2)->Name == "Host");
	CHECK(a.Find(Kind::Joined, 1) && a.Find(Kind::Joined, 1)->Name == "Cy");
	CHECK(a.S.MaxSlots() == 3 && a.S.SeatedCount() == 3 && a.S.LocalSlot() == 2);
	a.Clear();

	SendAll(*listen, toClient, {
		{ 0xF6, 7, 0 }, { 0xF6, 2, 1, 'X' }, { 0xF6, 0, 0 }, { 0xF6, 1, 0 }, { 0xF6, 200, 0 }, { 0xF7, 0, 0 }, { 0xF7, 1, 9 },
		{ 0xF7, 2, 0 }, { 0xF7, 255, 0 }, { 0xF2, 0, 1, 0, 20, 0, 0, 0, 0, 0, 0 }, Welcome(1, 4), { 0xF0, 1, 0 } });
	a.Update();
	CHECK(a.Events.empty());
	CHECK(a.S.Active() && a.S.SeatedCount() == 3);

	SendAll(*listen, toClient, { { 0xF7, 1, static_cast<uint8_t>(LeaveReason::Kicked) }, { 0x10, 7 }, {} });
	a.Update();
	const Session::Event* left = a.Find(Kind::Left, 1);
	CHECK(left && left->Name == "Cy" && left->Leave == LeaveReason::Kicked);
	CHECK(a.Messages.size() == 2);
	CHECK(!a.Messages.empty() && a.Messages[0].Slot == 0 && a.Messages[0].Data == Bytes({ 0x10, 7 }));

	SendAll(*listen, toClient, { { 0xF5, static_cast<uint8_t>(LeaveReason::Kicked) } });
	a.Update();
	const Session::Event* lost = a.Find(Kind::Lost);
	CHECK(lost && lost->Leave == LeaveReason::Kicked && lost->Slot == 2);
	CHECK(!a.S.Active());
	CHECK(log.Errors() == 0);
}

TEST(SessionHostSurvivesMalformedPacketsFromAClient)
{
	LogCapture log;
	FakeClock clock;
	Lan lan(MakeParams(&clock));
	Node& host = *lan.Host;
	std::unique_ptr<LoopbackTransport> raw = LoopbackTransport::Connect(*lan.Link);
	Endpoint e{ raw.get() };
	e.Poll();
	lan.Pump(1);
	CHECK(e.Has(PeerEvent::Kind::Connected) && host.S.PeerCount() == 1);
	if (!e.Has(PeerEvent::Kind::Connected))
	{
		return;
	}
	const PeerId toHost = e.Find(PeerEvent::Kind::Connected)->Peer;

	Bytes trailing = Hello(k_SessionProtocol, PROTOCOL, CONTENT, "Zed");
	trailing.push_back(0xAA);
	SendAll(*raw, toHost, {
		{ 0xF0 }, { 0xF0, k_SessionProtocol }, { 0xF0, k_SessionProtocol, 0, PROTOCOL }, trailing, { 0xF3 }, { 0xF4, 1 },
		Welcome(1, 4), { 0xF2, 0, 1, 0, 20, 0, 0, 0, 0, 0, 0 }, { 0xF6, 1, 0 }, { 0xF7, 1, 0 }, { 0xFF }, { 0xF5 }, { 0xF5, 9 },
		{ 0x05, 1, 2 }, {} });
	lan.Pump(1);
	CHECK(host.Events.empty() && host.Messages.empty());
	CHECK(host.S.Active() && host.S.PeerCount() == 1 && host.S.SeatedCount() == 1);

	SendAll(*raw, toHost, { Hello(k_SessionProtocol, PROTOCOL, CONTENT, "Zed") });
	lan.Pump(1);
	e.Poll();
	CHECK(host.Find(Kind::Joined, 1) && host.Find(Kind::Joined, 1)->Name == "Zed");
	const Packet* welcome = FindPacket(e, 0xF1);
	CHECK(welcome && welcome->Data == Bytes({ 0xF1, 1, 4, 4, 'H', 'o', 's', 't', 0 }));
	host.Clear();

	SendAll(*raw, toHost, {
		{ 0xF4, 0xFF, 0xFF, 0xFF, 0xFF }, { 0xF5, 9 },
		{ 0xF5, static_cast<uint8_t>(LeaveReason::TransportLost) }, { 0x05, 9 } });
	lan.Pump(1);
	CHECK(host.Events.empty());
	CHECK(host.S.SeatedCount() == 2);
	CHECK(host.Messages.size() == 1 && host.Messages[0].Slot == 1 && host.Messages[0].Data == Bytes({ 0x05, 9 }));

	SendAll(*raw, toHost, { { 0xF5, static_cast<uint8_t>(LeaveReason::Bye) } });
	lan.Pump(1);
	const Session::Event* left = host.Find(Kind::Left, 1);
	CHECK(left && left->Leave == LeaveReason::Bye && left->Name == "Zed");
	CHECK(host.S.PeerCount() == 0 && host.S.SeatedCount() == 1);
	CHECK(log.Errors() == 0);
}

TEST(SessionDropsGameSendsInTheSessionRange)
{
	Lan lan(MakeParams(nullptr));
	Node& a = lan.Join("Ann");
	lan.Pump();
	{
		LogCapture log;
		lan.Host->S.Send(1, Bytes({ 0xF0, 1 }), Delivery::Reliable);
		lan.Host->S.Broadcast(Bytes({ 0xFF }), Delivery::Reliable);
		a.S.Send(0, Bytes({ 0xF5, 0 }), Delivery::Reliable);
		a.S.Broadcast(Bytes({ 0xF3, 0, 0, 0, 0 }), Delivery::Unreliable);
		CHECK(log.Errors() == 4);
	}
	lan.Pump();
	CHECK(a.Messages.empty() && lan.Host->Messages.empty());
	CHECK(a.S.Connected() && lan.Host->Count(Kind::Left) == 0);

	a.S.Send(0, Bytes({ 0xEF, 1 }), Delivery::Reliable);
	lan.Pump();
	CHECK(lan.Host->Messages.size() == 1 && lan.Host->Messages[0].Data == Bytes({ 0xEF, 1 }));
}

TEST(SessionGameMessagesCarryTheSendersSlot)
{
	Lan lan(MakeParams(nullptr));
	Node& a = lan.Join("Ann");
	lan.Pump();
	Node& b = lan.Join("Bo");
	lan.Pump();
	Session& host = lan.Host->S;
	CHECK(a.S.LocalSlot() == 1 && b.S.LocalSlot() == 2);

	host.Broadcast(Bytes({ 0x10, 1 }), Delivery::Reliable);
	host.Broadcast(Bytes({ 0x11 }), Delivery::Unreliable, 1);
	host.Send(2, Bytes({ 0x12 }), Delivery::Unreliable);
	host.Send(0, Bytes({ 0x13 }), Delivery::Reliable);
	a.S.Send(0, Bytes({ 0x20, 5 }), Delivery::Reliable);
	a.S.Send(2, Bytes({ 0x22 }), Delivery::Reliable);
	b.S.Broadcast(Bytes({ 0x21 }), Delivery::Unreliable);
	lan.Pump();

	CHECK(a.Messages.size() == 1 && a.Messages[0].Slot == 0 && a.Messages[0].Data == Bytes({ 0x10, 1 }));
	CHECK(b.Messages.size() == 3);
	for (const Session::Message& m : b.Messages)
	{
		CHECK(m.Slot == 0);
	}
	const std::vector<Session::Message>& in = lan.Host->Messages;
	CHECK(in.size() == 2);
	for (const Session::Message& m : in)
	{
		CHECK((m.Slot == 1 && m.Data == Bytes({ 0x20, 5 })) || (m.Slot == 2 && m.Data == Bytes({ 0x21 })));
	}
}

TEST(SessionHostTimesOutASilentClient)
{
	FakeClock clock;
	Lan lan(MakeParams(&clock));
	lan.Join("Ann");
	lan.Pump();
	Node& host = *lan.Host;
	CHECK(host.S.SeatedCount() == 2);

	double leftAt = -1.0;
	for (int step = 1; step <= 24 && leftAt < 0.0; step++)
	{
		clock.Advance(std::chrono::milliseconds(250));
		host.Update();
		if (host.Find(Kind::Left))
		{
			leftAt = step * 0.25;
		}
	}
	const Session::Event* left = host.Find(Kind::Left, 1);
	CHECK(left && left->Leave == LeaveReason::TimedOut && left->Name == "Ann");
	CHECK(leftAt > 5.0 && leftAt <= 5.25);
	CHECK(host.S.SeatedCount() == 1 && host.S.PeerCount() == 0);
}

TEST(SessionClientTimesOutASilentHost)
{
	FakeClock clock;
	Lan lan(MakeParams(&clock));
	Node& a = lan.Join("Ann");
	lan.Pump();
	CHECK(a.S.Connected());

	double lostAt = -1.0;
	for (int step = 1; step <= 24 && lostAt < 0.0; step++)
	{
		clock.Advance(std::chrono::milliseconds(250));
		a.Update();
		if (a.Find(Kind::Lost))
		{
			lostAt = step * 0.25;
		}
	}
	const Session::Event* lost = a.Find(Kind::Lost);
	CHECK(lost && lost->Leave == LeaveReason::TimedOut && lost->Slot == 1 && lost->Name == "Host");
	CHECK(lostAt > 5.0 && lostAt <= 5.25);
	CHECK(!a.S.Active());
}

TEST(SessionHandshakeTimesOutOnBothSides)
{
	LogCapture log;
	FakeClock clock;
	{
		std::unique_ptr<LoopbackTransport> mute = LoopbackTransport::Listen();
		Node a(MakeParams(&clock));
		a.S.StartClient(LoopbackTransport::Connect(*mute), "Ann");
		clock.Advance(std::chrono::milliseconds(4000));
		a.Update();
		clock.Advance(std::chrono::milliseconds(1500));
		a.Update();
		CHECK(a.S.Active() && a.Events.empty());
		clock.Advance(std::chrono::milliseconds(3400));
		a.Update();
		CHECK(a.S.Active() && a.Events.empty());
		clock.Advance(std::chrono::milliseconds(200));
		a.Update();
		const Session::Event* lost = a.Find(Kind::Lost);
		CHECK(lost && lost->Leave == LeaveReason::TimedOut && lost->Slot == -1);
		CHECK(!a.S.Active());
	}
	{
		Node a(MakeParams(&clock));
		a.S.StartClient(std::make_unique<ScriptedTransport>(std::make_shared<Script>()), "Ann");
		clock.Advance(std::chrono::milliseconds(4900));
		a.Update();
		CHECK(a.S.Active() && a.Events.empty());
		clock.Advance(std::chrono::milliseconds(200));
		a.Update();
		const Session::Event* lost = a.Find(Kind::Lost);
		CHECK(lost && lost->Leave == LeaveReason::TimedOut && lost->Slot == -1);
	}
	{
		Lan lan(MakeParams(&clock));
		std::unique_ptr<LoopbackTransport> silent = LoopbackTransport::Connect(*lan.Link);
		lan.Pump(1);
		CHECK(lan.Host->S.PeerCount() == 1);
		clock.Advance(std::chrono::milliseconds(4900));
		lan.Pump(1);
		CHECK(lan.Host->S.PeerCount() == 1);
		clock.Advance(std::chrono::milliseconds(200));
		lan.Pump(1);
		CHECK(lan.Host->S.PeerCount() == 0);
		CHECK(lan.Host->Events.empty());
	}
}

TEST(SessionByeLeavesBeforeTheTransportCloses)
{
	Lan lan(MakeParams(nullptr));
	Node& a = lan.Join("Ann");
	lan.Pump();
	Node& b = lan.Join("Bo");
	lan.Pump();

	a.S.Stop();
	CHECK(!a.S.Active() && a.Events.size() == 2);
	lan.Pump();
	const Session::Event* left = lan.Host->Find(Kind::Left, 1);
	CHECK(left && left->Leave == LeaveReason::Bye && left->Name == "Ann");
	const Session::Event* seen = b.Find(Kind::Left, 1);
	CHECK(seen && seen->Leave == LeaveReason::Bye && seen->Name == "Ann");
	CHECK(b.S.SeatedCount() == 2 && !b.S.SlotTaken(1));

	lan.Host->S.Stop();
	lan.Pump();
	const Session::Event* lost = b.Find(Kind::Lost);
	CHECK(lost && lost->Leave == LeaveReason::Bye && lost->Slot == 2 && lost->Name == "Host");
	CHECK(!b.S.Active());
}

TEST(SessionKickFreesTheSeat)
{
	Lan lan(MakeParams(nullptr));
	Node& a = lan.Join("Ann");
	lan.Pump();
	Node& b = lan.Join("Bo");
	lan.Pump();
	Node& host = *lan.Host;

	{
		LogCapture log;
		host.S.Kick(0);
		host.S.Kick(3);
		host.S.Kick(40);
		a.S.Kick(2);
		CHECK(log.Errors() == 0);
	}
	CHECK(host.S.SeatedCount() == 3);

	host.S.Kick(1);
	CHECK(host.S.SeatedCount() == 2 && host.S.PeerCount() == 1);
	const std::vector<Session::Event> kicked = host.S.TakeEvents();
	CHECK(kicked.size() == 1);
	CHECK(!kicked.empty() && kicked[0].Type == Kind::Left && kicked[0].Slot == 1 && kicked[0].Leave == LeaveReason::Kicked && kicked[0].Name == "Ann");
	lan.Pump();
	const Session::Event* lost = a.Find(Kind::Lost);
	CHECK(lost && lost->Leave == LeaveReason::Kicked && lost->Slot == 1);
	CHECK(!a.S.Active());
	const Session::Event* seen = b.Find(Kind::Left, 1);
	CHECK(seen && seen->Leave == LeaveReason::Kicked);

	Node& c = lan.Join("Cy");
	lan.Pump();
	CHECK(c.S.LocalSlot() == 1);
	CHECK(b.Find(Kind::Joined, 1) && b.S.NameOf(1) == "Cy");
}

TEST(SessionChurnReusesSlots)
{
	Lan lan(MakeParams(nullptr, 3));
	Node& a = lan.Join("Ann");
	lan.Pump();
	Node& host = *lan.Host;
	host.Clear();
	a.Clear();

	int welcomedInSlot2 = 0;
	int leftWithTheRightReason = 0;
	for (int cycle = 0; cycle < 100; cycle++)
	{
		Node churner(lan.Params);
		churner.S.StartClient(LoopbackTransport::Connect(*lan.Link), "C" + std::to_string(cycle));
		for (int round = 0; round < 3; round++)
		{
			host.Update();
			a.Update();
			churner.Update();
		}
		welcomedInSlot2 += churner.Find(Kind::Welcomed, 2) ? 1 : 0;

		const bool kick = cycle % 2 == 1;
		if (kick)
		{
			host.S.Kick(2);
		}
		else
		{
			churner.S.Stop();
		}
		for (int round = 0; round < 2; round++)
		{
			host.Update();
			a.Update();
			churner.Update();
		}
		const Session::Event* left = host.Find(Kind::Left, 2);
		const LeaveReason expected = kick ? LeaveReason::Kicked : LeaveReason::Bye;
		leftWithTheRightReason += left && left->Leave == expected && left->Name == "C" + std::to_string(cycle) ? 1 : 0;
		CHECK(!churner.S.Active());
		CHECK(host.S.PeerCount() == 1 && host.S.SeatedCount() == 2);
		CHECK(a.S.SeatedCount() == 2 && !a.S.SlotTaken(2));
		host.Clear();
	}
	CHECK(welcomedInSlot2 == 100);
	CHECK(leftWithTheRightReason == 100);
	CHECK(a.Count(Kind::Joined) == 100 && a.Count(Kind::Left) == 100);
	CHECK(a.S.Connected() && a.S.LocalSlot() == 1);
}

TEST(SessionRttBecomesPositiveAfterPings)
{
	FakeClock clock;
	Lan lan(MakeParams(&clock));
	Node& a = lan.Join("Ann");
	lan.Pump();
	CHECK(lan.Host->S.RttMs(1) == 0.0f);

	for (int i = 0; i < 3; i++)
	{
		clock.Advance(std::chrono::milliseconds(1000));
		lan.Pump(1);
		clock.Advance(std::chrono::milliseconds(40));
		lan.Pump(1);
	}
	CHECK(lan.Host->S.RttMs(1) > 30.0f && lan.Host->S.RttMs(1) < 50.0f);
	CHECK(a.S.RttMs(0) > 0.0f && a.S.RttMs(0) < 50.0f);
	CHECK(a.S.Connected() && lan.Host->Count(Kind::Left) == 0);
	CHECK(lan.Host->S.Describe().find("1:") != std::string::npos);
}

TEST(SessionHandshakesAndTalksOverTcp)
{
	std::string error;
	std::unique_ptr<TcpTransport> listen = TcpTransport::Listen("127.0.0.1", 0, error);
	CHECK(listen != nullptr);
	if (!listen)
	{
		return;
	}
	const uint16_t port = listen->GetPort();
	Node host(MakeParams(nullptr));
	host.S.StartHost(std::move(listen), "Host");
	Node a(MakeParams(nullptr));
	a.S.StartClient(TcpTransport::Connect("127.0.0.1", port, error), "Ann");
	CHECK(a.S.Active());

	auto pumpUntil = [&](auto done)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
		while (!done() && std::chrono::steady_clock::now() < deadline)
		{
			host.Update();
			a.Update();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return done();
	};

	CHECK(pumpUntil([&] { return a.Find(Kind::Welcomed) && host.Find(Kind::Joined); }));
	CHECK(a.S.LocalSlot() == 1);

	a.S.Send(0, Bytes({ 0x42, 1, 2, 3 }), Delivery::Reliable);
	host.S.Send(1, Bytes({ 0x43 }), Delivery::Unreliable);
	CHECK(pumpUntil([&] { return !host.Messages.empty() && !a.Messages.empty(); }));
	CHECK(!host.Messages.empty() && host.Messages[0].Slot == 1 && host.Messages[0].Data == Bytes({ 0x42, 1, 2, 3 }));
	CHECK(!a.Messages.empty() && a.Messages[0].Slot == 0 && a.Messages[0].Data == Bytes({ 0x43 }));

	a.S.Stop();
	CHECK(pumpUntil([&] { return host.Find(Kind::Left) != nullptr; }));
	CHECK(host.Find(Kind::Left, 1) && host.Find(Kind::Left, 1)->Leave == LeaveReason::Bye);
}

TEST(SessionClientTurnsAwayAHostWithMoreSeats)
{
	LogCapture log;
	std::unique_ptr<LoopbackTransport> listen = LoopbackTransport::Listen();
	Endpoint h{ listen.get() };
	Node a(MakeParams(nullptr));
	a.S.StartClient(LoopbackTransport::Connect(*listen), "Ann");
	a.Update();
	h.Poll();
	CHECK(h.Has(PeerEvent::Kind::Connected));
	if (!h.Has(PeerEvent::Kind::Connected))
	{
		return;
	}
	const PeerId toClient = h.Find(PeerEvent::Kind::Connected)->Peer;
	h.Packets.clear();

	SendAll(*listen, toClient, { Welcome(7, 8) });
	a.Update();
	const Session::Event* rejected = a.Find(Kind::Rejected);
	CHECK(rejected && rejected->Rejection == RejectReason::ProtocolMismatch && rejected->Slot == -1);
	CHECK(rejected && rejected->Detail.find("8 seats") != std::string::npos);
	CHECK(a.Count(Kind::Welcomed) == 0 && a.Count(Kind::Joined) == 0);
	CHECK(!a.S.Active());
	h.Poll();
	CHECK(FindPacket(h, 0xF5) && FindPacket(h, 0xF5)->Data == Bytes({ 0xF5, static_cast<uint8_t>(LeaveReason::Bye) }));

	Node b(MakeParams(nullptr));
	b.S.StartClient(LoopbackTransport::Connect(*listen), "Bo");
	b.Update();
	h.Events.clear();
	h.Poll();
	CHECK(h.Has(PeerEvent::Kind::Connected));
	if (!h.Has(PeerEvent::Kind::Connected))
	{
		return;
	}
	const PeerId toB = h.Find(PeerEvent::Kind::Connected)->Peer;
	SendAll(*listen, toB, { Welcome(1, 4), { 0xF6, 20, 1, 'Z' }, { 0xF6, 4, 1, 'Z' }, { 0xF7, 20, 0 } });
	b.Update();
	CHECK(b.Find(Kind::Welcomed, 1) != nullptr);
	CHECK(b.Count(Kind::Joined) == 0 && b.Count(Kind::Left) == 0);
	CHECK(b.S.Active() && b.S.MaxSlots() == 4 && b.S.SeatedCount() == 2);
	CHECK(log.Errors() == 0);
}

TEST(SessionTreatsAReusedIdAsAFreshPeer)
{
	Node host(MakeParams(nullptr));
	std::shared_ptr<Script> script = HostWithAnn(host);
	CHECK(host.Find(Kind::Joined, 1) && host.Find(Kind::Joined, 1)->Name == "Ann");
	host.Clear();

	script->Events = { Disconnect(7, DisconnectReason::Reset, "reset by peer"), Connect(7) };
	script->Packets.push_back({ 7, Hello(k_SessionProtocol, PROTOCOL, CONTENT, "Ann2") });
	script->Packets.push_back({ 99, { 0x10 } });
	script->Packets.push_back({ 99, { 0x11 } });
	host.Update();
	CHECK(host.Events.size() == 2);
	const Session::Event* left = host.Find(Kind::Left, 1);
	CHECK(left && left->Leave == LeaveReason::TransportLost && left->Transport == DisconnectReason::Reset && left->Detail == "reset by peer");
	const Session::Event* joined = host.Find(Kind::Joined, 1);
	CHECK(joined && joined->Name == "Ann2");
	CHECK(!host.Events.empty() && host.Events[0].Type == Kind::Left);
	CHECK(host.S.PeerCount() == 1 && host.S.SeatedCount() == 2);
	CHECK(script->DisconnectsOf(99) == 1 && script->DisconnectsOf(7) == 0);
	host.Clear();

	script->Packets.push_back({ 7, { 0x10, 1 } });
	script->Packets.push_back({ 7, { 0xF5, static_cast<uint8_t>(LeaveReason::Bye) } });
	script->Events.push_back(Disconnect(7, DisconnectReason::Closed, "closed"));
	host.Update();
	const Session::Event* bye = host.Find(Kind::Left, 1);
	CHECK(bye && bye->Leave == LeaveReason::Bye);
	CHECK(host.Events.size() == 1 && host.Messages.empty());
	CHECK(host.S.PeerCount() == 0);
}

TEST(SessionClientTheHostTimedOutLearnsItOverASilentTransport)
{
	for (const bool byeLost : { false, true })
	{
		FakeClock clock;
		SteamWire wire;
		wire.DropByesToClient = byeLost;
		Node host(MakeParams(&clock));
		Node a(MakeParams(&clock));
		host.S.StartHost(std::make_unique<FakeSteam>(wire, true, clock), "Host");
		a.S.StartClient(std::make_unique<FakeSteam>(wire, false, clock), "Ann");
		auto step = [&](bool runClient)
		{
			clock.Advance(std::chrono::milliseconds(100));
			host.Update();
			if (runClient)
			{
				a.Update();
			}
		};
		for (int i = 0; i < 10; i++)
		{
			step(true);
		}
		CHECK(a.S.Connected() && host.S.SeatedCount() == 2);

		for (int i = 0; i < 70; i++)
		{
			step(false);
		}
		const Session::Event* left = host.Find(Kind::Left, 1);
		CHECK(left && left->Leave == LeaveReason::TimedOut);

		int steps = 0;
		for (; steps < 100 && a.S.Active(); steps++)
		{
			step(true);
		}
		const Session::Event* lost = a.Find(Kind::Lost);
		CHECK(lost && lost->Leave == LeaveReason::TimedOut && lost->Slot == 1);
		CHECK(steps <= (byeLost ? 56 : 1));
		CHECK(host.S.SeatedCount() == 1);
	}
}

TEST(SessionReseatsAClientThatRejoinsUnderTheSameId)
{
	FakeClock clock;
	SteamWire wire;
	Node host(MakeParams(&clock));
	Node a(MakeParams(&clock));
	host.S.StartHost(std::make_unique<FakeSteam>(wire, true, clock), "Host");
	a.S.StartClient(std::make_unique<FakeSteam>(wire, false, clock), "Ann");
	auto step = [&](bool runHost)
	{
		clock.Advance(std::chrono::milliseconds(100));
		if (runHost)
		{
			host.Update();
		}
		a.Update();
	};
	for (int i = 0; i < 10; i++)
	{
		step(true);
	}
	CHECK(a.S.Connected() && host.S.SeatedCount() == 2);
	host.Clear();

	for (int i = 0; i < 60 && a.S.Active(); i++)
	{
		step(false);
	}
	CHECK(a.Find(Kind::Lost) && a.Find(Kind::Lost)->Leave == LeaveReason::TimedOut);
	a.Clear();
	a.S.StartClient(std::make_unique<FakeSteam>(wire, false, clock), "Ann");
	step(false);
	step(true);
	step(true);

	CHECK(a.Find(Kind::Welcomed, 1) && a.S.Connected());
	CHECK(host.Events.size() == 2);
	CHECK(host.Events.size() == 2 && host.Events[0].Type == Kind::Left && host.Events[0].Slot == 1
		&& host.Events[0].Leave == LeaveReason::TransportLost && host.Events[0].Transport == DisconnectReason::Reset);
	CHECK(host.Events.size() == 2 && host.Events[1].Type == Kind::Joined && host.Events[1].Slot == 1 && host.Events[1].Name == "Ann");
	CHECK(host.S.SeatedCount() == 2 && host.S.PeerCount() == 1);

	for (int i = 0; i < 100; i++)
	{
		step(true);
	}
	CHECK(a.S.Connected() && a.Count(Kind::Lost) == 0 && host.Count(Kind::Left) == 1);
}

TEST(SessionFreesTheSeatOfAPeerWhoseNewHelloIsTurnedAway)
{
	Node host(MakeParams(nullptr));
	std::shared_ptr<Script> script = HostWithAnn(host);
	host.Clear();

	script->Packets.push_back({ 7, Hello(k_SessionProtocol, PROTOCOL + 1, CONTENT, "Ann") });
	host.Update();
	CHECK(host.Events.size() == 2);
	CHECK(host.Events.size() == 2 && host.Events[0].Type == Kind::Left && host.Events[0].Slot == 1 && host.Events[1].Type == Kind::Rejected
		&& host.Events[1].Rejection == RejectReason::ProtocolMismatch);
	CHECK(host.S.SeatedCount() == 1 && host.S.PeerCount() == 0);
	CHECK(script->DisconnectsOf(7) == 1);
}

TEST(SessionNeverHandsALeaversMessagesToTheNextOccupant)
{
	Node host(MakeParams(nullptr));
	std::shared_ptr<Script> script = HostWithAnn(host);
	host.Clear();

	script->Events.push_back(Connect(8));
	script->Packets.push_back({ 7, { 0x10, 'o' } });
	script->Packets.push_back({ 7, { 0xF5, static_cast<uint8_t>(LeaveReason::Bye) } });
	script->Packets.push_back({ 8, Hello(k_SessionProtocol, PROTOCOL, CONTENT, "Bo") });
	script->Packets.push_back({ 8, { 0x10, 'n' } });
	host.Update();
	CHECK(host.Events.size() == 2);
	CHECK(host.Events.size() == 2 && host.Events[0].Type == Kind::Left && host.Events[0].Name == "Ann" && host.Events[1].Type == Kind::Joined
		&& host.Events[1].Slot == 1 && host.Events[1].Name == "Bo");
	CHECK(host.Messages.size() == 1 && host.Messages[0].Slot == 1 && host.Messages[0].Data == Bytes({ 0x10, 'n' }));
	CHECK(host.Events.size() == 2 && host.Events[0].LastMessages == std::vector<Bytes>({ { 0x10, 'o' } }) && host.Events[1].LastMessages.empty());

	script->Packets.push_back({ 8, { 0x10, 'x' } });
	host.S.Update();
	host.S.Kick(1);
	CHECK(host.S.TakeMessages().empty());
	const std::vector<Session::Event> events = host.S.TakeEvents();
	CHECK(events.size() == 1 && events[0].Type == Kind::Left && events[0].Leave == LeaveReason::Kicked);
	CHECK(events.size() == 1 && events[0].LastMessages == std::vector<Bytes>({ { 0x10, 'x' } }));
}

TEST(SessionRateLimitsWarningsAndHangsUpOnABadPeer)
{
	LogCapture log;
	Node host(MakeParams(nullptr));
	std::shared_ptr<Script> script = HostWithAnn(host);
	host.Clear();

	script->Events.push_back(Connect(8));
	for (int i = 0; i < 40; i++)
	{
		script->Packets.push_back({ 8, { 0xF0 } });
	}
	for (int i = 0; i < 20; i++)
	{
		script->Packets.push_back({ 7, { 0xF3 } });
		script->Packets.push_back({ 7, { 0xFC } });
	}
	host.Update();
	CHECK(log.Warnings() == 5);
	CHECK(script->DisconnectsOf(8) >= 1 && host.S.PeerCount() == 0);
	const Session::Event* left = host.Find(Kind::Left, 1);
	CHECK(left && left->Leave == LeaveReason::Kicked);
	CHECK(script->SentTo(7, Bytes({ 0xF5, static_cast<uint8_t>(LeaveReason::Kicked) })));
	CHECK(script->SentTo(8, Bytes({ 0xF5, static_cast<uint8_t>(LeaveReason::Kicked) })));
	CHECK(host.Events.size() == 1);
	CHECK(log.Errors() == 0);
}

TEST(SessionClampsThePingIntervalToHalfTheTimeout)
{
	LogCapture log;
	SessionParams params = MakeParams(nullptr);
	params.Timeout = 2.0;
	params.PingInterval = 5.0;
	Session session(params);
	CHECK(session.GetParams().PingInterval == 1.0);
	CHECK(log.Warnings() == 1);
}

TEST(SessionReportsTheTransportsReasonOnLost)
{
	auto script = std::make_shared<Script>();
	Node a(MakeParams(nullptr));
	a.S.StartClient(std::make_unique<ScriptedTransport>(script), "Ann");
	script->Events.push_back(Connect(1));
	a.Update();
	CHECK(!script->Sent.empty() && script->Sent[0].first == 1 && script->Sent[0].second[0] == 0xF0);
	script->Packets.push_back({ 1, Welcome(1, 4) });
	a.Update();
	CHECK(a.S.Connected());
	script->Events.push_back(Disconnect(1, DisconnectReason::TimedOut, "ETIMEDOUT"));
	a.Update();
	const Session::Event* lost = a.Find(Kind::Lost);
	CHECK(lost && lost->Leave == LeaveReason::TransportLost && lost->Transport == DisconnectReason::TimedOut && lost->Detail == "ETIMEDOUT"
		&& lost->Slot == 1 && lost->Name == "Host");
}

TEST(SessionLetsTheGameAnswerAnOlderWire)
{
	LogCapture log;
	SessionParams params = MakeParams(nullptr);
	params.UnseatedPacket = [](std::span<const uint8_t> data) -> std::optional<Bytes>
	{
		if (!data.empty() && data[0] == 0x40)
		{
			return Bytes({ 0x42, 'o', 'l', 'd' });
		}
		return std::nullopt;
	};
	Lan lan(params);
	std::unique_ptr<LoopbackTransport> raw = LoopbackTransport::Connect(*lan.Link);
	Endpoint e{ raw.get() };
	e.Poll();
	lan.Pump(1);
	CHECK(e.Has(PeerEvent::Kind::Connected));
	if (!e.Has(PeerEvent::Kind::Connected))
	{
		return;
	}
	const PeerId toHost = e.Find(PeerEvent::Kind::Connected)->Peer;

	SendAll(*raw, toHost, { { 0x41, 1 } });
	lan.Pump(1);
	CHECK(lan.Host->S.PeerCount() == 1);

	SendAll(*raw, toHost, { { 0x40, 19, 0 } });
	lan.Pump(1);
	e.Poll();
	CHECK(lan.Host->S.PeerCount() == 0 && lan.Host->Events.empty());
	CHECK(e.Packets.size() == 1 && e.Packets[0].Data == Bytes({ 0x42, 'o', 'l', 'd' }));
	CHECK(e.Has(PeerEvent::Kind::Disconnected));
}

TEST(SessionHandsTheHostAPartingMessageOnTheLeft)
{
	Lan lan(MakeParams(nullptr));
	Node& a = lan.Join("Ann");
	lan.Pump();
	Node& b = lan.Join("Bo");
	lan.Pump();
	lan.Host->Clear();

	a.S.Send(0, Bytes({ 0x30, 1 }), Delivery::Reliable);
	a.S.Send(0, Bytes({ 0x30, 2 }), Delivery::Reliable);
	a.S.Stop();
	b.S.Send(0, Bytes({ 0x31 }), Delivery::Reliable);
	lan.Pump();

	const Session::Event* left = lan.Host->Find(Kind::Left, 1);
	CHECK(left && left->Leave == LeaveReason::Bye && left->LastMessages == std::vector<Bytes>({ { 0x30, 1 }, { 0x30, 2 } }));
	CHECK(lan.Host->Messages.size() == 1 && lan.Host->Messages[0].Slot == 2 && lan.Host->Messages[0].Data == Bytes({ 0x31 }));
	const Session::Event* seen = b.Find(Kind::Left, 1);
	CHECK(seen && seen->LastMessages.empty());
}

TEST(SessionOverTcpSeatsAFullSessionAndTurnsTheNextAwayAsFull)
{
	std::string error;
	std::unique_ptr<TcpTransport> listen = TcpTransport::Listen("127.0.0.1", 0, error);
	CHECK(listen != nullptr);
	if (!listen)
	{
		return;
	}
	const uint16_t port = listen->GetPort();
	Node host(MakeParams(nullptr, MAX_SESSION_SLOTS));
	host.S.StartHost(std::move(listen), "Host");

	std::vector<std::unique_ptr<Node>> clients;
	for (int i = 0; i < MAX_SESSION_SLOTS; i++)
	{
		clients.push_back(std::make_unique<Node>(MakeParams(nullptr, MAX_SESSION_SLOTS)));
		Node& c = *clients.back();
		c.S.StartClient(TcpTransport::Connect("127.0.0.1", port, error), "C" + std::to_string(i));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
		while (c.Events.empty() && std::chrono::steady_clock::now() < deadline)
		{
			host.Update();
			c.Update();
			std::this_thread::yield();
		}
	}

	CHECK(host.S.SeatedCount() == MAX_SESSION_SLOTS);
	int welcomed = 0;
	for (int i = 0; i + 1 < MAX_SESSION_SLOTS; i++)
	{
		welcomed += clients[static_cast<size_t>(i)]->Find(Kind::Welcomed) ? 1 : 0;
	}
	CHECK(welcomed == MAX_SESSION_SLOTS - 1);
	const Session::Event* full = clients.back()->Find(Kind::Rejected);
	CHECK(full && full->Rejection == RejectReason::Full);
	CHECK(clients.back()->Count(Kind::Lost) == 0);
}

TEST(SessionIdleConnectionsCannotLockJoinersOut)
{
	LogCapture log;
	std::string error;
	std::unique_ptr<TcpTransport> listen = TcpTransport::Listen("127.0.0.1", 0, error);
	CHECK(listen != nullptr);
	if (!listen)
	{
		return;
	}
	const uint16_t port = listen->GetPort();
	Node host(MakeParams(nullptr));
	host.S.StartHost(std::move(listen), "Host");

	std::vector<std::unique_ptr<TcpTransport>> idle;
	for (int i = 0; i < 70; i++)
	{
		idle.push_back(TcpTransport::Connect("127.0.0.1", port, error));
		Endpoint e{ idle.back().get() };
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
		while (!e.Has(PeerEvent::Kind::Connected) && !e.Has(PeerEvent::Kind::Disconnected) && std::chrono::steady_clock::now() < deadline)
		{
			std::this_thread::yield();
			e.Poll();
		}
		host.Update();
	}
	CHECK(host.S.PeerCount() <= 16);

	Node a(MakeParams(nullptr));
	a.S.StartClient(TcpTransport::Connect("127.0.0.1", port, error), "Ann");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
	while (a.Events.empty() && std::chrono::steady_clock::now() < deadline)
	{
		host.Update();
		a.Update();
		std::this_thread::yield();
	}
	CHECK(a.Find(Kind::Welcomed, 1) && a.S.Connected());
	CHECK(host.S.SeatedCount() == 2 && host.S.PeerCount() <= 17);
}

TEST(SessionSaysByeOnlyToTheSeatedPeersItTimesOut)
{
	FakeClock clock;
	SessionParams params = MakeParams(&clock);
	params.Timeout = 10.0;
	params.HandshakeTimeout = 5.0;
	Node host(params);
	std::shared_ptr<Script> script = HostWithAnn(host);
	script->Events.push_back(Connect(8));
	host.Update();
	CHECK(host.S.PeerCount() == 2);

	const Bytes timedOut = { 0xF5, static_cast<uint8_t>(LeaveReason::TimedOut) };
	clock.Advance(std::chrono::milliseconds(5100));
	host.Update();
	CHECK(script->DisconnectsOf(8) == 1 && host.S.PeerCount() == 1);
	CHECK(!script->SentTo(8, timedOut));

	clock.Advance(std::chrono::milliseconds(5000));
	host.Update();
	CHECK(host.Find(Kind::Left, 1) && host.Find(Kind::Left, 1)->Leave == LeaveReason::TimedOut);
	CHECK(script->SentTo(7, timedOut) && script->DisconnectsOf(7) == 1);
}

TEST(SessionLetsAJoinerInThroughAConnectionFlood)
{
	LogCapture log;
	Node host(MakeParams(nullptr));
	std::shared_ptr<Script> script = HostWithAnn(host);
	host.Clear();
	script->Events.push_back(Connect(100));
	script->Packets.push_back({ 100, { 0x10 } });
	host.Update();

	script->Events.push_back(Connect(101));
	for (PeerId flood = 102; flood <= 121; flood++)
	{
		script->Events.push_back(Connect(flood));
	}
	script->Packets.push_back({ 101, Hello(k_SessionProtocol, PROTOCOL, CONTENT, "Bo") });
	host.Update();

	CHECK(host.Find(Kind::Joined, 2) && host.Find(Kind::Joined, 2)->Name == "Bo");
	CHECK(script->DisconnectsOf(101) == 0 && script->DisconnectsOf(100) == 0);
	bool oldestSilentWent = true;
	for (PeerId flood = 102; flood <= 121; flood++)
	{
		oldestSilentWent = oldestSilentWent && script->DisconnectsOf(flood) == (flood <= 106 ? 1 : 0);
	}
	CHECK(oldestSilentWent);
	CHECK(host.S.PeerCount() == 2 + 16);
}
