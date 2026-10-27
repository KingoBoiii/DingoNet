#include "TestFramework.h"
#include "TransportTestUtils.h"

#include "Internal/UdpTestAccess.h"

#include <DingoNet/Conditioner.h>
#include <DingoNet/Session.h>
#include <DingoNet/UdpTransport.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Dingo::Net;
using TransportTest::Endpoint;
using TransportTest::LogCapture;
using TransportTest::Pattern;
using TransportTest::PumpUntil;
using TransportTest::Text;

namespace
{

	using Kind = PeerEvent::Kind;
	using SessionKind = Session::Event::Kind;
	using Bytes = std::vector<uint8_t>;

	struct UdpPair
	{
		std::unique_ptr<UdpTransport> Host;
		std::unique_ptr<UdpTransport> Client;
		Endpoint H;
		Endpoint C;
		PeerId ToClient = 0;
		PeerId ToHost = 0;
	};

	bool OpenPair(UdpPair& pair, const UdpParams& params = {})
	{
		std::string error;
		pair.Host = UdpTransport::Listen("127.0.0.1", 0, error, params);
		if (!pair.Host || pair.Host->GetPort() == 0)
		{
			return false;
		}
		pair.Client = UdpTransport::Connect("127.0.0.1", pair.Host->GetPort(), error, params);
		if (!pair.Client)
		{
			return false;
		}
		pair.H.Link = pair.Host.get();
		pair.C.Link = pair.Client.get();
		if (!PumpUntil({ &pair.H, &pair.C }, [&] { return pair.H.Has(Kind::Connected) && pair.C.Has(Kind::Connected); }))
		{
			return false;
		}
		pair.ToClient = pair.H.Find(Kind::Connected)->Peer;
		pair.ToHost = pair.C.Find(Kind::Connected)->Peer;
		pair.H.Events.clear();
		pair.C.Events.clear();
		return true;
	}

	Bytes Numbered(uint32_t number)
	{
		Bytes data(4);
		std::memcpy(data.data(), &number, 4);
		return data;
	}

	uint32_t NumberOf(const Bytes& data)
	{
		uint32_t number = 0;
		if (data.size() >= 4)
		{
			std::memcpy(&number, data.data(), 4);
		}
		return number;
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

		const Session::Event* Find(SessionKind kind, int slot = -2) const
		{
			for (const Session::Event& e : Events)
			{
				if (e.Type == kind && (slot == -2 || e.Slot == slot))
				{
					return &e;
				}
			}
			return nullptr;
		}

		int Count(SessionKind kind) const
		{
			int count = 0;
			for (const Session::Event& e : Events)
			{
				count += e.Type == kind ? 1 : 0;
			}
			return count;
		}
	};

	SessionParams SessionSetup(int maxSlots = 4)
	{
		SessionParams params;
		params.MaxSlots = maxSlots;
		params.Protocol = 20;
		params.ContentHash = 0x11223344;
		params.Build = "test";
		return params;
	}

	template<typename Done>
	bool Pump(std::vector<Node*> nodes, Done done, std::chrono::milliseconds limit = std::chrono::milliseconds(5000))
	{
		const auto deadline = std::chrono::steady_clock::now() + limit;
		while (!done() && std::chrono::steady_clock::now() < deadline)
		{
			for (Node* node : nodes)
			{
				node->Update();
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return done();
	}

}

TEST(UdpConnectsAndSendsEveryDelivery)
{
	UdpPair pair;
	CHECK(OpenPair(pair));
	if (!pair.ToClient)
	{
		return;
	}
	CHECK(pair.Host->Describe().find("udp") != std::string::npos);

	pair.Client->Send(pair.ToHost, Text("reliable"), Delivery::Reliable);
	pair.Client->Send(pair.ToHost, Text("loose"), Delivery::Unreliable);
	pair.Client->Send(pair.ToHost, Text("sequenced"), Delivery::UnreliableSequenced);
	pair.Host->Send(pair.ToClient, Text("back"), Delivery::Reliable);
	CHECK(PumpUntil({ &pair.H, &pair.C }, [&] { return pair.H.Packets.size() == 3 && pair.C.Packets.size() == 1; }));
	CHECK(pair.H.Packets.size() == 3 && pair.C.Packets.size() == 1);
	CHECK(pair.C.Packets[0].From == pair.ToHost && pair.C.Packets[0].Data == Text("back"));
	int seen = 0;
	for (const Packet& p : pair.H.Packets)
	{
		CHECK(p.From == pair.ToClient);
		seen += (p.Data == Text("reliable") || p.Data == Text("loose") || p.Data == Text("sequenced")) ? 1 : 0;
	}
	CHECK(seen == 3);
	CHECK(pair.Client->BytesOut == 8 + 5 + 9 && pair.Host->BytesIn == 8 + 5 + 9);
}

TEST(UdpReliableKeepsOrderAndLargePacketsArriveWhole)
{
	UdpPair pair;
	CHECK(OpenPair(pair));
	if (!pair.ToClient)
	{
		return;
	}
	for (uint32_t i = 0; i < 300; i++)
	{
		pair.Client->Send(pair.ToHost, Numbered(i), Delivery::Reliable);
	}
	const Bytes big = Pattern(16 * 1024, 7);
	pair.Client->Send(pair.ToHost, big, Delivery::Reliable);
	const Bytes huge = Pattern(64 * 1024, 9);
	pair.Client->Send(pair.ToHost, huge, Delivery::Reliable);
	CHECK(PumpUntil({ &pair.H, &pair.C }, [&] { return pair.H.Packets.size() == 302; }));
	if (pair.H.Packets.size() != 302)
	{
		return;
	}
	for (uint32_t i = 0; i < 300; i++)
	{
		CHECK(NumberOf(pair.H.Packets[i].Data) == i);
	}
	CHECK(pair.H.Packets[300].Data == big);
	CHECK(pair.H.Packets[301].Data == huge);
}

TEST(UdpEnforcesTheSenderSizeCaps)
{
	LogCapture log;
	UdpPair pair;
	CHECK(OpenPair(pair));
	if (!pair.ToClient)
	{
		return;
	}
	pair.Client->Send(pair.ToHost, Pattern(64 * 1024 + 1, 1), Delivery::Reliable);
	CHECK(log.Errors() == 1);
	pair.Client->Send(pair.ToHost, Pattern(MAX_UNRELIABLE_BYTES + 1, 1), Delivery::Unreliable);
	pair.Client->Send(pair.ToHost, Pattern(MAX_UNRELIABLE_BYTES + 1, 1), Delivery::UnreliableSequenced);
	CHECK(log.Errors() == 3);
	pair.Client->Send(pair.ToHost, Pattern(MAX_UNRELIABLE_BYTES, 1), Delivery::Unreliable);
	CHECK(log.Errors() == 3);
	CHECK(PumpUntil({ &pair.H, &pair.C }, [&] { return !pair.H.Packets.empty(); }));
	CHECK(pair.H.Packets.size() == 1 && pair.H.Packets[0].Data.size() == MAX_UNRELIABLE_BYTES);
	pair.Client->Send(9999, Text("nobody"), Delivery::Reliable);
	CHECK(log.Errors() == 3);
}

TEST(UdpDropsAPeerThatSendsMoreThanTheReceiversLimit)
{
	LogCapture log;
	UdpParams strict;
	strict.MaxReliableBytes = 2000;
	std::string error;
	auto host = UdpTransport::Listen("127.0.0.1", 0, error, strict);
	CHECK(host != nullptr);
	if (!host)
	{
		return;
	}
	auto client = UdpTransport::Connect("127.0.0.1", host->GetPort(), error);
	Endpoint h{ host.get() };
	Endpoint c{ client.get() };
	CHECK(PumpUntil({ &h, &c }, [&] { return h.Has(Kind::Connected) && c.Has(Kind::Connected); }));
	if (!c.Has(Kind::Connected))
	{
		return;
	}
	client->Send(c.Find(Kind::Connected)->Peer, Pattern(10000, 3), Delivery::Reliable);
	CHECK(PumpUntil({ &h, &c }, [&] { return h.Has(Kind::Disconnected) && c.Has(Kind::Disconnected); }));
	const PeerEvent* dropped = h.Find(Kind::Disconnected);
	CHECK(dropped && dropped->Reason == DisconnectReason::Error);
	CHECK(h.Packets.empty());
	CHECK(log.Errors() == 1);
	const PeerEvent* seen = c.Find(Kind::Disconnected);
	CHECK(seen && seen->Reason == DisconnectReason::Closed);
}

TEST(UdpHangupOnADeadPeerFreesItsSlot)
{
	UdpParams params;
	params.MaxConnections = 2;
	std::string error;
	auto host = UdpTransport::Listen("127.0.0.1", 0, error, params);
	CHECK(host != nullptr);
	if (!host)
	{
		return;
	}
	auto first = UdpTransport::Connect("127.0.0.1", host->GetPort(), error, params);
	auto second = UdpTransport::Connect("127.0.0.1", host->GetPort(), error, params);
	Endpoint h{ host.get() };
	Endpoint c0{ first.get() };
	Endpoint c1{ second.get() };
	const auto accepted = [&]
	{
		size_t count = 0;
		for (const PeerEvent& e : h.Events)
		{
			count += e.Type == Kind::Connected ? 1 : 0;
		}
		return count;
	};
	CHECK(PumpUntil({ &h, &c0, &c1 }, [&] { return accepted() == 2; }));
	if (accepted() != 2)
	{
		return;
	}
	for (const PeerEvent& e : h.Events)
	{
		host->Disconnect(e.Peer);
	}

	UdpParams late;
	late.ConnectTimeout = 0.8;
	bool joined = false;
	for (int attempt = 0; attempt < 7 && !joined; attempt++)
	{
		auto client = UdpTransport::Connect("127.0.0.1", host->GetPort(), error, late);
		Endpoint c{ client.get() };
		joined = PumpUntil({ &h, &c }, [&] { return c.Has(Kind::Connected) || c.Has(Kind::Disconnected); }, std::chrono::milliseconds(1500)) && c.Has(Kind::Connected);
	}
	CHECK(joined);
}

TEST(UdpSequencedNeverGoesBackwards)
{
	UdpPair pair;
	CHECK(OpenPair(pair));
	if (!pair.ToClient)
	{
		return;
	}
	for (uint32_t i = 0; i < 200; i++)
	{
		pair.Client->Send(pair.ToHost, Numbered(i), Delivery::UnreliableSequenced);
		if (i % 8 == 7)
		{
			pair.H.Poll();
			pair.C.Poll();
		}
	}
	PumpUntil({ &pair.H, &pair.C }, [] { return false; }, std::chrono::milliseconds(200));
	CHECK(!pair.H.Packets.empty());
	uint32_t last = 0;
	bool first = true;
	for (const Packet& p : pair.H.Packets)
	{
		const uint32_t n = NumberOf(p.Data);
		CHECK(first || n > last);
		last = n;
		first = false;
	}
}

TEST(UdpDisconnectDeliversWhatWasQueuedAndRaisesNoLocalEvent)
{
	UdpPair pair;
	CHECK(OpenPair(pair));
	if (!pair.ToClient)
	{
		return;
	}
	pair.Host->Send(pair.ToClient, Text("parting"), Delivery::Reliable);
	pair.Host->Disconnect(pair.ToClient);
	pair.Host->Send(pair.ToClient, Text("too late"), Delivery::Reliable);
	CHECK(PumpUntil({ &pair.H, &pair.C }, [&] { return pair.C.Has(Kind::Disconnected); }));
	CHECK(pair.C.Packets.size() == 1 && pair.C.Packets[0].Data == Text("parting"));
	const PeerEvent* gone = pair.C.Find(Kind::Disconnected);
	CHECK(gone && gone->Reason == DisconnectReason::Closed);
	PumpUntil({ &pair.H, &pair.C }, [] { return false; }, std::chrono::milliseconds(100));
	CHECK(!pair.H.Has(Kind::Disconnected));
	pair.Client->Send(pair.ToHost, Text("to a gone peer"), Delivery::Reliable);
}

TEST(UdpCloseDeliversWhatWasQueuedToAPeerThatIsPolling)
{
	UdpPair pair;
	CHECK(OpenPair(pair));
	if (!pair.ToClient)
	{
		return;
	}
	pair.Host->Send(pair.ToClient, Text("bye"), Delivery::Reliable);
	std::atomic<bool> stop{ false };
	std::thread poller([&]
	{
		while (!stop)
		{
			pair.C.Poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	});
	pair.Host->Close();
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	stop = true;
	poller.join();
	CHECK(pair.C.Packets.size() == 1 && pair.C.Packets[0].Data == Text("bye"));
	const PeerEvent* gone = pair.C.Find(Kind::Disconnected);
	CHECK(gone && gone->Reason == DisconnectReason::Closed);
	pair.H.Poll();
	CHECK(pair.H.Events.empty());
	pair.Host->Send(pair.ToClient, Text("inert"), Delivery::Reliable);
	pair.Host->Close();
}

TEST(UdpAConnectToNobodyFails)
{
	std::string error;
	uint16_t port = 0;
	{
		auto probe = UdpTransport::Listen("127.0.0.1", 0, error);
		CHECK(probe != nullptr);
		if (!probe)
		{
			return;
		}
		port = probe->GetPort();
	}
	UdpParams params;
	params.ConnectTimeout = 0.5;
	auto client = UdpTransport::Connect("127.0.0.1", port, error, params);
	CHECK(client != nullptr);
	if (!client)
	{
		return;
	}
	Endpoint c{ client.get() };
	CHECK(PumpUntil({ &c }, [&] { return c.Has(Kind::Disconnected); }, std::chrono::milliseconds(10000)));
	const PeerEvent* failed = c.Find(Kind::Disconnected);
	CHECK(failed && failed->Reason == DisconnectReason::ConnectFailed);
	CHECK(!c.Has(Kind::Connected));
}

TEST(UdpRejectsABadAddressAndATakenPort)
{
	std::string error;
	CHECK(UdpTransport::Listen("not an address", 0, error) == nullptr);
	CHECK(!error.empty());
	CHECK(UdpTransport::Connect("256.1.1.1", 4000, error) == nullptr);

	auto first = UdpTransport::Listen("127.0.0.1", 0, error);
	CHECK(first != nullptr);
	if (!first)
	{
		return;
	}
	error.clear();
	CHECK(UdpTransport::Listen("127.0.0.1", first->GetPort(), error) == nullptr);
	CHECK(!error.empty());
}

TEST(UdpHostStopsAnsweringPastItsConnectionCap)
{
	UdpParams params;
	params.MaxConnections = 2;
	params.ConnectTimeout = 0.6;
	std::string error;
	auto host = UdpTransport::Listen("127.0.0.1", 0, error, params);
	CHECK(host != nullptr);
	if (!host)
	{
		return;
	}
	Endpoint h{ host.get() };
	auto first = UdpTransport::Connect("127.0.0.1", host->GetPort(), error, params);
	auto second = UdpTransport::Connect("127.0.0.1", host->GetPort(), error, params);
	CHECK(first != nullptr && second != nullptr);
	if (!first || !second)
	{
		return;
	}
	Endpoint c0{ first.get() };
	Endpoint c1{ second.get() };
	CHECK(PumpUntil({ &h, &c0, &c1 }, [&] { return c0.Has(Kind::Connected) && c1.Has(Kind::Connected); }));

	auto third = UdpTransport::Connect("127.0.0.1", host->GetPort(), error, params);
	CHECK(third != nullptr);
	if (!third)
	{
		return;
	}
	Endpoint c2{ third.get() };
	CHECK(PumpUntil({ &h, &c0, &c1, &c2 }, [&] { return c2.Has(Kind::Disconnected); }, std::chrono::milliseconds(10000)));
	CHECK(!c2.Has(Kind::Connected));
	const PeerEvent* failed = c2.Find(Kind::Disconnected);
	CHECK(failed && failed->Reason == DisconnectReason::ConnectFailed);
}

TEST(UdpASilentPeerTimesOut)
{
	UdpParams params;
	params.PeerTimeout = 0.6;
	UdpPair pair;
	CHECK(OpenPair(pair, params));
	if (!pair.ToClient)
	{
		return;
	}
	Internal::UdpTestAccess::DropIncomingDatagrams(*pair.Host, 1.0f, 1);
	CHECK(PumpUntil({ &pair.H, &pair.C }, [&] { return pair.H.Has(Kind::Disconnected); }, std::chrono::milliseconds(6000)));
	const PeerEvent* gone = pair.H.Find(Kind::Disconnected);
	CHECK(gone && gone->Reason == DisconnectReason::TimedOut && gone->Peer == pair.ToClient);
}

TEST(UdpReliableSurvivesTenPercentDatagramLoss)
{
	UdpPair pair;
	CHECK(OpenPair(pair));
	if (!pair.ToClient)
	{
		return;
	}
	Internal::UdpTestAccess::DropIncomingDatagrams(*pair.Host, 0.1f, 11);
	Internal::UdpTestAccess::DropIncomingDatagrams(*pair.Client, 0.1f, 12);
	for (uint32_t i = 0; i < 1000; i++)
	{
		pair.Client->Send(pair.ToHost, Numbered(i), Delivery::Reliable);
		pair.Host->Send(pair.ToClient, Numbered(i), Delivery::Reliable);
	}
	CHECK(PumpUntil({ &pair.H, &pair.C }, [&] { return pair.H.Packets.size() == 1000 && pair.C.Packets.size() == 1000; }, std::chrono::milliseconds(20000)));
	if (pair.H.Packets.size() != 1000 || pair.C.Packets.size() != 1000)
	{
		return;
	}
	for (uint32_t i = 0; i < 1000; i++)
	{
		CHECK(NumberOf(pair.H.Packets[i].Data) == i);
		CHECK(NumberOf(pair.C.Packets[i].Data) == i);
	}
	CHECK(!pair.H.Has(Kind::Disconnected) && !pair.C.Has(Kind::Disconnected));
}

TEST(UdpUnderLagJitterAndLossKeepsASequencedStreamForward)
{
	std::string error;
	auto host = UdpTransport::Listen("127.0.0.1", 0, error);
	CHECK(host != nullptr);
	if (!host)
	{
		return;
	}
	ConditionerParams weather;
	weather.LagSeconds = 0.1;
	weather.JitterSeconds = 0.03;
	weather.Loss = 0.1f;
	auto client = std::make_unique<Conditioner>(UdpTransport::Connect("127.0.0.1", host->GetPort(), error), weather);
	Endpoint h{ host.get() };
	Endpoint c{ client.get() };
	CHECK(PumpUntil({ &h, &c }, [&] { return h.Has(Kind::Connected) && c.Has(Kind::Connected); }));
	if (!c.Has(Kind::Connected))
	{
		return;
	}
	const PeerId toHost = c.Find(Kind::Connected)->Peer;

	for (uint32_t i = 0; i < 120; i++)
	{
		client->Send(toHost, Numbered(i), Delivery::UnreliableSequenced);
		PumpUntil({ &h, &c }, [] { return false; }, std::chrono::milliseconds(4));
	}
	PumpUntil({ &h, &c }, [] { return false; }, std::chrono::milliseconds(400));

	CHECK(h.Packets.size() > 10 && h.Packets.size() < 120);
	uint32_t last = 0;
	bool first = true;
	for (const Packet& p : h.Packets)
	{
		const uint32_t n = NumberOf(p.Data);
		CHECK(first || n > last);
		last = n;
		first = false;
	}
	CHECK(client->Dropped > 0);
}

TEST(UdpSessionHandshakesAndTalks)
{
	std::string error;
	auto listen = UdpTransport::Listen("127.0.0.1", 0, error);
	CHECK(listen != nullptr);
	if (!listen)
	{
		return;
	}
	const uint16_t port = listen->GetPort();
	Node host(SessionSetup());
	host.S.StartHost(std::move(listen), "Host");
	Node a(SessionSetup());
	a.S.StartClient(UdpTransport::Connect("127.0.0.1", port, error), "Ann");

	CHECK(Pump({ &host, &a }, [&] { return a.Find(SessionKind::Welcomed) && host.Find(SessionKind::Joined); }));
	CHECK(a.S.LocalSlot() == 1);

	a.S.Send(0, Bytes({ 0x42, 1, 2, 3 }), Delivery::Reliable);
	host.S.Send(1, Bytes({ 0x43 }), Delivery::UnreliableSequenced);
	CHECK(Pump({ &host, &a }, [&] { return !host.Messages.empty() && !a.Messages.empty(); }));
	CHECK(!host.Messages.empty() && host.Messages[0].Slot == 1 && host.Messages[0].Data == Bytes({ 0x42, 1, 2, 3 }));
	CHECK(!a.Messages.empty() && a.Messages[0].Slot == 0 && a.Messages[0].Data == Bytes({ 0x43 }));

	CHECK(Pump({ &host, &a }, [&] { return host.S.RttMs(1) > 0.0f || a.S.RttMs(0) > 0.0f; }, std::chrono::milliseconds(2500)));

	std::atomic<bool> stop{ false };
	std::thread hostPump([&]
	{
		while (!stop)
		{
			host.Update();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	});
	a.S.Stop();
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	stop = true;
	hostPump.join();
	CHECK(Pump({ &host }, [&] { return host.Find(SessionKind::Left) != nullptr; }));
	CHECK(host.Find(SessionKind::Left, 1) && host.Find(SessionKind::Left, 1)->Leave == LeaveReason::Bye);
}

TEST(UdpSessionFillsAllThirtyTwoSeats)
{
	std::string error;
	auto listen = UdpTransport::Listen("127.0.0.1", 0, error);
	CHECK(listen != nullptr);
	if (!listen)
	{
		return;
	}
	const uint16_t port = listen->GetPort();
	Node host(SessionSetup(32));
	host.S.StartHost(std::move(listen), "Host");
	std::vector<std::unique_ptr<Node>> clients;
	std::vector<Node*> all{ &host };
	for (int i = 0; i < 31; i++)
	{
		clients.push_back(std::make_unique<Node>(SessionSetup(32)));
		clients.back()->S.StartClient(UdpTransport::Connect("127.0.0.1", port, error), "P" + std::to_string(i));
		all.push_back(clients.back().get());
		CHECK(Pump(all, [&] { return host.Count(SessionKind::Joined) == i + 1; }));
	}
	CHECK(host.Count(SessionKind::Joined) == 31);
	CHECK(Pump(all, [&]
	{
		for (auto& client : clients)
		{
			if (client->Count(SessionKind::Welcomed) != 1 || client->Count(SessionKind::Joined) != 30)
			{
				return false;
			}
		}
		return true;
	}, std::chrono::milliseconds(8000)));

	host.S.Broadcast(Bytes({ 0x50, 1 }), Delivery::Reliable);
	CHECK(Pump(all, [&]
	{
		for (auto& client : clients)
		{
			if (client->Messages.size() != 1)
			{
				return false;
			}
		}
		return true;
	}));
}

TEST(UdpSessionRunsAtTenPercentDatagramLoss)
{
	std::string error;
	auto listen = UdpTransport::Listen("127.0.0.1", 0, error);
	CHECK(listen != nullptr);
	if (!listen)
	{
		return;
	}
	const uint16_t port = listen->GetPort();
	UdpTransport* hostLink = listen.get();
	Node host(SessionSetup());
	host.S.StartHost(std::move(listen), "Host");

	auto link = UdpTransport::Connect("127.0.0.1", port, error);
	CHECK(link != nullptr);
	if (!link)
	{
		return;
	}
	UdpTransport* clientLink = link.get();
	Node a(SessionSetup());
	a.S.StartClient(std::move(link), "Ann");
	CHECK(Pump({ &host, &a }, [&] { return a.Find(SessionKind::Welcomed) && host.Find(SessionKind::Joined); }));
	Internal::UdpTestAccess::DropIncomingDatagrams(*hostLink, 0.1f, 21);
	Internal::UdpTestAccess::DropIncomingDatagrams(*clientLink, 0.1f, 22);

	for (uint32_t i = 0; i < 200; i++)
	{
		Bytes up = Numbered(i);
		up.insert(up.begin(), 0x42);
		a.S.Send(0, up, Delivery::Reliable);
		Bytes down = Numbered(i);
		down.insert(down.begin(), 0x43);
		host.S.Send(1, down, Delivery::Reliable);
	}
	CHECK(Pump({ &host, &a }, [&] { return host.Messages.size() == 200 && a.Messages.size() == 200; }, std::chrono::milliseconds(15000)));
	for (size_t i = 0; i < host.Messages.size() && i < a.Messages.size(); i++)
	{
		CHECK(host.Messages[i].Data.size() == 5 && host.Messages[i].Data[0] == 0x42 && NumberOf(Bytes(host.Messages[i].Data.begin() + 1, host.Messages[i].Data.end())) == i);
		CHECK(a.Messages[i].Data.size() == 5 && a.Messages[i].Data[0] == 0x43 && NumberOf(Bytes(a.Messages[i].Data.begin() + 1, a.Messages[i].Data.end())) == i);
	}
	CHECK(host.Count(SessionKind::Left) == 0 && a.Count(SessionKind::Lost) == 0);
}

TEST(UdpSessionTimeoutFiresBeforeTheTransportsOwn)
{
	bool sessionWon = false;
	for (int attempt = 0; attempt < 3 && !sessionWon; attempt++)
	{
		std::string error;
		auto listen = UdpTransport::Listen("127.0.0.1", 0, error);
		CHECK(listen != nullptr);
		if (!listen)
		{
			return;
		}
		const uint16_t port = listen->GetPort();
		SessionParams params = SessionSetup();
		params.Timeout = 1.0;
		params.PingInterval = 0.25;
		Node host(params);
		host.S.StartHost(std::move(listen), "Host");

		auto link = UdpTransport::Connect("127.0.0.1", port, error);
		CHECK(link != nullptr);
		if (!link)
		{
			return;
		}
		UdpTransport* clientLink = link.get();
		Node a(params);
		a.S.StartClient(std::move(link), "Ann");
		CHECK(Pump({ &host, &a }, [&] { return a.Find(SessionKind::Welcomed) && host.Find(SessionKind::Joined); }));

		Internal::UdpTestAccess::DropIncomingDatagrams(*clientLink, 1.0f, 5);
		const auto started = std::chrono::steady_clock::now();
		CHECK(Pump({ &host, &a }, [&] { return a.Find(SessionKind::Lost) != nullptr; }, std::chrono::milliseconds(4000)));
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
		const Session::Event* lost = a.Find(SessionKind::Lost);
		sessionWon = lost && lost->Leave == LeaveReason::TimedOut && !lost->FromBye && seconds < UdpParams{}.PeerTimeout;
	}
	CHECK(sessionWon);
}

TEST(UdpCarriesOnThroughFailedReceives)
{
	LogCapture log;
	UdpPair pair;
	CHECK(OpenPair(pair));
	if (!pair.ToClient)
	{
		return;
	}
	Internal::UdpTestAccess::FailIncomingDatagrams(*pair.Host, 3);
	for (uint32_t i = 0; i < 50; i++)
	{
		pair.Client->Send(pair.ToHost, Numbered(i), Delivery::Reliable);
		pair.C.Poll();
		pair.H.Poll();
	}
	CHECK(PumpUntil({ &pair.H, &pair.C }, [&] { return pair.H.Packets.size() == 50; }));
	for (size_t i = 0; i < pair.H.Packets.size(); i++)
	{
		CHECK(NumberOf(pair.H.Packets[i].Data) == i);
	}
	CHECK(Internal::UdpTestAccess::IsOpen(*pair.Host));
	CHECK(!pair.H.Has(Kind::Disconnected) && !pair.C.Has(Kind::Disconnected));
	CHECK(log.Warnings() == 1 && log.Errors() == 0);
}

TEST(UdpClosesOnlyWhenTheSocketFailsForASecond)
{
	LogCapture log;
	UdpPair pair;
	CHECK(OpenPair(pair));
	if (!pair.ToClient)
	{
		return;
	}
	Internal::UdpTestAccess::BreakSocket(*pair.Host);
	const auto broken = std::chrono::steady_clock::now();
	pair.H.Poll();
	CHECK(Internal::UdpTestAccess::IsOpen(*pair.Host) && pair.H.Events.empty());
	CHECK(PumpUntil({ &pair.H }, [&] { return pair.H.Has(Kind::Disconnected); }));
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - broken).count();
	CHECK(seconds >= 0.99);
	const PeerEvent* failed = pair.H.Find(Kind::Disconnected);
	CHECK(failed && failed->Reason == DisconnectReason::Error && failed->Peer == pair.ToClient);
	CHECK(!Internal::UdpTestAccess::IsOpen(*pair.Host));
	CHECK(log.Errors() == 1 && log.Warnings() == 1);
	pair.Host->Send(pair.ToClient, Text("inert"), Delivery::Reliable);
}

TEST(UdpResetsHalfOpenConnectsPastSixteen)
{
	UdpParams params;
	params.MaxConnections = 20;
	std::string error;
	auto host = UdpTransport::Listen("127.0.0.1", 0, error, params);
	CHECK(host != nullptr);
	if (!host)
	{
		return;
	}
	Endpoint h{ host.get() };
	std::vector<std::unique_ptr<UdpTransport>> halfOpen;
	for (int i = 0; i < 40; i++)
	{
		halfOpen.push_back(UdpTransport::Connect("127.0.0.1", host->GetPort(), error));
		if (halfOpen.back())
		{
			Endpoint once{ halfOpen.back().get() };
			once.Poll();
		}
	}
	PumpUntil({ &h }, [] { return false; }, std::chrono::milliseconds(50));
	CHECK(Internal::UdpTestAccess::PendingPeers(*host) == 16);

	auto client = UdpTransport::Connect("127.0.0.1", host->GetPort(), error);
	Endpoint c{ client.get() };
	CHECK(PumpUntil({ &h, &c }, [&] { return (c.Has(Kind::Connected) && h.Has(Kind::Connected)) || c.Has(Kind::Disconnected); }));
	CHECK(c.Has(Kind::Connected) && h.Has(Kind::Connected) && !c.Has(Kind::Disconnected));
	CHECK(Internal::UdpTestAccess::PendingPeers(*host) <= 16);
}

TEST(UdpResetsAHalfOpenConnectAfterConnectTimeout)
{
	UdpParams params;
	params.MaxConnections = 2;
	params.ConnectTimeout = 0.5;
	std::string error;
	auto host = UdpTransport::Listen("127.0.0.1", 0, error, params);
	CHECK(host != nullptr);
	if (!host)
	{
		return;
	}
	Endpoint h{ host.get() };
	std::vector<std::unique_ptr<UdpTransport>> halfOpen;
	for (int i = 0; i < 2; i++)
	{
		halfOpen.push_back(UdpTransport::Connect("127.0.0.1", host->GetPort(), error));
		if (halfOpen.back())
		{
			Endpoint once{ halfOpen.back().get() };
			once.Poll();
		}
	}
	CHECK(PumpUntil({ &h }, [&] { return Internal::UdpTestAccess::PendingPeers(*host) == 2; }, std::chrono::milliseconds(1000)));
	const auto full = std::chrono::steady_clock::now();
	CHECK(PumpUntil({ &h }, [&] { return Internal::UdpTestAccess::PendingPeers(*host) == 0; }, std::chrono::milliseconds(2000)));
	CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - full).count() >= 0.4);

	auto client = UdpTransport::Connect("127.0.0.1", host->GetPort(), error);
	Endpoint c{ client.get() };
	CHECK(PumpUntil({ &h, &c }, [&] { return c.Has(Kind::Connected) || c.Has(Kind::Disconnected); }));
	CHECK(c.Has(Kind::Connected));
}

TEST(UdpCapsTheConnectionsFromOneAddress)
{
	UdpParams params;
	params.MaxConnections = 8;
	params.MaxConnectionsPerAddress = 2;
	std::string error;
	auto host = UdpTransport::Listen("127.0.0.1", 0, error, params);
	CHECK(host != nullptr);
	if (!host)
	{
		return;
	}
	auto first = UdpTransport::Connect("127.0.0.1", host->GetPort(), error);
	auto second = UdpTransport::Connect("127.0.0.1", host->GetPort(), error);
	Endpoint h{ host.get() };
	Endpoint c0{ first.get() };
	Endpoint c1{ second.get() };
	CHECK(PumpUntil({ &h, &c0, &c1 }, [&] { return c0.Has(Kind::Connected) && c1.Has(Kind::Connected); }));
	if (!c0.Has(Kind::Connected) || !c1.Has(Kind::Connected))
	{
		return;
	}

	UdpParams quick;
	quick.ConnectTimeout = 0.25;
	auto third = UdpTransport::Connect("127.0.0.1", host->GetPort(), error, quick);
	Endpoint c2{ third.get() };
	CHECK(PumpUntil({ &h, &c0, &c1, &c2 }, [&] { return c2.Has(Kind::Disconnected); }));
	const PeerEvent* failed = c2.Find(Kind::Disconnected);
	CHECK(failed && failed->Reason == DisconnectReason::ConnectFailed);
	CHECK(!c2.Has(Kind::Connected));

	first->Disconnect(c0.Find(Kind::Connected)->Peer);
	second->Disconnect(c1.Find(Kind::Connected)->Peer);
	PumpUntil({ &h, &c0, &c1 }, [&] { return std::count_if(h.Events.begin(), h.Events.end(), [](const PeerEvent& e) { return e.Type == Kind::Disconnected; }) == 2; });
}

TEST(UdpDropsAPeerWhoseQueuePassesTheCap)
{
	LogCapture log;
	UdpParams params;
	params.MaxQueuedBytes = 256 * 1024;
	UdpPair pair;
	CHECK(OpenPair(pair, params));
	if (!pair.ToClient)
	{
		return;
	}
	for (int i = 0; i < 8; i++)
	{
		pair.Host->Send(pair.ToClient, Pattern(64 * 1024, static_cast<uint8_t>(i)), Delivery::Reliable);
	}
	CHECK(Internal::UdpTestAccess::QueuedBytes(*pair.Host, pair.ToClient) > 256 * 1024);
	CHECK(Internal::UdpTestAccess::QueuedBytes(*pair.Host, pair.ToClient) <= 256 * 1024 + 64 * 1024);
	pair.H.Poll();
	const PeerEvent* dropped = pair.H.Find(Kind::Disconnected);
	CHECK(dropped && dropped->Reason == DisconnectReason::Error && dropped->Peer == pair.ToClient);
	CHECK(log.Errors() == 1);
	CHECK(Internal::UdpTestAccess::QueuedBytes(*pair.Host, pair.ToClient) == 0);
	CHECK(PumpUntil({ &pair.H, &pair.C }, [&] { return pair.C.Has(Kind::Disconnected); }));
}

TEST(UdpADrainingPeerStaysUnderTheQueueCap)
{
	UdpParams params;
	params.MaxQueuedBytes = 256 * 1024;
	UdpPair pair;
	CHECK(OpenPair(pair, params));
	if (!pair.ToClient)
	{
		return;
	}
	for (uint32_t i = 0; i < 300; i++)
	{
		Bytes data = Pattern(4096, static_cast<uint8_t>(i));
		std::memcpy(data.data(), &i, 4);
		pair.Host->Send(pair.ToClient, data, Delivery::Reliable);
		pair.H.Poll();
		pair.C.Poll();
	}
	CHECK(PumpUntil({ &pair.H, &pair.C }, [&]
	{
		return pair.C.Packets.size() == 300 && Internal::UdpTestAccess::QueuedBytes(*pair.Host, pair.ToClient) == 0;
	}));
	CHECK(!pair.H.Has(Kind::Disconnected) && !pair.C.Has(Kind::Disconnected));
	for (size_t i = 0; i < pair.C.Packets.size(); i++)
	{
		CHECK(NumberOf(pair.C.Packets[i].Data) == i);
	}
}

TEST(UdpReadBudgetCountsEmptyPackets)
{
	UdpParams params;
	params.ReadBudgetBytes = 640;
	UdpPair pair;
	CHECK(OpenPair(pair, params));
	if (!pair.ToClient)
	{
		return;
	}
	for (int i = 0; i < 200; i++)
	{
		pair.Client->Send(pair.ToHost, Bytes(), Delivery::Reliable);
	}
	size_t most = 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
	while (pair.H.Packets.size() < 200 && std::chrono::steady_clock::now() < deadline)
	{
		pair.C.Poll();
		const size_t before = pair.H.Packets.size();
		pair.H.Poll();
		most = (std::max)(most, pair.H.Packets.size() - before);
	}
	CHECK(pair.H.Packets.size() == 200);
	CHECK(most > 0 && most <= 10);
	CHECK(pair.Host->BytesIn == 0);
}

TEST(UdpRefusesPeersWithTooFewChannels)
{
	LogCapture log;
	std::string error;
	auto host = UdpTransport::Listen("127.0.0.1", 0, error);
	CHECK(host != nullptr);
	if (!host)
	{
		return;
	}
	auto a = Internal::UdpTestAccess::Open("127.0.0.1", host->GetPort(), false, 1, error);
	auto b = Internal::UdpTestAccess::Open("127.0.0.1", host->GetPort(), false, 1, error);
	CHECK(a != nullptr && b != nullptr);
	if (!a || !b)
	{
		return;
	}
	Endpoint h{ host.get() };
	Endpoint ca{ a.get() };
	Endpoint cb{ b.get() };
	CHECK(PumpUntil({ &h, &ca, &cb }, [&] { return ca.Has(Kind::Disconnected) && cb.Has(Kind::Disconnected); }));
	PumpUntil({ &h, &ca, &cb }, [] { return false; }, std::chrono::milliseconds(50));
	CHECK(h.Events.empty());
	CHECK(log.Warnings() == 2);
}

TEST(UdpAClientRefusesAHostWithTooFewChannels)
{
	std::string error;
	auto host = Internal::UdpTestAccess::Open("127.0.0.1", 0, true, 1, error);
	CHECK(host != nullptr);
	if (!host)
	{
		return;
	}
	auto client = UdpTransport::Connect("127.0.0.1", host->GetPort(), error);
	Endpoint h{ host.get() };
	Endpoint c{ client.get() };
	CHECK(PumpUntil({ &h, &c }, [&] { return c.Has(Kind::Disconnected); }));
	const PeerEvent* failed = c.Find(Kind::Disconnected);
	CHECK(failed && failed->Reason == DisconnectReason::ConnectFailed);
	CHECK(!c.Has(Kind::Connected));
}

TEST(UdpWarnsOnceForAPeerItCannotSendTo)
{
	LogCapture log;
	std::string error;
	auto host = Internal::UdpTestAccess::Open("127.0.0.1", 0, true, 1, error);
	CHECK(host != nullptr);
	if (!host)
	{
		return;
	}
	auto client = Internal::UdpTestAccess::Open("127.0.0.1", host->GetPort(), false, 1, error);
	Endpoint h{ host.get() };
	Endpoint c{ client.get() };
	CHECK(PumpUntil({ &h, &c }, [&] { return h.Has(Kind::Connected) && c.Has(Kind::Connected); }));
	if (!h.Has(Kind::Connected))
	{
		return;
	}
	const PeerId toClient = h.Find(Kind::Connected)->Peer;
	for (int i = 0; i < 3; i++)
	{
		host->Send(toClient, Text("loose"), Delivery::Unreliable);
		host->Send(toClient, Text("sequenced"), Delivery::UnreliableSequenced);
	}
	CHECK(log.Warnings() == 1);
	host->Send(toClient, Text("reliable"), Delivery::Reliable);
	CHECK(PumpUntil({ &h, &c }, [&] { return !c.Packets.empty(); }));
	CHECK(c.Packets.size() == 1 && c.Packets[0].Data == Text("reliable"));
}
