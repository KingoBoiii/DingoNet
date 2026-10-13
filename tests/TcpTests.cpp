#include "TestFramework.h"
#include "TransportTestUtils.h"

#include "Platform/Socket.h"

#include <DingoNet/TcpTransport.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
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

	struct TcpPair
	{
		std::unique_ptr<TcpTransport> Host;
		std::unique_ptr<TcpTransport> Client;
		Endpoint H;
		Endpoint C;
		PeerId ToClient = 0;
		PeerId ToHost = 0;
	};

	bool OpenPair(TcpPair& pair, const TcpParams& params = {})
	{
		std::string error;
		pair.Host = TcpTransport::Listen("127.0.0.1", 0, error, params);
		if (!pair.Host || pair.Host->GetPort() == 0)
		{
			return false;
		}
		pair.Client = TcpTransport::Connect("127.0.0.1", pair.Host->GetPort(), error, params);
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

	uint16_t ClosedPort()
	{
		std::string error;
		auto probe = TcpTransport::Listen("127.0.0.1", 0, error);
		return probe ? probe->GetPort() : 0;
	}

	struct RawClient
	{
		Internal::SocketHandle Socket = Internal::INVALID_SOCKET_HANDLE;
		bool Library = false;

		~RawClient()
		{
			if (Socket != Internal::INVALID_SOCKET_HANDLE)
			{
				Internal::CloseSocket(Socket);
			}
			if (Library)
			{
				Internal::ReleaseSocketLibrary();
			}
		}

		bool Start()
		{
			std::string error;
			Library = Internal::AcquireSocketLibrary(error);
			return Library;
		}

		bool BeginConnect(uint16_t port)
		{
			Internal::SocketAddress address;
			Internal::SocketError error;
			if (!Internal::ParseAddress("127.0.0.1", port, address))
			{
				return false;
			}
			Socket = Internal::OpenStreamSocket(address.Family, error);
			if (Socket == Internal::INVALID_SOCKET_HANDLE || Internal::ConfigureStream(Socket) != Internal::SocketError::None)
			{
				return false;
			}
			error = Internal::Connect(Socket, address);
			return error == Internal::SocketError::None || error == Internal::SocketError::InProgress;
		}

		bool SendAll(const std::vector<uint8_t>& bytes)
		{
			size_t sent = 0;
			for (int attempt = 0; attempt < 1000 && sent < bytes.size(); attempt++)
			{
				Internal::SocketError error;
				const ptrdiff_t moved = Internal::Send(Socket, bytes.data() + sent, bytes.size() - sent, error);
				if (moved > 0)
				{
					sent += static_cast<size_t>(moved);
				}
				else if (error != Internal::SocketError::WouldBlock)
				{
					return false;
				}
			}
			return sent == bytes.size();
		}
	};

	bool WaitConnected(Internal::SocketHandle socket)
	{
		for (int i = 0; i < 2000; i++)
		{
			Internal::SocketError error;
			const Internal::ConnectStatus status = Internal::PollConnect(socket, error);
			if (status != Internal::ConnectStatus::Pending)
			{
				return status == Internal::ConnectStatus::Connected;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	}

}

TEST(TcpExchangesFramesBothWaysInOrder)
{
	TcpPair pair;
	CHECK(OpenPair(pair));
	if (!pair.Host || !pair.Client || pair.ToHost == 0)
	{
		return;
	}
	CHECK(pair.Host->Describe().find(std::to_string(pair.Host->GetPort())) != std::string::npos);

	const std::vector<size_t> sizes = { 0, 1, MAX_UNRELIABLE_BYTES, 60 * 1024 };
	uint64_t framed = 0;
	for (size_t i = 0; i < sizes.size(); i++)
	{
		const Delivery mode = sizes[i] == MAX_UNRELIABLE_BYTES ? Delivery::Unreliable : Delivery::Reliable;
		pair.Client->Send(pair.ToHost, Pattern(sizes[i], static_cast<uint8_t>(i)), mode);
		pair.Host->Send(pair.ToClient, Pattern(sizes[i], static_cast<uint8_t>(50 + i)), mode);
		framed += 4 + sizes[i];
	}
	CHECK(pair.Client->BytesOut == framed);
	CHECK(pair.Host->BytesOut == framed);

	CHECK(PumpUntil({ &pair.H, &pair.C }, [&] { return pair.H.Packets.size() >= sizes.size() && pair.C.Packets.size() >= sizes.size(); }));
	CHECK(pair.H.Packets.size() == sizes.size());
	CHECK(pair.C.Packets.size() == sizes.size());
	for (size_t i = 0; i < sizes.size() && i < pair.H.Packets.size() && i < pair.C.Packets.size(); i++)
	{
		CHECK(pair.H.Packets[i].From == pair.ToClient);
		CHECK(pair.H.Packets[i].Data == Pattern(sizes[i], static_cast<uint8_t>(i)));
		CHECK(pair.C.Packets[i].Data == Pattern(sizes[i], static_cast<uint8_t>(50 + i)));
	}
	CHECK(pair.Host->BytesIn == framed);
	CHECK(pair.Client->BytesIn == framed);
	CHECK(pair.H.Events.empty());
	CHECK(pair.C.Events.empty());
}

TEST(TcpConnectToAClosedPortFails)
{
	const uint16_t port = ClosedPort();
	CHECK(port != 0);
	std::string error;
	auto client = TcpTransport::Connect("127.0.0.1", port, error);
	CHECK(client != nullptr);
	if (!client)
	{
		return;
	}
	Endpoint c{ client.get() };
	const auto limit = std::chrono::milliseconds(static_cast<int>(TcpParams{}.ConnectTimeout * 1000.0) + 1000);
	CHECK(PumpUntil({ &c }, [&] { return c.Has(Kind::Disconnected) || c.Has(Kind::Connected); }, limit));
	if (c.Has(Kind::Connected))
	{
		std::printf("  skipped: another process took the freed port %u\n", static_cast<unsigned>(port));
		return;
	}
	const PeerEvent* failed = c.Find(Kind::Disconnected);
	CHECK(failed != nullptr && (failed->Reason == DisconnectReason::Refused || failed->Reason == DisconnectReason::ConnectFailed));
	CHECK(failed != nullptr && !failed->Detail.empty());
}

TEST(TcpRejectsBadAddresses)
{
	std::string error;
	CHECK(TcpTransport::Connect("not an address", 1, error) == nullptr);
	CHECK(!error.empty());
	error.clear();
	CHECK(TcpTransport::Listen("256.0.0.1", 0, error) == nullptr);
	CHECK(!error.empty());
}

TEST(TcpDisconnectDeliversThePartingPacket)
{
	TcpPair pair;
	CHECK(OpenPair(pair));
	if (pair.ToClient == 0)
	{
		return;
	}
	pair.Host->Send(pair.ToClient, Text("bye"), Delivery::Reliable);
	pair.Host->Disconnect(pair.ToClient);
	CHECK(PumpUntil({ &pair.H, &pair.C }, [&] { return pair.C.Has(Kind::Disconnected); }));
	CHECK(pair.C.Packets.size() == 1 && pair.C.Packets[0].Data == Text("bye"));
	const PeerEvent* gone = pair.C.Find(Kind::Disconnected);
	CHECK(gone != nullptr && gone->Reason == DisconnectReason::Closed);
	CHECK(pair.H.Events.empty());
}

TEST(TcpPeerClosingMidSendRaisesDisconnected)
{
	TcpPair pair;
	CHECK(OpenPair(pair));
	if (pair.ToHost == 0)
	{
		return;
	}
	const std::vector<uint8_t> chunk = Pattern(60 * 1024, 7);
	for (int i = 0; i < 40; i++)
	{
		pair.Client->Send(pair.ToHost, chunk, Delivery::Reliable);
	}
	pair.C.Poll();
	pair.H.Poll();
	pair.Host->Close();

	bool gone = PumpUntil({ &pair.C }, [&] { return pair.C.Has(Kind::Disconnected); });
	CHECK(gone);
	const PeerEvent* event = pair.C.Find(Kind::Disconnected);
	CHECK(event != nullptr && event->Peer == pair.ToHost);
	CHECK(event != nullptr && (event->Reason == DisconnectReason::Closed || event->Reason == DisconnectReason::Reset));
}

// The SIGPIPE coverage: the transport reads first, so its own reads meet a reset before any send can.
TEST(TcpSocketSendsAfterAResetFailWithoutSigpipe)
{
	RawClient client;
	CHECK(client.Start());
	Internal::SocketError error;
	Internal::SocketAddress address;
	CHECK(Internal::ParseAddress("127.0.0.1", 0, address));
	const Internal::SocketHandle listener = Internal::OpenStreamSocket(address.Family, error);
	CHECK(listener != Internal::INVALID_SOCKET_HANDLE);
	if (listener == Internal::INVALID_SOCKET_HANDLE)
	{
		return;
	}
	CHECK(Internal::Bind(listener, address) == Internal::SocketError::None);
	CHECK(Internal::Listen(listener, 1) == Internal::SocketError::None);
	CHECK(Internal::ConfigureStream(listener) == Internal::SocketError::None);
	Internal::SocketAddress bound;
	CHECK(Internal::GetLocalAddress(listener, bound) == Internal::SocketError::None);
	CHECK(client.BeginConnect(bound.Port));

	Internal::SocketHandle accepted = Internal::INVALID_SOCKET_HANDLE;
	for (int i = 0; i < 2000 && accepted == Internal::INVALID_SOCKET_HANDLE; i++)
	{
		accepted = Internal::Accept(listener, error);
		if (accepted == Internal::INVALID_SOCKET_HANDLE)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	CHECK(accepted != Internal::INVALID_SOCKET_HANDLE);
	CHECK(WaitConnected(client.Socket));
	if (accepted != Internal::INVALID_SOCKET_HANDLE)
	{
		Internal::AbortSocket(accepted);
	}

	const std::vector<uint8_t> chunk = Pattern(1024, 3);
	Internal::SocketError last = Internal::SocketError::None;
	for (int i = 0; i < 2000; i++)
	{
		Internal::Send(client.Socket, chunk.data(), chunk.size(), last);
		if (last != Internal::SocketError::None && last != Internal::SocketError::WouldBlock)
		{
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(last == Internal::SocketError::Reset || last == Internal::SocketError::Aborted);
	for (int i = 0; i < 3; i++)
	{
		Internal::Send(client.Socket, chunk.data(), chunk.size(), last);
		CHECK(last == Internal::SocketError::Reset || last == Internal::SocketError::Aborted);
	}
	Internal::CloseSocket(listener);
}

TEST(TcpFramingMatchesHeadstone)
{
	LogCapture log;
	std::string error;
	TcpParams params;
	params.MaxFrameBytes = 16;
	auto host = TcpTransport::Listen("127.0.0.1", 0, error, params);
	CHECK(host != nullptr);
	if (!host)
	{
		return;
	}
	Endpoint h{ host.get() };

	RawClient raw;
	CHECK(raw.Start());
	CHECK(raw.BeginConnect(host->GetPort()));
	CHECK(WaitConnected(raw.Socket));
	CHECK(PumpUntil({ &h }, [&] { return h.Has(Kind::Connected); }));

	CHECK(raw.SendAll({ 3, 0, 0, 0, 'a' }));
	h.Poll();
	CHECK(raw.SendAll({ 'b', 'c', 16, 0, 0, 0 }));
	CHECK(raw.SendAll(Pattern(16, 9)));
	CHECK(PumpUntil({ &h }, [&] { return h.Packets.size() >= 2; }));
	CHECK(h.Packets.size() == 2);
	CHECK(h.Packets.size() >= 1 && h.Packets[0].Data == Text("abc"));
	CHECK(h.Packets.size() >= 2 && h.Packets[1].Data == Pattern(16, 9));

	const uint64_t before = host->BytesIn;
	CHECK(raw.SendAll({ 1, 0 }));
	CHECK(PumpUntil({ &h }, [&] { return host->BytesIn == before + 2; }));
	CHECK(h.Packets.size() == 2);
	CHECK(raw.SendAll({ 0, 0, 'z' }));
	CHECK(PumpUntil({ &h }, [&] { return h.Packets.size() >= 3; }));
	CHECK(h.Packets.size() == 3 && h.Packets[2].Data == Text("z"));

	CHECK(raw.SendAll({ 17, 0, 0, 0 }));
	CHECK(PumpUntil({ &h }, [&] { return h.Has(Kind::Disconnected); }));
	const PeerEvent* dropped = h.Find(Kind::Disconnected);
	CHECK(dropped != nullptr && dropped->Reason == DisconnectReason::Error);
}

TEST(TcpDropsOversizePacketsAndLogs)
{
	LogCapture log;
	TcpPair pair;
	CHECK(OpenPair(pair));
	if (pair.ToHost == 0)
	{
		return;
	}
	pair.Client->Send(pair.ToHost, Pattern(MAX_UNRELIABLE_BYTES + 1, 1), Delivery::Unreliable);
	CHECK(log.Errors() == 1);
	pair.Client->Send(pair.ToHost, Pattern(64 * 1024 + 1, 2), Delivery::Reliable);
	CHECK(log.Errors() == 2);
	CHECK(pair.Client->BytesOut == 0);
	pair.Client->Send(pair.ToHost, Pattern(64 * 1024, 3), Delivery::Reliable);
	CHECK(PumpUntil({ &pair.H, &pair.C }, [&] { return !pair.H.Packets.empty(); }));
	CHECK(pair.H.Packets.size() == 1 && pair.H.Packets[0].Data == Pattern(64 * 1024, 3));
	CHECK(pair.C.Events.empty());
}

TEST(TcpHostTurnsAwayConnectionsPastTheLimit)
{
	LogCapture log;
	TcpParams params;
	params.MaxConnections = 1;
	TcpPair pair;
	CHECK(OpenPair(pair, params));
	if (pair.ToHost == 0)
	{
		return;
	}
	std::string error;
	auto extra = TcpTransport::Connect("127.0.0.1", pair.Host->GetPort(), error);
	CHECK(extra != nullptr);
	if (!extra)
	{
		return;
	}
	Endpoint e{ extra.get() };
	CHECK(PumpUntil({ &pair.H, &e }, [&] { return e.Has(Kind::Disconnected); }));
	CHECK(pair.H.Events.empty());
}

TEST(TcpDisconnectFlushesALargeQueueBeforeClosing)
{
	TcpPair pair;
	CHECK(OpenPair(pair));
	if (pair.ToClient == 0)
	{
		return;
	}
	constexpr int CHUNKS = 34;
	for (int i = 0; i < CHUNKS; i++)
	{
		pair.Host->Send(pair.ToClient, Pattern(60 * 1024, static_cast<uint8_t>(i)), Delivery::Reliable);
	}
	pair.Host->Send(pair.ToClient, Text("bye"), Delivery::Reliable);
	pair.Host->Disconnect(pair.ToClient);
	CHECK(PumpUntil({ &pair.H, &pair.C }, [&] { return pair.C.Has(Kind::Disconnected); }));

	CHECK(pair.C.Packets.size() == CHUNKS + 1);
	bool intact = pair.C.Packets.size() == CHUNKS + 1;
	for (int i = 0; intact && i < CHUNKS; i++)
	{
		intact = pair.C.Packets[static_cast<size_t>(i)].Data == Pattern(60 * 1024, static_cast<uint8_t>(i));
	}
	CHECK(intact);
	CHECK(!pair.C.Packets.empty() && pair.C.Packets.back().Data == Text("bye"));
	const PeerEvent* gone = pair.C.Find(Kind::Disconnected);
	CHECK(gone != nullptr && gone->Reason == DisconnectReason::Closed);
	CHECK(pair.H.Events.empty());
}

TEST(TcpReadsAtMostTheBudgetPerPoll)
{
	TcpParams params;
	params.MaxReadBytesPerPoll = 4096;
	TcpPair pair;
	CHECK(OpenPair(pair, params));
	if (pair.ToHost == 0)
	{
		return;
	}
	constexpr int COUNT = 10;
	for (int i = 0; i < COUNT; i++)
	{
		pair.Client->Send(pair.ToHost, Pattern(1000, static_cast<uint8_t>(i)), Delivery::Reliable);
	}
	pair.C.Poll();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	pair.H.Poll();
	CHECK(pair.Host->BytesIn == 4096);
	CHECK(pair.H.Packets.size() == 4);

	CHECK(PumpUntil({ &pair.H, &pair.C }, [&] { return pair.H.Packets.size() >= COUNT; }));
	CHECK(pair.H.Packets.size() == COUNT);
	for (size_t i = 0; i < pair.H.Packets.size(); i++)
	{
		CHECK(pair.H.Packets[i].Data == Pattern(1000, static_cast<uint8_t>(i)));
	}
}

TEST(TcpHostTakesANewConnectionWhileAnOldOneLingers)
{
	LogCapture log;
	TcpParams params;
	params.MaxConnections = 1;
	TcpPair pair;
	CHECK(OpenPair(pair, params));
	if (pair.ToClient == 0)
	{
		return;
	}
	pair.Host->Disconnect(pair.ToClient);
	pair.H.Poll();

	std::string error;
	auto next = TcpTransport::Connect("127.0.0.1", pair.Host->GetPort(), error);
	CHECK(next != nullptr);
	if (!next)
	{
		return;
	}
	Endpoint n{ next.get() };
	CHECK(PumpUntil({ &pair.H, &n }, [&] { return pair.H.Has(Kind::Connected); }));
	n.Poll();
	CHECK(!n.Has(Kind::Disconnected));
}
