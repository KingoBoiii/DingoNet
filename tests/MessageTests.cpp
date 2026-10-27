#include "TestFramework.h"
#include "TransportTestUtils.h"

#include <DingoNet/LoopbackTransport.h>
#include <DingoNet/Messages.h>
#include <DingoNet/Session.h>

#include <memory>
#include <string>
#include <vector>

using namespace Dingo::Net;
using TransportTest::LogCapture;

namespace
{

	struct Move
	{
		static constexpr uint8_t Id = 3;
		static constexpr Direction Dir = Direction::ToHost;
		static constexpr Delivery Send = Delivery::Unreliable;
		static constexpr const char* Name = "Move";

		uint16_t Seq = 0;
		float X = 0.0f;
		std::string Tag;

		void Write(ByteWriter& w) const
		{
			w.U16(Seq);
			w.F32(X);
			w.Str(Tag);
		}

		bool Read(ByteReader& r)
		{
			Seq = r.U16();
			X = r.F32();
			Tag = r.Str();
			return r.Ok();
		}
	};

	struct Snapshot
	{
		static constexpr uint8_t Id = 4;
		static constexpr Direction Dir = Direction::ToClients;
		static constexpr Delivery Send = Delivery::Reliable;

		std::vector<uint8_t> Blob;

		void Write(ByteWriter& w) const
		{
			for (uint8_t b : Blob)
			{
				w.U8(b);
			}
		}

		bool Read(ByteReader& r)
		{
			Blob.clear();
			while (r.Remaining() > 0)
			{
				Blob.push_back(r.U8());
			}
			return true;
		}
	};

	struct Bulk
	{
		static constexpr uint8_t Id = 6;
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

	struct Chat
	{
		static constexpr uint8_t Id = 5;
		static constexpr Direction Dir = Direction::Both;
		static constexpr Delivery Send = Delivery::Reliable;
		static constexpr const char* Name = "Chat";

		uint8_t Value = 0;

		void Write(ByteWriter& w) const { w.U8(Value); }
		bool Read(ByteReader& r)
		{
			Value = r.U8();
			return r.Ok();
		}
	};

	struct Reserved
	{
		static constexpr uint8_t Id = 0xF3;
		static constexpr Direction Dir = Direction::Both;
		static constexpr Delivery Send = Delivery::Reliable;

		void Write(ByteWriter&) const {}
		bool Read(ByteReader&) { return true; }
	};

	enum class Kind : uint8_t
	{
		Ping = 9
	};

	struct EnumMsg
	{
		static constexpr Kind Id = Kind::Ping;
		static constexpr Direction Dir = Direction::Both;
		static constexpr Delivery Send = Delivery::Reliable;

		uint8_t Value = 0;

		void Write(ByteWriter& w) const { w.U8(Value); }
		bool Read(ByteReader& r)
		{
			Value = r.U8();
			return r.Ok();
		}
	};

	struct Received
	{
		int Count = 0;
		int Slot = -1;
		Move Last;
	};

	struct Nameless
	{
		static constexpr uint8_t Id = 0x2A;
		static constexpr Direction Dir = Direction::Both;
		static constexpr Delivery Send = Delivery::Unreliable;
		void Write(ByteWriter& w) const
		{
			for (size_t i = 0; i < MAX_UNRELIABLE_BYTES; i++)
			{
				w.U8(0);
			}
		}
		bool Read(ByteReader&) { return true; }
	};

	struct BulkSequenced
	{
		static constexpr uint8_t Id = 0x2B;
		static constexpr Direction Dir = Direction::Both;
		static constexpr Delivery Send = Delivery::UnreliableSequenced;
		static constexpr const char* Name = "BulkSequenced";

		size_t Size = 0;

		void Write(ByteWriter& w) const
		{
			for (size_t i = 0; i < Size; i++)
			{
				w.U8(0);
			}
		}

		bool Read(ByteReader&) { return true; }
	};

	std::vector<uint8_t> Wire(const Move& move)
	{
		return *Encode(move);
	}

}

TEST(MessageRoundTripDispatchesEqualFields)
{
	Move sent;
	sent.Seq = 513;
	sent.X = 12.5f;
	sent.Tag = "dingo";

	ByteWriter body;
	body.U8(Move::Id);
	sent.Write(body);
	std::optional<std::vector<uint8_t>> bytes = Encode(sent);
	CHECK(bytes.has_value());
	CHECK(*bytes == body.Data());

	MessageRouter router;
	Received got;
	CHECK(router.On<Move>([&](int slot, const Move& m) { got.Count++; got.Slot = slot; got.Last = m; }));
	CHECK(router.Dispatch(2, *bytes, true));
	CHECK(got.Count == 1);
	CHECK(got.Slot == 2);
	CHECK(got.Last.Seq == 513);
	CHECK(got.Last.X == 12.5f);
	CHECK(got.Last.Tag == "dingo");
}

TEST(MessageDispatchDropsTheWrongDirection)
{
	MessageRouter router;
	int moves = 0;
	int snapshots = 0;
	int chats = 0;
	router.On<Move>([&](int, const Move&) { moves++; });
	router.On<Snapshot>([&](int, const Snapshot&) { snapshots++; });
	router.On<Chat>([&](int, const Chat&) { chats++; });

	const std::vector<uint8_t> move = Wire(Move{});
	const std::vector<uint8_t> snapshot = *Encode(Snapshot{});
	const std::vector<uint8_t> chat = *Encode(Chat{});

	CHECK(!router.Dispatch(1, move, false));
	CHECK(!router.Dispatch(0, snapshot, true));
	CHECK(moves == 0 && snapshots == 0);
	CHECK(router.Stats(Move::Id).DroppedDirection == 1);
	CHECK(router.Stats(Snapshot::Id).DroppedDirection == 1);

	CHECK(router.Dispatch(1, move, true));
	CHECK(router.Dispatch(0, snapshot, false));
	CHECK(router.Dispatch(1, chat, true));
	CHECK(router.Dispatch(0, chat, false));
	CHECK(moves == 1 && snapshots == 1 && chats == 2);
}

TEST(MessageDispatchDropsMalformedBodies)
{
	MessageRouter router;
	int moves = 0;
	router.On<Move>([&](int, const Move&) { moves++; });

	std::vector<uint8_t> bytes = Wire(Move{});
	std::vector<uint8_t> truncated = bytes;
	truncated.pop_back();
	std::vector<uint8_t> trailing = bytes;
	trailing.push_back(0);

	CHECK(!router.Dispatch(1, truncated, true));
	CHECK(!router.Dispatch(1, trailing, true));
	CHECK(!router.Dispatch(1, std::vector<uint8_t>{ Move::Id }, true));
	CHECK(moves == 0);
	CHECK(router.Stats(Move::Id).DroppedMalformed == 3);
	CHECK(router.Stats(Move::Id).Received == 0);
	CHECK(router.Dispatch(1, bytes, true));
	CHECK(moves == 1);
}

TEST(MessageDispatchIgnoresEmptyAndUnknown)
{
	MessageRouter router;
	router.On<Move>([](int, const Move&) {});
	CHECK(!router.Dispatch(1, std::vector<uint8_t>{}, true));
	CHECK(!router.Dispatch(1, std::vector<uint8_t>{ 0x42, 1, 2 }, true));
	CHECK(router.DroppedUnknown() == 1);
}

TEST(MessageEncodeRejectsAnOversizeUnreliableMessage)
{
	LogCapture log;
	Bulk big;
	big.Blob.assign(MAX_UNRELIABLE_BYTES, 1);
	CHECK(!Encode(big).has_value());
	CHECK(log.Errors() == 1);
	CHECK(log.Last().find("Bulk") != std::string::npos);

	Snapshot huge;
	huge.Blob.assign(MAX_UNRELIABLE_BYTES * 2, 7);
	std::optional<std::vector<uint8_t>> bytes = Encode(huge);
	CHECK(bytes.has_value());
	CHECK(bytes->size() == huge.Blob.size() + 1);
	CHECK(log.Errors() == 1);

	Bulk fits;
	fits.Blob.assign(MAX_UNRELIABLE_BYTES - 1, 1);
	CHECK(Encode(fits).has_value());
	CHECK(log.Errors() == 1);
}

TEST(MessageEncodeCapsAnUnreliableSequencedMessageLikeAnUnreliableOne)
{
	LogCapture log;
	BulkSequenced big;
	big.Size = MAX_UNRELIABLE_BYTES;
	CHECK(!Encode(big).has_value());
	CHECK(log.Errors() == 1);
	CHECK(log.Last().find("BulkSequenced") != std::string::npos);

	BulkSequenced fits;
	fits.Size = MAX_UNRELIABLE_BYTES - 1;
	CHECK(Encode(fits).has_value());
	CHECK(log.Errors() == 1);
}

TEST(MessageLogFallsBackToTheHexIdWithoutAName)
{
	LogCapture log;
	CHECK(!Encode(Nameless{}).has_value());
	CHECK(log.Last().find("0x2A") != std::string::npos);
}

TEST(MessageRegistrationRefusesDuplicatesAndReservedIds)
{
	LogCapture log;
	MessageRouter router;
	CHECK(router.On<Move>([](int, const Move&) {}));
	CHECK(log.Errors() == 0);
	CHECK(!router.On<Move>([](int, const Move&) {}));
	CHECK(log.Errors() == 1);
	CHECK(!router.On<Reserved>([](int, const Reserved&) {}));
	CHECK(log.Errors() == 2);
	CHECK(!router.Registered(Reserved::Id));
	CHECK(router.Registered(Move::Id));
	CHECK(router.NameOf(Move::Id) == "Move");
	CHECK(router.KnownIds() == std::vector<uint8_t>{ Move::Id });
}

TEST(MessageCountersTrackTrafficAndDrops)
{
	MessageRouter router;
	router.On<Move>([](int, const Move&) {});

	Move move;
	move.Tag = "ab";
	std::optional<std::vector<uint8_t>> bytes = Encode(move);
	CHECK(bytes.has_value());
	router.NoteSent<Move>(bytes->size());
	router.NoteSent<Move>(bytes->size());
	CHECK(router.Stats(Move::Id).Sent == 2);
	CHECK(router.Stats(Move::Id).BytesOut == 2 * bytes->size());

	CHECK(router.Dispatch(1, *bytes, true));
	CHECK(!router.Dispatch(1, *bytes, false));
	std::vector<uint8_t> shortBytes = *bytes;
	shortBytes.pop_back();
	CHECK(!router.Dispatch(1, shortBytes, true));

	const MessageStats& stats = router.Stats(Move::Id);
	CHECK(stats.Received == 1);
	CHECK(stats.BytesIn == bytes->size());
	CHECK(stats.DroppedDirection == 1);
	CHECK(stats.DroppedMalformed == 1);

	router.ResetStats();
	CHECK(router.Stats(Move::Id).Received == 0);
	CHECK(router.Stats(Move::Id).Sent == 0);
	CHECK(router.Registered(Move::Id));
}

TEST(MessageSessionSendsTypedMessagesThroughTheRouter)
{
	SessionParams params;
	params.MaxSlots = 3;
	Session host(params);
	Session clientA(params);
	Session clientB(params);

	std::unique_ptr<LoopbackTransport> listen = LoopbackTransport::Listen();
	LoopbackTransport* link = listen.get();
	host.StartHost(std::move(listen), "Host");
	clientA.StartClient(LoopbackTransport::Connect(*link), "A");
	clientB.StartClient(LoopbackTransport::Connect(*link), "B");

	MessageRouter hostRouter;
	MessageRouter clientRouter;
	host.SetRouter(&hostRouter);
	clientA.SetRouter(&clientRouter);

	int hostMoves = 0;
	int hostSlot = -1;
	float hostX = 0.0f;
	hostRouter.On<Move>([&](int slot, const Move& m) { hostMoves++; hostSlot = slot; hostX = m.X; });
	int clientSnapshots = 0;
	size_t clientBlob = 0;
	clientRouter.On<Snapshot>([&](int, const Snapshot& s) { clientSnapshots++; clientBlob = s.Blob.size(); });

	auto pump = [&]
	{
		for (int i = 0; i < 4; i++)
		{
			host.Update();
			clientA.Update();
			clientB.Update();
		}
	};
	pump();
	CHECK(clientA.Connected() && clientB.Connected());

	Move move;
	move.X = 4.25f;
	clientA.Send(0, move);
	Snapshot snapshot;
	snapshot.Blob = { 1, 2, 3 };
	host.Broadcast(snapshot);
	pump();

	CHECK(hostMoves == 0);

	for (Session::Message& m : host.TakeMessages())
	{
		CHECK(hostRouter.Dispatch(m.Slot, m.Data, true));
	}
	for (Session::Message& m : clientA.TakeMessages())
	{
		CHECK(clientRouter.Dispatch(m.Slot, m.Data, false));
	}
	CHECK(hostMoves == 1);
	CHECK(hostSlot == clientA.LocalSlot());
	CHECK(hostX == 4.25f);
	CHECK(clientSnapshots == 1);
	CHECK(clientBlob == 3);
	CHECK(hostRouter.Stats(Snapshot::Id).Sent == 1);
	CHECK(clientRouter.Stats(Move::Id).Sent == 1);
	CHECK(clientB.TakeMessages().size() == 1);
}

TEST(MessageIdMayBeAnEnum)
{
	EnumMsg msg;
	msg.Value = 77;
	std::optional<std::vector<uint8_t>> bytes = Encode(msg);
	CHECK(bytes.has_value());
	CHECK((*bytes == std::vector<uint8_t>{ 9, 77 }));

	MessageRouter router;
	int got = 0;
	CHECK(router.On<EnumMsg>([&](int, const EnumMsg& m) { got = m.Value; }));
	CHECK(router.Dispatch(0, *bytes, true));
	CHECK(got == 77);
}

TEST(MessageEncodeRefusesAReservedId)
{
	LogCapture log;
	CHECK(!Encode(Reserved{}).has_value());
	CHECK(log.Errors() == 1);
}

TEST(MessageSendOnlyTypesAreKnownToTheRouter)
{
	MessageRouter router;
	router.On<Move>([](int, const Move&) {});
	router.NoteSent<Chat>(2);
	CHECK(!router.Registered(Chat::Id));
	CHECK(router.NameOf(Chat::Id) == "Chat");
	CHECK((router.KnownIds() == std::vector<uint8_t>{ Move::Id, Chat::Id }));
}

TEST(MessageHandlerMayRegisterAndDispatch)
{
	MessageRouter router;
	int chats = 0;
	bool inner = false;
	router.On<Move>([&](int slot, const Move&)
	{
		inner = router.On<Chat>([&](int, const Chat&) { chats++; });
		router.Dispatch(slot, *Encode(Chat{}), true);
	});
	CHECK(router.Dispatch(1, *Encode(Move{}), true));
	CHECK(inner);
	CHECK(chats == 1);
}

namespace
{

	struct Pair
	{
		Pair() : Host(Params()), A(Params()), B(Params())
		{
			std::unique_ptr<LoopbackTransport> listen = LoopbackTransport::Listen();
			Link = listen.get();
			Host.StartHost(std::move(listen), "Host");
			A.StartClient(LoopbackTransport::Connect(*Link), "A");
			B.StartClient(LoopbackTransport::Connect(*Link), "B");
			Pump();
		}

		static SessionParams Params()
		{
			SessionParams params;
			params.MaxSlots = 3;
			return params;
		}

		void Pump()
		{
			for (int i = 0; i < 4; i++)
			{
				Host.Update();
				A.Update();
				B.Update();
			}
		}

		Session Host;
		Session A;
		Session B;
		LoopbackTransport* Link = nullptr;
	};

}

TEST(MessageTypedBroadcastHonoursExceptSlot)
{
	Pair net;
	MessageRouter router;
	net.Host.SetRouter(&router);
	Snapshot snapshot;
	snapshot.Blob = { 1 };
	CHECK(net.Host.Broadcast(snapshot, net.A.LocalSlot()) == 1);
	net.Pump();
	CHECK(net.A.TakeMessages().empty());
	CHECK(net.B.TakeMessages().size() == 1);
	CHECK(router.Stats(Snapshot::Id).Sent == 1);
	CHECK(router.Stats(Snapshot::Id).BytesOut == 2);

	CHECK(net.Host.Broadcast(snapshot, net.A.LocalSlot()) == 1);
	CHECK(net.Host.Broadcast(snapshot, -1) == 2);
	CHECK(router.Stats(Snapshot::Id).Sent == 3);
}

TEST(MessageTypedSendWorksWithoutARouter)
{
	Pair net;
	Chat chat;
	chat.Value = 5;
	CHECK(net.A.Send(0, chat));
	net.Pump();
	std::vector<Session::Message> got = net.Host.TakeMessages();
	CHECK(got.size() == 1);
	CHECK((got[0].Data == std::vector<uint8_t>{ Chat::Id, 5 }));
}

TEST(MessageTypedSendCountsOnlyWhatReachedTheTransport)
{
	LogCapture log;
	MessageRouter router;
	Session idle;
	idle.SetRouter(&router);
	CHECK(!idle.Send(0, Chat{}));
	CHECK(idle.Broadcast(Chat{}) == 0);

	Pair net;
	net.A.SetRouter(&router);
	net.Host.SetRouter(&router);
	CHECK(!net.Host.Send(2, Chat{}) || net.Host.SlotTaken(2));
	CHECK(!net.Host.Send(9, Chat{}));
	CHECK(!net.Host.Send(0, Chat{}));
	CHECK(router.Stats(Chat::Id).Sent == (net.Host.SlotTaken(2) ? 1u : 0u));

	router.ResetStats();
	Session lonely(Pair::Params());
	std::unique_ptr<LoopbackTransport> listen = LoopbackTransport::Listen();
	LoopbackTransport* link = listen.get();
	lonely.StartHost(std::move(listen), "H");
	Session unwelcomed(Pair::Params());
	unwelcomed.StartClient(LoopbackTransport::Connect(*link), "C");
	unwelcomed.SetRouter(&router);
	lonely.SetRouter(&router);
	CHECK(!unwelcomed.Send(0, Chat{}));
	CHECK(lonely.Broadcast(Chat{}) == 0);
	CHECK(router.Stats(Chat::Id).Sent == 0);
	CHECK(router.Stats(Chat::Id).BytesOut == 0);
}

TEST(MessageTypedSendRefusesTheWrongDirection)
{
	Pair net;
	MessageRouter router;
	net.Host.SetRouter(&router);
	net.A.SetRouter(&router);
	LogCapture log;
	CHECK(!net.Host.Send(net.A.LocalSlot(), Move{}));
	CHECK(log.Errors() == 1);
	CHECK(net.Host.Broadcast(Move{}) == 0);
	CHECK(!net.A.Send(0, Snapshot{}));
	CHECK(log.Errors() == 3);
	CHECK(router.Stats(Move::Id).Sent == 0);
	CHECK(router.Stats(Snapshot::Id).Sent == 0);
	net.Pump();
	CHECK(net.A.TakeMessages().empty());
	CHECK(net.Host.TakeMessages().empty());
}

TEST(MessageTypedSendOfAnOversizeUnreliableMessageSendsNothing)
{
	Pair net;
	MessageRouter router;
	net.Host.SetRouter(&router);
	LogCapture log;
	Bulk big;
	big.Blob.assign(MAX_UNRELIABLE_BYTES, 1);
	CHECK(!net.Host.Send(net.A.LocalSlot(), big));
	CHECK(net.Host.Broadcast(big) == 0);
	CHECK(log.Errors() == 2);
	CHECK(router.Stats(Bulk::Id).Sent == 0);
	net.Pump();
	CHECK(net.A.TakeMessages().empty());
	CHECK(net.B.TakeMessages().empty());
}
