#include "TestFramework.h"

#include "SteamTransport.h"

#include <DingoNet/Bytes.h>
#include <DingoNet/Session.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
#include <steam/steam_api.h>
#include <steam/isteamnetworkingmessages.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace Dingo::Net;

// Stands in for steam_api's callback manager, which CCallbackBase befriends.
class CCallbackMgr
{
public:
	static void Register(CCallbackBase* callback, int id)
	{
		callback->m_nCallbackFlags = static_cast<uint8>(callback->m_nCallbackFlags | CCallbackBase::k_ECallbackFlagsRegistered);
		callback->m_iCallback = id;
		Callbacks().push_back(callback);
	}

	static void Unregister(CCallbackBase* callback)
	{
		callback->m_nCallbackFlags = static_cast<uint8>(callback->m_nCallbackFlags & ~CCallbackBase::k_ECallbackFlagsRegistered);
		std::erase(Callbacks(), callback);
	}

	template<typename T>
	static void Dispatch(T& param)
	{
		const std::vector<CCallbackBase*> callbacks = Callbacks();
		for (CCallbackBase* callback : callbacks)
		{
			if (callback->m_iCallback == T::k_iCallback)
			{
				callback->Run(&param);
			}
		}
	}

	static size_t Count()
	{
		return Callbacks().size();
	}

private:
	static std::vector<CCallbackBase*>& Callbacks()
	{
		static std::vector<CCallbackBase*> s_Callbacks;
		return s_Callbacks;
	}
};

S_API void S_CALLTYPE SteamAPI_RegisterCallback(CCallbackBase* callback, int id)
{
	CCallbackMgr::Register(callback, id);
}

S_API void S_CALLTYPE SteamAPI_UnregisterCallback(CCallbackBase* callback)
{
	CCallbackMgr::Unregister(callback);
}

namespace
{

	using Bytes = std::vector<uint8_t>;
	using Kind = PeerEvent::Kind;

	constexpr uint64_t HOST = 76561190000000001ull;
	constexpr uint64_t ANN = 76561190000000002ull;
	constexpr uint64_t STRANGER = 76561190000000009ull;

	SteamNetworkingIdentity Identity(uint64_t steamId)
	{
		SteamNetworkingIdentity identity;
		identity.Clear();
		identity.SetSteamID64(steamId);
		return identity;
	}

	struct FakeMessage : SteamNetworkingMessage_t
	{
		Bytes Data;
	};

	class FakeMessages final : public ISteamNetworkingMessages
	{
	public:
		EResult SendMessageToUser(const SteamNetworkingIdentity& to, const void* data, uint32 size, int, int channel) override
		{
			Calls++;
			const auto* bytes = static_cast<const uint8_t*>(data);
			Sent.push_back({ to.GetSteamID64(), Bytes(bytes, bytes + size) });
			return channel == 0 ? k_EResultOK : k_EResultFail;
		}

		int ReceiveMessagesOnChannel(int channel, SteamNetworkingMessage_t** out, int max) override
		{
			Calls++;
			int count = 0;
			while (channel == 0 && count < max && !Inbox.empty())
			{
				auto* m = new FakeMessage();
				m->Data = std::move(Inbox.front().second);
				m->m_pData = m->Data.data();
				m->m_cbSize = static_cast<int>(m->Data.size());
				m->m_identityPeer = Identity(Inbox.front().first);
				m->m_pfnRelease = [](SteamNetworkingMessage_t* message) { delete static_cast<FakeMessage*>(message); };
				Inbox.pop_front();
				out[count++] = m;
			}
			return count;
		}

		bool AcceptSessionWithUser(const SteamNetworkingIdentity& who) override
		{
			Calls++;
			Accepted.push_back(who.GetSteamID64());
			return true;
		}

		bool CloseSessionWithUser(const SteamNetworkingIdentity& who) override
		{
			Calls++;
			Closed.push_back(who.GetSteamID64());
			return true;
		}

		bool CloseChannelWithUser(const SteamNetworkingIdentity&, int) override
		{
			Calls++;
			return true;
		}

		ESteamNetworkingConnectionState GetSessionConnectionInfo(const SteamNetworkingIdentity&, SteamNetConnectionInfo_t*, SteamNetConnectionRealTimeStatus_t*) override
		{
			Calls++;
			return k_ESteamNetworkingConnectionState_None;
		}

		int Calls = 0;
		std::deque<std::pair<uint64_t, Bytes>> Inbox;
		std::vector<std::pair<uint64_t, Bytes>> Sent;
		std::vector<uint64_t> Accepted;
		std::vector<uint64_t> Closed;
	};

	struct Polled
	{
		std::vector<Packet> Packets;
		std::vector<PeerEvent> Events;

		void From(Transport& transport)
		{
			Packets.clear();
			Events.clear();
			transport.Poll(Packets, Events);
		}
	};

	bool AnnAllowed(uint64_t user)
	{
		return user == ANN;
	}

	void RequestSession(uint64_t from)
	{
		SteamNetworkingMessagesSessionRequest_t request;
		request.m_identityRemote = Identity(from);
		CCallbackMgr::Dispatch(request);
	}

	void FailSession(uint64_t who, int reason)
	{
		SteamNetworkingMessagesSessionFailed_t failed;
		std::memset(&failed.m_info, 0, sizeof(failed.m_info));
		failed.m_info.m_identityRemote = Identity(who);
		failed.m_info.m_eEndReason = reason;
		std::memcpy(failed.m_info.m_szEndDebug, "timed out", sizeof("timed out"));
		CCallbackMgr::Dispatch(failed);
	}

	Bytes Hello(std::string_view name)
	{
		ByteWriter w;
		w.U8(0xF0);
		w.U16(k_SessionProtocol);
		w.U16(7);
		w.U32(0);
		w.Str(name);
		w.Str("steam");
		return w.Data();
	}

}

TEST(SteamTransportDrainsStaleMessagesOnCreate)
{
	FakeMessages steam;
	steam.Inbox.push_back({ ANN, { 1 } });
	steam.Inbox.push_back({ ANN, { 2 } });
	std::vector<std::string> log;
	auto t = SteamTransport::Create(&steam, 0, &AnnAllowed, [&log](LogLevel, std::string_view m) { log.emplace_back(m); });
	CHECK(t != nullptr && steam.Inbox.empty());
	CHECK(log.size() == 1 && log[0].find("2 stale") != std::string::npos);
	CHECK(SteamTransport::Create(nullptr, 0, &AnnAllowed) == nullptr);
	CHECK(SteamTransport::Create(&steam, 0, &AnnAllowed, {}, [] { return false; }) == nullptr);
}

TEST(SteamTransportHostTakesOnlyAllowedUsers)
{
	FakeMessages steam;
	auto t = SteamTransport::Create(&steam, 0, &AnnAllowed);
	CHECK(t != nullptr);
	if (!t)
	{
		return;
	}
	RequestSession(STRANGER);
	RequestSession(ANN);
	CHECK(steam.Accepted == std::vector<uint64_t>({ ANN }));

	steam.Inbox.push_back({ STRANGER, { 9 } });
	steam.Inbox.push_back({ ANN, { 1 } });
	steam.Inbox.push_back({ ANN, { 2 } });
	Polled p;
	p.From(*t);
	CHECK(p.Events.size() == 1 && p.Events[0].Type == Kind::Connected && p.Events[0].Peer == ANN);
	CHECK(p.Packets.size() == 2 && p.Packets[0].From == ANN && p.Packets[0].Data == Bytes({ 1 }) && p.Packets[1].Data == Bytes({ 2 }));
	CHECK(t->BytesIn == 2);

	FailSession(ANN, k_ESteamNetConnectionEnd_Misc_Timeout);
	p.From(*t);
	CHECK(p.Events.size() == 1 && p.Events[0].Type == Kind::Disconnected && p.Events[0].Peer == ANN
		&& p.Events[0].Reason == DisconnectReason::TimedOut);
}

TEST(SteamTransportClientTalksOnlyToTheHost)
{
	FakeMessages steam;
	std::vector<std::string> log;
	auto t = SteamTransport::Create(&steam, HOST, {}, [&log](LogLevel, std::string_view m) { log.emplace_back(m); });
	CHECK(t != nullptr);
	if (!t)
	{
		return;
	}
	steam.Inbox.push_back({ ANN, { 9 } });
	steam.Inbox.push_back({ HOST, { 1 } });
	Polled p;
	p.From(*t);
	CHECK(p.Events.size() == 1 && p.Events[0].Type == Kind::Connected && p.Events[0].Peer == HOST);
	CHECK(p.Packets.size() == 1 && p.Packets[0].From == HOST);

	t->Send(HOST, Bytes({ 5, 6 }), Delivery::Reliable);
	t->Send(HOST, Bytes(MAX_UNRELIABLE_BYTES + 1, 0), Delivery::Unreliable);
	CHECK(steam.Sent.size() == 1 && steam.Sent[0].first == HOST && steam.Sent[0].second == Bytes({ 5, 6 }));
	CHECK(t->BytesOut == 2 && log.size() == 1);

	t->Disconnect(HOST);
	steam.Inbox.push_back({ HOST, { 2 } });
	p.From(*t);
	CHECK(p.Packets.empty() && p.Events.empty());
}

TEST(SteamTransportMakesNoSteamCallOnceSteamIsDown)
{
	FakeMessages steam;
	bool alive = true;
	{
		Session host;
		SessionParams params;
		params.Protocol = 7;
		host.SetParams(params);
		host.StartHost(SteamTransport::Create(&steam, 0, &AnnAllowed, {}, [&alive] { return alive; }), "Host");
		steam.Inbox.push_back({ ANN, Hello("Ann") });
		host.Update();
		CHECK(host.SeatedCount() == 2);
		CHECK(!steam.Sent.empty());

		alive = false;
		steam.Calls = 0;
		steam.Inbox.push_back({ ANN, { 0x10 } });
		host.Update();
		host.Send(1, Bytes({ 0x10 }), Delivery::Reliable);
		host.Stop();
		CHECK(steam.Calls == 0);
		CHECK(!host.Active());
	}
	CHECK(steam.Calls == 0);
	CHECK(CCallbackMgr::Count() == 0);
}

TEST(SteamTransportIsInertAfterClose)
{
	FakeMessages steam;
	auto t = SteamTransport::Create(&steam, 0, &AnnAllowed);
	CHECK(t != nullptr);
	if (!t)
	{
		return;
	}
	steam.Inbox.push_back({ ANN, { 1 } });
	Polled p;
	p.From(*t);
	CHECK(p.Events.size() == 1);

	FailSession(ANN, k_ESteamNetConnectionEnd_Misc_Timeout);
	t->Close();
	CHECK(steam.Closed.empty());

	steam.Calls = 0;
	RequestSession(ANN);
	steam.Inbox.push_back({ ANN, { 2 } });
	p.From(*t);
	t->Send(ANN, Bytes({ 3 }), Delivery::Reliable);
	t->Disconnect(ANN);
	t->Close();
	CHECK(p.Packets.empty() && p.Events.empty());
	CHECK(steam.Calls == 0 && steam.Accepted.empty());

	auto u = SteamTransport::Create(&steam, 0, &AnnAllowed);
	CHECK(u != nullptr);
	if (!u)
	{
		return;
	}
	steam.Inbox.push_back({ ANN, { 4 } });
	p.From(*u);
	u->Close();
	CHECK(steam.Closed == std::vector<uint64_t>({ ANN }));
}

TEST(SteamTransportLetsANewSessionsHelloThroughTheIgnoreWindow)
{
	FakeMessages steam;
	auto t = SteamTransport::Create(&steam, 0, &AnnAllowed);
	CHECK(t != nullptr);
	if (!t)
	{
		return;
	}
	steam.Inbox.push_back({ ANN, { 1 } });
	Polled p;
	p.From(*t);
	t->Disconnect(ANN);

	steam.Inbox.push_back({ ANN, { 2 } });
	p.From(*t);
	CHECK(p.Packets.empty() && p.Events.empty());

	const Bytes hello = Hello("Ann");
	steam.Inbox.push_back({ ANN, hello });
	steam.Inbox.push_back({ ANN, { 3 } });
	p.From(*t);
	CHECK(p.Events.size() == 1 && p.Events[0].Type == Kind::Connected && p.Events[0].Peer == ANN);
	CHECK(p.Packets.size() == 2 && p.Packets[0].Data == hello && p.Packets[1].Data == Bytes({ 3 }));
}
