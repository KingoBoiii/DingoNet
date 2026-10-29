#include "TestFramework.h"

#include "Platform/Socket.h"

#include <DingoNet/LanDiscovery.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Dingo::Net;
namespace Net = Dingo::Net::Internal;

namespace
{

	using SteadyClock = std::chrono::steady_clock;

	struct FakeClock
	{
		SteadyClock::time_point Now = SteadyClock::now();

		std::function<SteadyClock::time_point()> Hook()
		{
			return [this] { return Now; };
		}

		void Advance(double seconds)
		{
			Now += std::chrono::duration_cast<SteadyClock::duration>(std::chrono::duration<double>(seconds));
		}
	};

	std::unique_ptr<LanHost> OpenHost(uint16_t protocol = 7, uint32_t hash = 99, const std::string& name = "Dingo")
	{
		LanHostParams params;
		params.Port = 0;
		params.BindAddress = "127.0.0.1";
		params.Protocol = protocol;
		params.ContentHash = hash;
		params.GamePort = 4000;
		params.Name = name;
		std::string error;
		return LanHost::Open(params, error);
	}

	LanBrowserParams BrowserParamsFor(const LanHost& host, FakeClock& clock)
	{
		LanBrowserParams params;
		params.Port = host.GetPort();
		params.QueryAddress = "127.0.0.1";
		params.Protocol = 7;
		params.ContentHash = 99;
		params.Clock = clock.Hook();
		return params;
	}

	bool PumpUntilHosts(LanHost* host, LanBrowser& browser, size_t count)
	{
		for (int i = 0; i < 400; i++)
		{
			if (host)
			{
				host->Poll();
			}
			browser.Poll();
			if (browser.Hosts().size() >= count)
			{
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return false;
	}

	void Settle()
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	struct RawSocket
	{
		RawSocket()
		{
			std::string error;
			Net::AcquireSocketLibrary(error);
			Net::SocketError socketError;
			Handle = Net::OpenUdpSocket(Net::AddressFamily::IPv4, socketError);
			Net::SocketAddress any;
			Net::ParseAddress("127.0.0.1", 0, any);
			Net::Bind(Handle, any);
		}

		~RawSocket()
		{
			Net::CloseSocket(Handle);
			Net::ReleaseSocketLibrary();
		}

		void Send(uint16_t port, const std::vector<uint8_t>& bytes)
		{
			Net::SocketAddress to;
			Net::ParseAddress("127.0.0.1", port, to);
			Net::SocketError error;
			Net::SendTo(Handle, bytes.data(), bytes.size(), to, error);
		}

		std::vector<std::vector<uint8_t>> Drain()
		{
			std::vector<std::vector<uint8_t>> out;
			for (int attempt = 0; attempt < 40; attempt++)
			{
				std::vector<uint8_t> buffer(2048);
				Net::SocketAddress from;
				Net::SocketError error;
				const ptrdiff_t got = Net::RecvFrom(Handle, buffer.data(), buffer.size(), from, error);
				if (got < 0)
				{
					if (!out.empty())
					{
						break;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
					continue;
				}
				buffer.resize(static_cast<size_t>(got));
				out.push_back(std::move(buffer));
			}
			return out;
		}

		Net::SocketHandle Handle = Net::INVALID_SOCKET_HANDLE;
	};

	std::vector<uint8_t> Query(size_t size = 512)
	{
		std::vector<uint8_t> bytes = { 'D', 'N', 'L', 'A', 'N', 1, 1, 7, 0 };
		bytes.resize(size, 0);
		return bytes;
	}

	std::vector<uint8_t> Reply(uint8_t taken = 1, uint8_t max = 4, uint8_t accepting = 1, uint16_t dataSize = 0, size_t dataBytes = 0)
	{
		std::vector<uint8_t> bytes = { 'D', 'N', 'L', 'A', 'N', 2, 1, 7, 0, 99, 0, 0, 0, 0xA0, 0x0F, 1, 'H', taken, max, accepting };
		bytes.push_back(static_cast<uint8_t>(dataSize & 0xFF));
		bytes.push_back(static_cast<uint8_t>(dataSize >> 8));
		bytes.resize(bytes.size() + dataBytes, 0);
		return bytes;
	}

}

TEST(Lan_BrowserFindsHost)
{
	auto host = OpenHost();
	CHECK(host != nullptr);
	LanHostStatus status;
	status.SeatsTaken = 2;
	status.SeatsMax = 4;
	status.GameData = { 'm', 'a', 'p' };
	host->SetStatus(status);

	FakeClock clock;
	std::string error;
	auto browser = LanBrowser::Open(BrowserParamsFor(*host, clock), error);
	CHECK(browser != nullptr);
	CHECK(PumpUntilHosts(host.get(), *browser, 1));
	if (browser->Hosts().empty())
	{
		return;
	}

	const LanHostInfo& info = browser->Hosts()[0];
	CHECK(info.Address == "127.0.0.1");
	CHECK(info.GamePort == 4000);
	CHECK(info.Name == "Dingo");
	CHECK(info.Protocol == 7);
	CHECK(info.ContentHash == 99);
	CHECK(info.SeatsTaken == 2);
	CHECK(info.SeatsMax == 4);
	CHECK(info.Accepting);
	CHECK(info.GameData == std::vector<uint8_t>({ 'm', 'a', 'p' }));
	CHECK(info.ProtocolMatches);
	CHECK(info.ContentMatches);
	CHECK(browser->QueriesSent() == 1);
}

TEST(Lan_MismatchIsListedNotHidden)
{
	auto host = OpenHost(8, 5);
	FakeClock clock;
	std::string error;
	auto browser = LanBrowser::Open(BrowserParamsFor(*host, clock), error);
	CHECK(PumpUntilHosts(host.get(), *browser, 1));
	if (browser->Hosts().empty())
	{
		return;
	}
	CHECK(!browser->Hosts()[0].ProtocolMatches);
	CHECK(!browser->Hosts()[0].ContentMatches);
}

TEST(Lan_StatusChangesReachTheList)
{
	auto host = OpenHost();
	FakeClock clock;
	std::string error;
	auto browser = LanBrowser::Open(BrowserParamsFor(*host, clock), error);
	CHECK(PumpUntilHosts(host.get(), *browser, 1));
	if (browser->Hosts().empty())
	{
		return;
	}
	CHECK(browser->Hosts()[0].Accepting);

	LanHostStatus status;
	status.Accepting = false;
	status.SeatsTaken = 4;
	host->SetStatus(status);
	clock.Advance(1.5);
	for (int i = 0; i < 400 && browser->Hosts()[0].Accepting; i++)
	{
		host->Poll();
		browser->Poll();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	CHECK(browser->Hosts().size() == 1);
	CHECK(!browser->Hosts()[0].Accepting);
	CHECK(browser->Hosts()[0].SeatsTaken == 4);
}

TEST(Lan_QueriesOncePerInterval)
{
	auto host = OpenHost();
	FakeClock clock;
	std::string error;
	auto browser = LanBrowser::Open(BrowserParamsFor(*host, clock), error);
	browser->Poll();
	browser->Poll();
	clock.Advance(0.5);
	browser->Poll();
	CHECK(browser->QueriesSent() == 1);
	clock.Advance(0.6);
	browser->Poll();
	CHECK(browser->QueriesSent() == 2);
}

TEST(Lan_SilentHostExpires)
{
	auto host = OpenHost();
	FakeClock clock;
	std::string error;
	auto browser = LanBrowser::Open(BrowserParamsFor(*host, clock), error);
	CHECK(PumpUntilHosts(host.get(), *browser, 1));

	clock.Advance(2.0);
	browser->Poll();
	CHECK(browser->Hosts().size() == 1);
	clock.Advance(1.5);
	browser->Poll();
	CHECK(browser->Hosts().empty());
}

TEST(Lan_ReplyNeverLargerThanQuery)
{
	auto host = OpenHost();
	LanHostStatus status;
	status.GameData.assign(1000, 0xAB);
	host->SetStatus(status);

	RawSocket raw;
	raw.Send(host->GetPort(), Query());
	Settle();
	host->Poll();
	const auto replies = raw.Drain();
	CHECK(replies.size() == 1);
	if (!replies.empty())
	{
		CHECK(replies[0].size() <= 512);
		CHECK(replies[0].size() > 256);
	}
}

TEST(Lan_ShortQueryGetsNoAnswer)
{
	auto host = OpenHost();
	RawSocket raw;
	raw.Send(host->GetPort(), Query(20));
	Settle();
	host->Poll();
	CHECK(host->Answered() == 0);
	CHECK(host->Malformed() == 1);
}

TEST(Lan_HostDropsMalformedDatagrams)
{
	auto host = OpenHost();
	RawSocket raw;
	std::vector<uint8_t> badMagic = Query();
	badMagic[0] = 'X';
	std::vector<uint8_t> badVersion = Query();
	badVersion[6] = 9;
	std::vector<uint8_t> reply = Query();
	reply[5] = 2;
	raw.Send(host->GetPort(), badMagic);
	raw.Send(host->GetPort(), badVersion);
	raw.Send(host->GetPort(), reply);
	raw.Send(host->GetPort(), { 1 });
	Settle();
	host->Poll();
	CHECK(host->Malformed() == 4);
	CHECK(host->Answered() == 0);
}

TEST(Lan_HostAnswersAtMost32PerPoll)
{
	auto host = OpenHost();
	RawSocket raw;
	for (int i = 0; i < 40; i++)
	{
		raw.Send(host->GetPort(), Query());
	}
	Settle();
	host->Poll();
	CHECK(host->Answered() == 32);
	CHECK(host->Throttled() == 8);
}

TEST(Lan_BrowserTakesAWellFormedReply)
{
	FakeClock clock;
	LanBrowserParams params;
	params.QueryAddress = "127.0.0.1";
	params.Port = 9;
	params.Clock = clock.Hook();
	std::string error;
	auto browser = LanBrowser::Open(params, error);
	CHECK(browser != nullptr);

	RawSocket raw;
	raw.Send(browser->GetPort(), Reply());
	Settle();
	browser->Poll();
	CHECK(browser->Hosts().size() == 1);
	CHECK(browser->Malformed() == 0);
}

TEST(Lan_BrowserDropsMalformedReplies)
{
	FakeClock clock;
	LanBrowserParams params;
	params.QueryAddress = "127.0.0.1";
	params.Port = 9;
	params.Clock = clock.Hook();
	std::string error;
	auto browser = LanBrowser::Open(params, error);
	CHECK(browser != nullptr);

	RawSocket raw;
	std::vector<uint8_t> trailing = Reply();
	trailing.push_back(0);
	std::vector<uint8_t> truncated = Reply(1, 4, 1, 5, 2);
	std::vector<uint8_t> badWire = Reply();
	badWire[6] = 2;

	raw.Send(browser->GetPort(), Reply(5, 4));
	raw.Send(browser->GetPort(), Reply(1, 0));
	raw.Send(browser->GetPort(), Reply(1, 33));
	raw.Send(browser->GetPort(), Reply(1, 4, 2));
	raw.Send(browser->GetPort(), Reply(1, 4, 1, 257, 257));
	raw.Send(browser->GetPort(), trailing);
	raw.Send(browser->GetPort(), truncated);
	raw.Send(browser->GetPort(), badWire);
	raw.Send(browser->GetPort(), Query());
	raw.Send(browser->GetPort(), { 'D', 'N' });
	Settle();
	browser->Poll();
	CHECK(browser->Hosts().empty());
	CHECK(browser->Malformed() == 10);
}

TEST(Lan_HostListIsCapped)
{
	FakeClock clock;
	LanBrowserParams params;
	params.QueryAddress = "127.0.0.1";
	params.Port = 9;
	params.Clock = clock.Hook();
	std::string error;
	auto browser = LanBrowser::Open(params, error);
	CHECK(browser != nullptr);

	std::vector<std::unique_ptr<RawSocket>> senders;
	for (size_t i = 0; i < LAN_MAX_HOSTS + 6; i++)
	{
		senders.push_back(std::make_unique<RawSocket>());
		senders.back()->Send(browser->GetPort(), Reply());
	}
	Settle();
	browser->Poll();
	CHECK(browser->Hosts().size() == LAN_MAX_HOSTS);
}

TEST(Lan_DefaultFilterCleansHostName)
{
	auto host = OpenHost(7, 99, "  Dingo\x01 the Great and Mighty Host  ");
	FakeClock clock;
	std::string error;
	auto browser = LanBrowser::Open(BrowserParamsFor(*host, clock), error);
	CHECK(PumpUntilHosts(host.get(), *browser, 1));
	if (browser->Hosts().empty())
	{
		return;
	}
	CHECK(browser->Hosts()[0].Name.size() <= 16);
	CHECK(browser->Hosts()[0].Name.find('\x01') == std::string::npos);
}

TEST(Lan_BusyPortFailsToOpen)
{
	auto first = OpenHost();
	LanHostParams params;
	params.Port = first->GetPort();
	params.BindAddress = "127.0.0.1";
	std::string error;
	auto second = LanHost::Open(params, error);
	CHECK(second == nullptr);
	CHECK(!error.empty());
}

TEST(Lan_OversizedDatagramDoesNotStopThePoll)
{
	auto host = OpenHost();
	RawSocket raw;
	raw.Send(host->GetPort(), std::vector<uint8_t>(1400, 0));
	raw.Send(host->GetPort(), std::vector<uint8_t>(1500, 0));
	raw.Send(host->GetPort(), Query());
	Settle();
	host->Poll();
	CHECK(host->Malformed() == 2);
	CHECK(host->Answered() == 1);
}
