#include "NetCheck.h"
#include "TransportTestUtils.h"

#include <DingoNet/TcpTransport.h>

#include <charconv>
#include <cstdio>
#include <string>
#include <vector>

using namespace Dingo::Net;
using TransportTest::Endpoint;
using TransportTest::Pattern;
using TransportTest::PumpUntil;
using TransportTest::Text;

namespace
{

	using Kind = PeerEvent::Kind;

#if defined(DN_PLATFORM_WINDOWS)
	constexpr const char* PLATFORM = "windows";
#else
	constexpr const char* PLATFORM = "linux";
#endif

	constexpr int CHECK_PACKETS = 200;

	bool SplitEndpoint(std::string_view text, std::string& address, uint16_t& port)
	{
		const size_t colon = text.rfind(':');
		if (colon == std::string_view::npos)
		{
			return false;
		}
		address = std::string(text.substr(0, colon));
		const std::string_view digits = text.substr(colon + 1);
		const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), port);
		return result.ec == std::errc() && result.ptr == digits.data() + digits.size();
	}

	size_t SizeOf(int index)
	{
		switch (index)
		{
		case 0: return 0;
		case 1: return 1;
		case 2: return MAX_UNRELIABLE_BYTES;
		case 3: return 60 * 1024;
		default: return static_cast<size_t>(index * 37 % 1500);
		}
	}

	std::string PeerPlatform(const Packet& packet)
	{
		return std::string(packet.Data.begin(), packet.Data.end());
	}

	int RunHost(std::string_view endpoint)
	{
		std::string address;
		uint16_t port = 0;
		if (!SplitEndpoint(endpoint, address, port))
		{
			std::printf("net-check: expected --tcp-host=<ipv4>:<port>\n");
			return 2;
		}
		std::string error;
		auto host = TcpTransport::Listen(address, port, error);
		if (!host)
		{
			std::printf("net-check host: %s\n", error.c_str());
			return 1;
		}
		std::printf("net-check host (%s): listening on %s:%u, waiting 60 s for a client\n", PLATFORM, address.c_str(), static_cast<unsigned>(host->GetPort()));
		std::fflush(stdout);

		Endpoint h{ host.get() };
		if (!PumpUntil({ &h }, [&] { return h.Has(Kind::Connected); }, std::chrono::milliseconds(60000)))
		{
			std::printf("net-check host: FAIL, no client connected\n");
			return 1;
		}
		const PeerId client = h.Find(Kind::Connected)->Peer;
		host->Send(client, Text(PLATFORM), Delivery::Reliable);

		size_t echoed = 0;
		std::string peerPlatform;
		const bool finished = PumpUntil({ &h }, [&]
		{
			for (; echoed < h.Packets.size(); echoed++)
			{
				if (echoed == 0)
				{
					peerPlatform = PeerPlatform(h.Packets[0]);
					continue;
				}
				host->Send(client, h.Packets[echoed].Data, Delivery::Reliable);
			}
			return h.Has(Kind::Disconnected);
		}, std::chrono::milliseconds(30000));
		const PeerEvent* gone = h.Find(Kind::Disconnected);
		std::printf("net-check host: client on %s, echoed %zu packets, %llu bytes in, %llu out, %s\n", peerPlatform.c_str(),
			echoed > 0 ? echoed - 1 : 0, static_cast<unsigned long long>(host->BytesIn), static_cast<unsigned long long>(host->BytesOut),
			finished && gone && gone->Reason == DisconnectReason::Closed ? "PASS" : "FAIL");
		return finished && echoed == CHECK_PACKETS + 1 ? 0 : 1;
	}

	int RunClient(std::string_view endpoint)
	{
		std::string address;
		uint16_t port = 0;
		if (!SplitEndpoint(endpoint, address, port))
		{
			std::printf("net-check: expected --tcp-client=<ipv4>:<port>\n");
			return 2;
		}
		std::string error;
		auto client = TcpTransport::Connect(address, port, error);
		if (!client)
		{
			std::printf("net-check client: %s\n", error.c_str());
			return 1;
		}
		Endpoint c{ client.get() };
		if (!PumpUntil({ &c }, [&] { return c.Has(Kind::Connected) || c.Has(Kind::Disconnected); }, std::chrono::milliseconds(10000))
			|| !c.Has(Kind::Connected))
		{
			const PeerEvent* failed = c.Find(Kind::Disconnected);
			std::printf("net-check client: FAIL, could not connect to %s:%u (%s)\n", address.c_str(), static_cast<unsigned>(port), failed ? failed->Detail.c_str() : "no answer");
			return 1;
		}
		const PeerId host = c.Find(Kind::Connected)->Peer;
		client->Send(host, Text(PLATFORM), Delivery::Reliable);
		for (int i = 0; i < CHECK_PACKETS; i++)
		{
			client->Send(host, Pattern(SizeOf(i), static_cast<uint8_t>(i)), Delivery::Reliable);
		}

		const bool all = PumpUntil({ &c }, [&] { return c.Packets.size() >= CHECK_PACKETS + 1 || c.Has(Kind::Disconnected); }, std::chrono::milliseconds(30000));
		int intact = 0;
		for (int i = 0; i + 1 < static_cast<int>(c.Packets.size()) && i < CHECK_PACKETS; i++)
		{
			intact += c.Packets[static_cast<size_t>(i) + 1].Data == Pattern(SizeOf(i), static_cast<uint8_t>(i)) ? 1 : 0;
		}
		const std::string hostPlatform = c.Packets.empty() ? std::string("?") : PeerPlatform(c.Packets[0]);
		client->Disconnect(host);
		const bool pass = all && intact == CHECK_PACKETS;
		std::printf("net-check client (%s): host on %s, %d/%d echoed intact, %llu bytes out, %llu in, %s\n", PLATFORM, hostPlatform.c_str(), intact,
			CHECK_PACKETS, static_cast<unsigned long long>(client->BytesOut), static_cast<unsigned long long>(client->BytesIn), pass ? "PASS" : "FAIL");
		PumpUntil({ &c }, [] { return false; }, std::chrono::milliseconds(200));
		return pass ? 0 : 1;
	}

}

int RunNetCheck(std::string_view argument)
{
	constexpr std::string_view HOST = "--tcp-host=";
	constexpr std::string_view CLIENT = "--tcp-client=";
	if (argument.starts_with(HOST))
	{
		return RunHost(argument.substr(HOST.size()));
	}
	if (argument.starts_with(CLIENT))
	{
		return RunClient(argument.substr(CLIENT.size()));
	}
	return -1;
}
