#include "TestFramework.h"
#include "TransportTestUtils.h"

#include <DingoNet/Conditioner.h>
#include <DingoNet/LoopbackTransport.h>
#include <DingoNet/Session.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace Dingo::Net;
using TransportTest::Endpoint;
using TransportTest::LogCapture;

namespace
{

	using Kind = Session::Event::Kind;
	using Bytes = std::vector<uint8_t>;

	constexpr uint16_t PROTOCOL = 31;
	constexpr uint32_t CONTENT = 0xABCD0123;

	struct Clock
	{
		std::chrono::steady_clock::time_point Now{};
	};

	struct Bulk
	{
		static constexpr uint8_t Id = 9;
		static constexpr Direction Dir = Direction::Both;
		static constexpr Delivery Send = Delivery::Unreliable;
		static constexpr const char* Name = "Bulk";

		std::vector<uint8_t> Blob;

		void Write(ByteWriter& w) const
		{
			for (uint8_t b : Blob)
			{
				w.U8(b);
			}
		}

		bool Read(ByteReader&) { return true; }
	};

	SessionParams MakeParams(Clock& clock, bool epochs)
	{
		SessionParams params;
		params.MaxSlots = 4;
		params.Protocol = PROTOCOL;
		params.ContentHash = CONTENT;
		params.Epochs = epochs;
		params.Clock = [&clock] { return clock.Now; };
		return params;
	}

	struct Node
	{
		explicit Node(SessionParams params) : S(std::move(params)) {}

		Session S;
		Conditioner* Cond = nullptr;
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

		const Session::Event* Find(Kind kind) const
		{
			for (const Session::Event& e : Events)
			{
				if (e.Type == kind)
				{
					return &e;
				}
			}
			return nullptr;
		}

		int Count(Kind kind) const
		{
			int n = 0;
			for (const Session::Event& e : Events)
			{
				n += e.Type == kind ? 1 : 0;
			}
			return n;
		}

		void Clear()
		{
			Events.clear();
			Messages.clear();
		}

		void Lag(double seconds, double jitter = 0.0)
		{
			Cond->Configure(seconds, 0.0f, jitter);
		}
	};

	// A host and its clients over loopback, every link behind a Conditioner on a fake clock.
	struct Rig
	{
		using Unseated = std::function<std::optional<Bytes>(std::span<const uint8_t>)>;

		explicit Rig(bool hostEpochs, int maxSlots = 4, Unseated unseated = {}) : Host(HostParams(hostEpochs, maxSlots, std::move(unseated)))
		{
			std::unique_ptr<LoopbackTransport> listen = LoopbackTransport::Listen();
			Link = listen.get();
			ConditionerParams cp;
			cp.Clock = [this] { return Time.Now; };
			auto cond = std::make_unique<Conditioner>(std::move(listen), cp);
			Host.Cond = cond.get();
			Host.S.StartHost(std::move(cond), "Host");
		}

		SessionParams HostParams(bool epochs, int maxSlots, Unseated unseated)
		{
			SessionParams params = Params(epochs, maxSlots);
			params.UnseatedPacket = std::move(unseated);
			return params;
		}

		SessionParams Params(bool epochs, int maxSlots = 4)
		{
			SessionParams params = MakeParams(Time, epochs);
			params.MaxSlots = maxSlots;
			return params;
		}

		Node& Join(const std::string& name, bool epochs)
		{
			Clients.push_back(std::make_unique<Node>(Params(epochs)));
			Node& node = *Clients.back();
			ConditionerParams cp;
			cp.Clock = [this] { return Time.Now; };
			auto cond = std::make_unique<Conditioner>(LoopbackTransport::Connect(*Link), cp);
			node.Cond = cond.get();
			node.S.StartClient(std::move(cond), name);
			return node;
		}

		void Step(int steps = 1, int ms = 10)
		{
			for (int i = 0; i < steps; i++)
			{
				Time.Now += std::chrono::milliseconds(ms);
				Host.Update();
				for (std::unique_ptr<Node>& client : Clients)
				{
					client->Update();
				}
			}
		}

		void Settle()
		{
			Step(80);
		}

		void ClearAll()
		{
			Host.Clear();
			for (std::unique_ptr<Node>& client : Clients)
			{
				client->Clear();
			}
		}

		Clock Time;
		Node Host;
		LoopbackTransport* Link = nullptr;
		std::vector<std::unique_ptr<Node>> Clients;
	};

	Bytes Game(uint8_t id, uint8_t value)
	{
		return Bytes({ id, value });
	}

	bool HasMessage(const Node& node, int slot, const Bytes& data)
	{
		for (const Session::Message& m : node.Messages)
		{
			if (m.Slot == slot && m.Data == data)
			{
				return true;
			}
		}
		return false;
	}

	struct Raw
	{
		explicit Raw(Rig& rig) : Link(LoopbackTransport::Connect(*rig.Link)), E(Link.get()) {}

		void Poll()
		{
			E.Poll();
			if (E.Has(PeerEvent::Kind::Connected))
			{
				Peer = E.Find(PeerEvent::Kind::Connected)->Peer;
			}
		}

		void Send(const Bytes& data)
		{
			Link->Send(Peer, data, Delivery::Reliable);
		}

		const Packet* Find(uint8_t id) const
		{
			for (const Packet& p : E.Packets)
			{
				if (!p.Data.empty() && p.Data[0] == id)
				{
					return &p;
				}
			}
			return nullptr;
		}

		std::unique_ptr<LoopbackTransport> Link;
		Endpoint E;
		PeerId Peer = 0;
	};

	Bytes RawHello(const std::string& name)
	{
		ByteWriter w;
		w.U8(0xF0);
		w.U16(k_SessionProtocol);
		w.U16(PROTOCOL);
		w.U32(CONTENT);
		w.Str(name);
		w.Str("raw");
		return w.Data();
	}

}

TEST(EpochStragglersFromTheOldEpochAreDroppedOnBothSides)
{
	LogCapture log;
	Rig rig(true);
	Node& a = rig.Join("Ann", true);
	rig.Settle();
	CHECK(a.S.Connected() && a.S.EpochsOn());

	a.Lag(0.5);
	rig.Host.Lag(0.5);
	rig.Host.S.Send(1, Game(0x10, 1), Delivery::Unreliable);
	a.S.Send(0, Game(0x20, 3), Delivery::Unreliable);
	a.S.Send(0, Game(0x21, 4), Delivery::Reliable);
	rig.Host.Lag(0.0);
	rig.Host.S.BeginEpoch();
	rig.Step(2);
	CHECK(a.S.GetEpoch() == 1);
	CHECK(a.Count(Kind::EpochChanged) == 1);

	rig.Step(80);
	CHECK(a.Messages.empty());
	CHECK(rig.Host.Messages.empty());
	CHECK(a.S.StaleDropped() == 1);
	CHECK(rig.Host.S.StaleDropped() == 2);

	a.Lag(0.0);
	rig.Host.S.Send(1, Game(0x12, 5), Delivery::Reliable);
	a.S.Send(0, Game(0x22, 6), Delivery::Unreliable);
	rig.Step(5);
	CHECK(a.Messages.size() == 1 && HasMessage(a, 0, Game(0x12, 5)));
	CHECK(rig.Host.Messages.size() == 1 && HasMessage(rig.Host, 1, Game(0x22, 6)));
	CHECK(log.Warnings() == 0 && log.Errors() == 0);
}

TEST(EpochPacketOfTheNewEpochThatBeatsItsEpochMessageIsDropped)
{
	LogCapture log;
	Rig rig(true);
	Node& a = rig.Join("Ann", true);
	rig.Settle();
	a.Clear();

	rig.Host.Lag(0.5);
	rig.Host.S.BeginEpoch();
	rig.Host.Lag(0.0);
	rig.Host.S.Send(1, Game(0x10, 1), Delivery::Unreliable);
	rig.Step(5);
	CHECK(a.S.GetEpoch() == 0);
	CHECK(a.Messages.empty());
	CHECK(a.S.StaleDropped() == 1);

	rig.Step(80);
	CHECK(a.S.GetEpoch() == 1);
	CHECK(a.Count(Kind::EpochChanged) == 1);
	const Session::Event* changed = a.Find(Kind::EpochChanged);
	CHECK(changed && changed->Epoch == 1);
	CHECK(a.Messages.empty());

	rig.Host.S.Send(1, Game(0x12, 3), Delivery::Unreliable);
	rig.Step(5);
	CHECK(a.Messages.size() == 1);
	CHECK(log.Warnings() == 0 && log.Errors() == 0);
}

TEST(EpochDropsQueuedMessagesAtBeginEpochAndAtEpoch)
{
	Rig rig(true);
	Node& a = rig.Join("Ann", true);
	rig.Settle();

	a.S.Send(0, Game(0x20, 1), Delivery::Reliable);
	rig.Time.Now += std::chrono::milliseconds(10);
	rig.Host.S.Update();
	rig.Host.S.BeginEpoch();
	CHECK(rig.Host.S.TakeMessages().empty());

	rig.Step(5);
	a.Clear();

	rig.Host.S.Send(1, Game(0x10, 1), Delivery::Reliable);
	rig.Time.Now += std::chrono::milliseconds(10);
	a.S.Update();
	rig.Host.S.BeginEpoch();
	rig.Host.S.Send(1, Game(0x11, 2), Delivery::Reliable);
	rig.Time.Now += std::chrono::milliseconds(10);
	a.S.Update();
	std::vector<Session::Message> left = a.S.TakeMessages();
	CHECK(left.size() == 1 && left[0].Data == Game(0x11, 2));
	CHECK(a.S.TakeEvents().size() == 1);

	rig.Host.S.Send(1, Game(0x12, 3), Delivery::Reliable);
	rig.Time.Now += std::chrono::milliseconds(10);
	a.S.Update();
	rig.Host.S.BeginEpoch();
	rig.Time.Now += std::chrono::milliseconds(10);
	a.S.Update();
	CHECK(a.S.TakeMessages().empty());
}

TEST(EpochWrapsFrom255To0AndTrafficStillFlows)
{
	LogCapture log;
	Rig rig(true);
	Node& a = rig.Join("Ann", true);
	rig.Settle();
	for (int i = 0; i < 255; i++)
	{
		rig.Host.S.BeginEpoch();
		rig.Step(1);
	}
	rig.Step(3);
	CHECK(rig.Host.S.GetEpoch() == 255 && a.S.GetEpoch() == 255);
	rig.ClearAll();

	rig.Host.S.Send(1, Game(0x10, 1), Delivery::Reliable);
	a.S.Send(0, Game(0x20, 2), Delivery::Unreliable);
	rig.Step(3);
	CHECK(HasMessage(a, 0, Game(0x10, 1)) && HasMessage(rig.Host, 1, Game(0x20, 2)));
	rig.ClearAll();

	a.Lag(0.5);
	a.S.Send(0, Game(0x21, 3), Delivery::Reliable);
	a.Lag(0.0);
	rig.Host.S.BeginEpoch();
	CHECK(rig.Host.S.GetEpoch() == 0);
	rig.Step(80);
	CHECK(a.S.GetEpoch() == 0);
	const Session::Event* changed = a.Find(Kind::EpochChanged);
	CHECK(changed && changed->Epoch == 0);
	CHECK(rig.Host.Messages.empty());

	rig.Host.S.Send(1, Game(0x11, 4), Delivery::Unreliable);
	a.S.Send(0, Game(0x22, 5), Delivery::Reliable);
	rig.Step(3);
	CHECK(HasMessage(a, 0, Game(0x11, 4)) && HasMessage(rig.Host, 1, Game(0x22, 5)));
	CHECK(log.Warnings() == 0 && log.Errors() == 0);
}

TEST(EpochJitterKeepsEveryDeliveredMessageInTheReceiversEpoch)
{
	LogCapture log;
	Rig rig(true);
	Node& a = rig.Join("Ann", true);
	Node& b = rig.Join("Bo", true);
	rig.Settle();
	rig.Host.Lag(0.05, 0.15);
	a.Lag(0.05, 0.15);
	b.Lag(0.05, 0.15);

	int delivered = 0;
	for (int tick = 0; tick < 400; tick++)
	{
		if (tick % 25 == 0)
		{
			rig.Host.S.BeginEpoch();
		}
		const uint8_t hostEpoch = rig.Host.S.GetEpoch();
		rig.Host.S.Send(1, Game(0x10, hostEpoch), tick % 2 ? Delivery::Reliable : Delivery::Unreliable);
		rig.Host.S.Broadcast(Game(0x11, hostEpoch), Delivery::Unreliable);
		a.S.Send(0, Game(0x20, a.S.GetEpoch()), tick % 3 ? Delivery::Unreliable : Delivery::Reliable);
		b.S.Send(0, Game(0x21, b.S.GetEpoch()), Delivery::Unreliable);
		rig.Step(1);
		for (Node* node : { &rig.Host, &a, &b })
		{
			for (const Session::Message& m : node->Messages)
			{
				CHECK(m.Data[1] == node->S.GetEpoch());
				delivered++;
			}
			node->Messages.clear();
		}
	}
	CHECK(delivered > 400);
	CHECK(rig.Host.S.StaleDropped() + a.S.StaleDropped() + b.S.StaleDropped() > 0);
	CHECK(log.Warnings() == 0 && log.Errors() == 0);
}

TEST(EpochLateJoinerGetsTheCurrentEpochInTheWelcome)
{
	LogCapture log;
	Rig rig(true);
	Node& a = rig.Join("Ann", true);
	rig.Settle();
	rig.Host.S.BeginEpoch();
	rig.Host.S.BeginEpoch();
	rig.Host.S.BeginEpoch();
	rig.Step(5);
	Node& b = rig.Join("Bo", true);
	rig.Settle();

	CHECK(b.S.Connected() && b.S.GetEpoch() == 3 && a.S.GetEpoch() == 3);
	CHECK(b.Count(Kind::EpochChanged) == 0);
	rig.ClearAll();
	b.S.Send(0, Game(0x20, 1), Delivery::Reliable);
	rig.Host.S.Send(2, Game(0x10, 2), Delivery::Unreliable);
	rig.Host.S.Broadcast(Game(0x11, 3), Delivery::Reliable);
	rig.Step(5);
	CHECK(HasMessage(rig.Host, 2, Game(0x20, 1)));
	CHECK(HasMessage(b, 0, Game(0x10, 2)) && HasMessage(b, 0, Game(0x11, 3)) && HasMessage(a, 0, Game(0x11, 3)));
	CHECK(log.Warnings() == 0 && log.Errors() == 0);
}

TEST(EpochClientAdoptsTheHostsEpochsFlagEitherWay)
{
	LogCapture log;
	{
		Rig rig(true);
		Node& off = rig.Join("Ann", false);
		CHECK(off.S.MaxUnreliablePayload() == MAX_UNRELIABLE_BYTES);
		rig.Settle();
		CHECK(off.S.EpochsOn() && off.S.MaxUnreliablePayload() == MAX_UNRELIABLE_BYTES - 2);
		rig.Host.S.BeginEpoch();
		rig.Step(5);
		CHECK(off.S.GetEpoch() == 1 && off.Count(Kind::EpochChanged) == 1);
		rig.ClearAll();
		off.S.Send(0, Game(0x20, 1), Delivery::Reliable);
		rig.Host.S.Send(1, Game(0x10, 2), Delivery::Reliable);
		rig.Step(5);
		CHECK(HasMessage(rig.Host, 1, Game(0x20, 1)) && HasMessage(off, 0, Game(0x10, 2)));
	}
	{
		Rig rig(false);
		Node& on = rig.Join("Ann", true);
		CHECK(on.S.MaxUnreliablePayload() == MAX_UNRELIABLE_BYTES - 2);
		rig.Settle();
		CHECK(!on.S.EpochsOn() && on.S.MaxUnreliablePayload() == MAX_UNRELIABLE_BYTES);
		rig.ClearAll();
		on.S.Send(0, Game(0x20, 1), Delivery::Reliable);
		rig.Host.S.Send(1, Game(0x10, 2), Delivery::Reliable);
		rig.Step(5);
		CHECK(HasMessage(rig.Host, 1, Game(0x20, 1)) && HasMessage(on, 0, Game(0x10, 2)));
	}
	CHECK(log.Warnings() == 0 && log.Errors() == 0);
}

TEST(EpochsOffPutGameMessagesOnTheWireExactlyAsV01Did)
{
	LogCapture log;
	Rig rig(false);
	Raw raw(rig);
	raw.Poll();
	rig.Step(1);
	raw.Poll();
	CHECK(raw.Peer != 0);
	raw.Send(RawHello("Zed"));
	rig.Step(3);
	raw.E.Packets.clear();
	raw.Poll();
	const Packet* welcome = raw.Find(0xF1);
	CHECK(welcome && welcome->Data == Bytes({ 0xF1, 1, 4, 0, 0, 4, 'H', 'o', 's', 't', 0 }));
	raw.E.Packets.clear();

	rig.Host.S.Send(1, Bytes({ 0x10, 7, 8 }), Delivery::Reliable);
	rig.Host.S.Send(1, Bytes({ 0x11 }), Delivery::Unreliable);
	rig.Host.S.Broadcast(Bytes({ 0x12, 1 }), Delivery::Unreliable);
	rig.Host.S.Send(1, Bytes(), Delivery::Reliable);
	rig.Step(3);
	raw.Poll();
	std::vector<Bytes> got;
	for (const Packet& p : raw.E.Packets)
	{
		if (p.Data.empty() || p.Data[0] < 0xF0)
		{
			got.push_back(p.Data);
		}
	}
	CHECK(got == std::vector<Bytes>({ Bytes({ 0x10, 7, 8 }), Bytes({ 0x11 }), Bytes({ 0x12, 1 }), Bytes() }));

	raw.Send(Bytes({ 0x20, 9 }));
	rig.Step(3);
	CHECK(rig.Host.Messages.size() == 1 && HasMessage(rig.Host, 1, Bytes({ 0x20, 9 })));
	rig.Host.Clear();

	raw.Send(Bytes({ 0xF9, 0, 0x20, 1 }));
	raw.Send(Bytes({ 0xF8, 1 }));
	rig.Step(3);
	CHECK(rig.Host.Messages.empty() && rig.Host.S.GetEpoch() == 0);
	CHECK(log.Warnings() == 2);

	const int warnings = log.Warnings();
	rig.Host.S.BeginEpoch();
	CHECK(log.Warnings() == warnings + 1);
	CHECK(rig.Host.S.GetEpoch() == 0);
	rig.Step(3);
	raw.E.Packets.clear();
	raw.Poll();
	CHECK(raw.Find(0xF8) == nullptr);

	CHECK(rig.Host.S.MaxUnreliablePayload() == 1100);
	rig.Host.S.Send(1, Bytes(1100, 0x30), Delivery::Unreliable);
	CHECK(log.Errors() == 0);
	rig.Host.S.Send(1, Bytes(1101, 0x30), Delivery::Unreliable);
	CHECK(log.Errors() == 1);
}

TEST(EpochsOnStampEveryGameMessageAndCheckEveryStamp)
{
	LogCapture log;
	Rig rig(true);
	Raw raw(rig);
	raw.Poll();
	rig.Step(1);
	raw.Poll();
	raw.Send(RawHello("Zed"));
	rig.Step(3);
	raw.E.Packets.clear();
	raw.Poll();
	const Packet* welcome = raw.Find(0xF1);
	CHECK(welcome && welcome->Data == Bytes({ 0xF1, 1, 4, 1, 0, 4, 'H', 'o', 's', 't', 0 }));

	rig.Host.S.BeginEpoch();
	rig.Step(3);
	raw.E.Packets.clear();
	raw.Poll();
	const Packet* epoch = raw.Find(0xF8);
	CHECK(epoch && epoch->Data == Bytes({ 0xF8, 1 }));
	raw.E.Packets.clear();

	rig.Host.S.Send(1, Bytes({ 0x10, 7 }), Delivery::Reliable);
	rig.Host.S.Send(1, Bytes({ 0x11 }), Delivery::Unreliable);
	rig.Host.S.Broadcast(Bytes({ 0x12, 1, 2 }), Delivery::Unreliable);
	CHECK(!rig.Host.S.Send(1, Bytes(), Delivery::Reliable));
	CHECK(log.Errors() == 1);
	rig.Step(3);
	raw.Poll();
	std::vector<Bytes> got;
	for (const Packet& p : raw.E.Packets)
	{
		if (p.Data[0] == 0xF9)
		{
			got.push_back(p.Data);
		}
	}
	CHECK(got == std::vector<Bytes>({ Bytes({ 0xF9, 1, 0x10, 7 }), Bytes({ 0xF9, 1, 0x11 }), Bytes({ 0xF9, 1, 0x12, 1, 2 }) }));

	raw.Send(Bytes({ 0xF9, 1, 0x20, 5 }));
	rig.Step(3);
	CHECK(rig.Host.Messages.size() == 1 && HasMessage(rig.Host, 1, Bytes({ 0x20, 5 })));
	rig.Host.Clear();

	const int warnings = log.Warnings();
	raw.Send(Bytes({ 0xF9, 0, 0x20, 6 }));
	raw.Send(Bytes({ 0xF9, 2, 0x20, 6 }));
	rig.Step(3);
	CHECK(rig.Host.Messages.empty());
	CHECK(rig.Host.S.StaleDropped() == 2);
	CHECK(log.Warnings() == warnings);

	raw.Send(Bytes({ 0x20, 7 }));
	raw.Send(Bytes({ 0xF9, 1 }));
	raw.Send(Bytes({ 0xF9 }));
	raw.Send(Bytes({ 0xF8, 2 }));
	rig.Step(3);
	CHECK(rig.Host.Messages.empty());
	CHECK(rig.Host.S.GetEpoch() == 1);
	CHECK(log.Warnings() == warnings + 2);
	CHECK(rig.Host.S.SeatedCount() == 2);
}

TEST(EpochUnseatedPeersNeverStampAndPartingMessagesSeeTheInnerBytes)
{
	LogCapture log;
	Bytes seen;
	Rig rig(true, 4, [&seen](std::span<const uint8_t> data) -> std::optional<Bytes>
	{
		seen.assign(data.begin(), data.end());
		return Bytes({ 0x01, 0x02 });
	});

	Raw raw(rig);
	raw.Poll();
	rig.Step(1);
	raw.Poll();
	raw.Send(Bytes({ 0xF9, 0, 0x33, 4 }));
	rig.Step(3);
	raw.Poll();
	CHECK(seen.empty() && raw.Find(0x01) == nullptr);
	CHECK(rig.Host.S.StaleDropped() == 0 && log.Warnings() == 1);
	raw.Send(Bytes({ 0x33, 4 }));
	rig.Step(3);
	raw.Poll();
	CHECK(seen == Bytes({ 0x33, 4 }) && raw.Find(0x01) != nullptr);

	Node& a = rig.Join("Ann", true);
	rig.Settle();
	CHECK(a.S.Connected());
	a.S.Send(0, Game(0x20, 9), Delivery::Reliable);
	a.S.Stop();
	rig.Step(3);
	const Session::Event* left = rig.Host.Find(Kind::Left);
	CHECK(left && left->LastMessages.size() == 1 && left->LastMessages[0] == Game(0x20, 9));
	CHECK(rig.Host.Messages.empty());
}

TEST(EpochDropsAStampedSessionIdAsMalformed)
{
	LogCapture log;
	Rig rig(true);
	Raw raw(rig);
	raw.Poll();
	rig.Step(1);
	raw.Poll();
	raw.Send(RawHello("Zed"));
	rig.Step(3);
	raw.Send(Bytes({ 0xF9, 0, 0xF5, 0 }));
	raw.Send(Bytes({ 0xF9, 0, 0xFF }));
	rig.Step(3);
	CHECK(rig.Host.S.SeatedCount() == 2 && rig.Host.Messages.empty() && rig.Host.Count(Kind::Left) == 0);
	CHECK(log.Warnings() == 1);
}

TEST(EpochBeginEpochAlsoDropsPendingPartingMessages)
{
	Rig rig(true);
	Node& a = rig.Join("Ann", true);
	rig.Settle();
	rig.ClearAll();

	a.S.Send(0, Game(0x20, 1), Delivery::Reliable);
	a.S.Stop();
	rig.Time.Now += std::chrono::milliseconds(10);
	rig.Host.S.Update();
	rig.Host.S.BeginEpoch();
	int lefts = 0;
	for (const Session::Event& e : rig.Host.S.TakeEvents())
	{
		lefts += e.Type == Kind::Left ? 1 : 0;
		CHECK(e.Type != Kind::Left || e.LastMessages.empty());
	}
	CHECK(lefts == 1);
}

TEST(EpochMaxUnreliablePayloadIs1098WithEpochsOn)
{
	LogCapture log;
	Rig rig(true);
	Node& a = rig.Join("Ann", true);
	rig.Settle();
	rig.ClearAll();
	CHECK(rig.Host.S.MaxUnreliablePayload() == 1098 && a.S.MaxUnreliablePayload() == 1098);

	Bytes fits(1098, 0x30);
	Bytes over(1099, 0x30);
	CHECK(rig.Host.S.Send(1, fits, Delivery::Unreliable));
	CHECK(a.S.Send(0, fits, Delivery::Unreliable));
	CHECK(rig.Host.S.Broadcast(fits, Delivery::Unreliable) == 1);
	CHECK(log.Errors() == 0);
	CHECK(!rig.Host.S.Send(1, over, Delivery::Unreliable));
	CHECK(!a.S.Send(0, over, Delivery::Unreliable));
	CHECK(rig.Host.S.Broadcast(over, Delivery::Unreliable) == 0);
	CHECK(a.S.Broadcast(over, Delivery::Unreliable) == 0);
	CHECK(log.Errors() == 4);
	CHECK(rig.Host.S.Send(1, over, Delivery::Reliable));
	rig.Step(5);
	CHECK(a.Messages.size() == 3 && rig.Host.Messages.size() == 1);

	MessageRouter router;
	rig.Host.S.SetRouter(&router);
	a.S.SetRouter(&router);
	Bulk good;
	good.Blob.assign(1097, 1);
	Bulk bad;
	bad.Blob.assign(1098, 1);
	CHECK(rig.Host.S.Send(1, good));
	CHECK(rig.Host.S.Broadcast(good) == 1);
	CHECK(router.Stats(Bulk::Id).Sent == 2);
	const int errors = log.Errors();
	CHECK(!rig.Host.S.Send(1, bad));
	CHECK(rig.Host.S.Broadcast(bad) == 0);
	CHECK(!a.S.Send(0, bad));
	CHECK(a.S.Broadcast(bad) == 0);
	CHECK(log.Errors() == errors + 4);
	CHECK(log.Last().find("Bulk") != std::string::npos);
	CHECK(router.Stats(Bulk::Id).Sent == 2);
	rig.Host.S.SetRouter(nullptr);
	a.S.SetRouter(nullptr);
}
