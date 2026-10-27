#include "TestFramework.h"
#include "TransportTestUtils.h"

#include <DingoNet/Bytes.h>
#include <DingoNet/Conditioner.h>
#include <DingoNet/LoopbackTransport.h>

#include <chrono>
#include <memory>
#include <vector>

using namespace Dingo::Net;
using TransportTest::Endpoint;
using TransportTest::LogCapture;
using TransportTest::Pattern;
using TransportTest::Text;

namespace
{

	using Kind = PeerEvent::Kind;

	struct FakeClock
	{
		std::chrono::steady_clock::time_point Now{};

		void Advance(std::chrono::milliseconds step)
		{
			Now += step;
		}
	};

	std::vector<uint8_t> Numbered(uint32_t index, bool reliable)
	{
		ByteWriter w;
		w.U8(reliable ? 1 : 0);
		w.U32(index);
		return w.Data();
	}

}

TEST(LoopbackDeliversBothWaysInOrder)
{
	LoopbackPair pair = CreateLoopbackPair();
	Endpoint host{ pair.Host.get() };
	Endpoint client{ pair.Client.get() };

	host.Poll();
	client.Poll();
	CHECK(host.Has(Kind::Connected));
	CHECK(client.Has(Kind::Connected));
	if (!host.Has(Kind::Connected) || !client.Has(Kind::Connected))
	{
		return;
	}
	const PeerId toClient = host.Find(Kind::Connected)->Peer;
	const PeerId toHost = client.Find(Kind::Connected)->Peer;

	const std::vector<size_t> sizes = { 0, 1, MAX_UNRELIABLE_BYTES, 60 * 1024 };
	for (size_t i = 0; i < sizes.size(); i++)
	{
		const Delivery mode = sizes[i] <= MAX_UNRELIABLE_BYTES && i % 2 == 0 ? Delivery::Unreliable : Delivery::Reliable;
		pair.Client->Send(toHost, Pattern(sizes[i], static_cast<uint8_t>(i)), mode);
		pair.Host->Send(toClient, Pattern(sizes[i], static_cast<uint8_t>(100 + i)), mode);
	}
	host.Poll();
	client.Poll();

	CHECK(host.Packets.size() == sizes.size());
	CHECK(client.Packets.size() == sizes.size());
	for (size_t i = 0; i < sizes.size() && i < host.Packets.size() && i < client.Packets.size(); i++)
	{
		CHECK(host.Packets[i].From == toClient);
		CHECK(host.Packets[i].Data == Pattern(sizes[i], static_cast<uint8_t>(i)));
		CHECK(client.Packets[i].Data == Pattern(sizes[i], static_cast<uint8_t>(100 + i)));
	}
	const uint64_t total = 1 + MAX_UNRELIABLE_BYTES + 60 * 1024;
	CHECK(pair.Client->BytesOut == total);
	CHECK(pair.Host->BytesIn == total);
	CHECK(pair.Host->BytesOut == total);
	CHECK(pair.Client->BytesIn == total);
}

TEST(LoopbackDisconnectReachesTheOtherSideAfterItsLastPacket)
{
	{
		LoopbackPair pair = CreateLoopbackPair();
		Endpoint host{ pair.Host.get() };
		Endpoint client{ pair.Client.get() };
		host.Poll();
		client.Poll();
		const PeerId toHost = client.Find(Kind::Connected)->Peer;
		pair.Client->Send(toHost, Text("bye"), Delivery::Reliable);
		pair.Client->Disconnect(toHost);
		client.Events.clear();
		host.Poll();
		client.Poll();
		CHECK(host.Packets.size() == 1 && host.Packets[0].Data == Text("bye"));
		const PeerEvent* gone = host.Find(Kind::Disconnected);
		CHECK(gone != nullptr && gone->Reason == DisconnectReason::Closed);
		CHECK(client.Events.empty());
	}
	{
		LoopbackPair pair = CreateLoopbackPair();
		Endpoint host{ pair.Host.get() };
		Endpoint client{ pair.Client.get() };
		host.Poll();
		client.Poll();
		const PeerId toClient = host.Find(Kind::Connected)->Peer;
		pair.Host->Send(toClient, Text("host left"), Delivery::Reliable);
		pair.Host->Close();
		client.Poll();
		CHECK(client.Packets.size() == 1 && client.Packets[0].Data == Text("host left"));
		const PeerEvent* gone = client.Find(Kind::Disconnected);
		CHECK(gone != nullptr && gone->Reason == DisconnectReason::Closed);
	}
}

TEST(LoopbackHostTakesManyClientsAndRefusesOnceClosed)
{
	auto host = LoopbackTransport::Listen();
	std::vector<std::unique_ptr<LoopbackTransport>> clients;
	for (int i = 0; i < 3; i++)
	{
		clients.push_back(LoopbackTransport::Connect(*host));
	}
	Endpoint h{ host.get() };
	h.Poll();
	CHECK(h.Events.size() == 3);
	for (size_t i = 0; i < clients.size(); i++)
	{
		Endpoint c{ clients[i].get() };
		c.Poll();
		CHECK(c.Has(Kind::Connected));
		clients[i]->Send(c.Find(Kind::Connected)->Peer, Pattern(8, static_cast<uint8_t>(i)), Delivery::Reliable);
	}
	h.Poll();
	CHECK(h.Packets.size() == 3);
	if (h.Packets.size() == 3)
	{
		CHECK(h.Packets[0].From != h.Packets[1].From && h.Packets[1].From != h.Packets[2].From);
	}

	host->Close();
	auto late = LoopbackTransport::Connect(*host);
	Endpoint l{ late.get() };
	l.Poll();
	CHECK(!l.Has(Kind::Connected));
	const PeerEvent* refused = l.Find(Kind::Disconnected);
	CHECK(refused != nullptr && refused->Reason == DisconnectReason::Refused);
}

TEST(LoopbackDropsOversizeUnreliableAndLogs)
{
	LogCapture log;
	LoopbackPair pair = CreateLoopbackPair();
	Endpoint host{ pair.Host.get() };
	Endpoint client{ pair.Client.get() };
	host.Poll();
	client.Poll();
	const PeerId toHost = client.Find(Kind::Connected)->Peer;

	pair.Client->Send(toHost, Pattern(MAX_UNRELIABLE_BYTES + 1, 1), Delivery::Unreliable);
	CHECK(log.Errors() == 1);
	pair.Client->Send(toHost, Pattern(MAX_UNRELIABLE_BYTES + 1, 2), Delivery::Reliable);
	CHECK(log.Errors() == 1);
	host.Poll();
	CHECK(host.Packets.size() == 1 && host.Packets[0].Data == Pattern(MAX_UNRELIABLE_BYTES + 1, 2));
}

TEST(ConditionerHoldsPacketsForTheLag)
{
	FakeClock clock;
	LoopbackPair pair = CreateLoopbackPair();
	ConditionerParams params;
	params.LagSeconds = 0.2;
	params.Clock = [&clock] { return clock.Now; };
	Conditioner conditioned(std::move(pair.Client), params);
	Endpoint host{ pair.Host.get() };
	Endpoint client{ &conditioned };
	host.Poll();
	client.Poll();
	const PeerId toHost = client.Find(Kind::Connected)->Peer;

	conditioned.Send(toHost, Text("late"), Delivery::Reliable);
	clock.Advance(std::chrono::milliseconds(199));
	client.Poll();
	host.Poll();
	CHECK(host.Packets.empty());
	clock.Advance(std::chrono::milliseconds(1));
	client.Poll();
	host.Poll();
	CHECK(host.Packets.size() == 1);
	CHECK(conditioned.BytesOut == 4);
}

TEST(ConditionerKeepsEveryReliablePacketInOrderUnderLagAndLoss)
{
	FakeClock clock;
	LoopbackPair pair = CreateLoopbackPair();
	ConditionerParams params;
	params.LagSeconds = 0.2;
	params.JitterSeconds = 0.05;
	params.Loss = 0.05f;
	params.Clock = [&clock] { return clock.Now; };
	Conditioner conditioned(std::move(pair.Client), params);
	Endpoint host{ pair.Host.get() };
	Endpoint client{ &conditioned };
	host.Poll();
	client.Poll();
	const PeerId toHost = client.Find(Kind::Connected)->Peer;

	constexpr uint32_t COUNT = 400;
	for (uint32_t i = 0; i < COUNT; i++)
	{
		conditioned.Send(toHost, Numbered(i, true), Delivery::Reliable);
		conditioned.Send(toHost, Numbered(i, false), Delivery::Unreliable);
		clock.Advance(std::chrono::milliseconds(5));
		client.Poll();
		host.Poll();
	}
	for (int i = 0; i < 100 && conditioned.GetQueuedCount() > 0; i++)
	{
		clock.Advance(std::chrono::milliseconds(10));
		client.Poll();
		host.Poll();
	}
	CHECK(conditioned.GetQueuedCount() == 0);

	uint32_t nextReliable = 0;
	uint32_t unreliable = 0;
	bool inOrder = true;
	for (const Packet& p : host.Packets)
	{
		ByteReader r(p.Data);
		const bool reliable = r.U8() == 1;
		const uint32_t index = r.U32();
		if (reliable)
		{
			inOrder = inOrder && index == nextReliable;
			nextReliable++;
		}
		else
		{
			unreliable++;
		}
	}
	CHECK(inOrder);
	CHECK(nextReliable == COUNT);
	CHECK(unreliable + conditioned.Dropped == COUNT);
	CHECK(conditioned.Dropped > 0);
	CHECK(conditioned.Dropped < COUNT / 10);
}

TEST(ConditionerLosesBothUnreliableKindsAndNeverLosesReliable)
{
	FakeClock clock;
	LoopbackPair pair = CreateLoopbackPair();
	ConditionerParams params;
	params.Loss = 1.0f;
	params.Clock = [&clock] { return clock.Now; };
	Conditioner conditioned(std::move(pair.Client), params);
	Endpoint host{ pair.Host.get() };
	Endpoint client{ &conditioned };
	host.Poll();
	client.Poll();
	const PeerId toHost = client.Find(Kind::Connected)->Peer;

	conditioned.Send(toHost, Text("loose"), Delivery::Unreliable);
	conditioned.Send(toHost, Text("sequenced"), Delivery::UnreliableSequenced);
	conditioned.Send(toHost, Text("kept"), Delivery::Reliable);
	client.Poll();
	host.Poll();
	CHECK(host.Packets.size() == 1 && host.Packets[0].Data == Text("kept"));
	CHECK(conditioned.Dropped == 2);
}

TEST(ConditionerNeverReleasesASequencedPacketAfterANewerOne)
{
	FakeClock clock;
	LoopbackPair pair = CreateLoopbackPair();
	ConditionerParams params;
	params.LagSeconds = 0.1;
	params.JitterSeconds = 0.08;
	params.Clock = [&clock] { return clock.Now; };
	Conditioner conditioned(std::move(pair.Client), params);
	Endpoint host{ pair.Host.get() };
	Endpoint client{ &conditioned };
	host.Poll();
	client.Poll();
	const PeerId toHost = client.Find(Kind::Connected)->Peer;

	constexpr uint32_t COUNT = 300;
	for (uint32_t i = 0; i < COUNT; i++)
	{
		conditioned.Send(toHost, Numbered(i, false), Delivery::UnreliableSequenced);
		conditioned.Send(toHost, Numbered(i, true), Delivery::Unreliable);
		clock.Advance(std::chrono::milliseconds(2));
		client.Poll();
		host.Poll();
	}
	for (int i = 0; i < 100 && conditioned.GetQueuedCount() > 0; i++)
	{
		clock.Advance(std::chrono::milliseconds(10));
		client.Poll();
		host.Poll();
	}
	CHECK(conditioned.GetQueuedCount() == 0);

	uint32_t sequenced = 0;
	uint32_t unsequenced = 0;
	bool forward = true;
	bool first = true;
	uint32_t last = 0;
	for (const Packet& p : host.Packets)
	{
		ByteReader r(p.Data);
		const bool loose = r.U8() == 1;
		const uint32_t index = r.U32();
		if (loose)
		{
			unsequenced++;
			continue;
		}
		forward = forward && (first || index > last);
		first = false;
		last = index;
		sequenced++;
	}
	CHECK(forward);
	CHECK(unsequenced == COUNT);
	CHECK(sequenced > 0 && sequenced < COUNT);
	CHECK(sequenced + conditioned.Dropped == COUNT);
}

TEST(ConditionerDisconnectSendsHeldPacketsFirst)
{
	FakeClock clock;
	LoopbackPair pair = CreateLoopbackPair();
	ConditionerParams params;
	params.LagSeconds = 0.5;
	params.Clock = [&clock] { return clock.Now; };
	Conditioner conditioned(std::move(pair.Client), params);
	Endpoint host{ pair.Host.get() };
	Endpoint client{ &conditioned };
	host.Poll();
	client.Poll();
	const PeerId toHost = client.Find(Kind::Connected)->Peer;

	conditioned.Send(toHost, Text("bye"), Delivery::Reliable);
	conditioned.Disconnect(toHost);
	host.Poll();
	CHECK(host.Packets.size() == 1 && host.Packets[0].Data == Text("bye"));
	CHECK(host.Has(Kind::Disconnected));
}

TEST(ConditionerDropsOversizeUnreliableAndLogs)
{
	LogCapture log;
	LoopbackPair pair = CreateLoopbackPair();
	ConditionerParams params;
	params.LagSeconds = 0.1;
	Conditioner conditioned(std::move(pair.Client), params);
	Endpoint client{ &conditioned };
	client.Poll();
	conditioned.Send(client.Find(Kind::Connected)->Peer, Pattern(MAX_UNRELIABLE_BYTES + 1, 0), Delivery::Unreliable);
	CHECK(log.Errors() == 1);
	CHECK(conditioned.GetQueuedCount() == 0);
	CHECK(conditioned.Dropped == 0);
}

TEST(ConditionerDropsHeldPacketsForALostPeer)
{
	FakeClock clock;
	LoopbackPair pair = CreateLoopbackPair();
	ConditionerParams params;
	params.LagSeconds = 0.5;
	params.Clock = [&clock] { return clock.Now; };
	Conditioner conditioned(std::move(pair.Client), params);
	Endpoint host{ pair.Host.get() };
	Endpoint client{ &conditioned };
	host.Poll();
	client.Poll();
	const PeerId toHost = client.Find(Kind::Connected)->Peer;
	const PeerId toClient = host.Find(Kind::Connected)->Peer;

	conditioned.Send(toHost, Text("held"), Delivery::Reliable);
	conditioned.Send(toHost, Text("held too"), Delivery::Unreliable);
	CHECK(conditioned.GetQueuedCount() == 2);
	pair.Host->Disconnect(toClient);
	client.Poll();
	CHECK(client.Has(Kind::Disconnected));
	CHECK(conditioned.GetQueuedCount() == 0);
}

TEST(ConditionerFollowsAClockThatJumpsBack)
{
	FakeClock clock;
	clock.Advance(std::chrono::milliseconds(10000));
	LoopbackPair pair = CreateLoopbackPair();
	ConditionerParams params;
	params.LagSeconds = 0.1;
	params.Clock = [&clock] { return clock.Now; };
	Conditioner conditioned(std::move(pair.Client), params);
	Endpoint host{ pair.Host.get() };
	Endpoint client{ &conditioned };
	host.Poll();
	client.Poll();
	const PeerId toHost = client.Find(Kind::Connected)->Peer;

	conditioned.Send(toHost, Text("first"), Delivery::Reliable);
	clock.Advance(std::chrono::milliseconds(100));
	client.Poll();
	host.Poll();
	CHECK(host.Packets.size() == 1);

	clock.Now = std::chrono::steady_clock::time_point{};
	conditioned.Send(toHost, Text("second"), Delivery::Reliable);
	clock.Advance(std::chrono::milliseconds(100));
	client.Poll();
	host.Poll();
	CHECK(host.Packets.size() == 2);
}
