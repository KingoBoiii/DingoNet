#include "SteamTransport.h"

#include <chrono>
#include <format>
#include <unordered_map>
#include <unordered_set>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
#include <steam/steam_api.h>
#include <steam/isteamnetworkingmessages.h>
#include <steam/isteamnetworkingutils.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace Dingo::Net
{

	namespace
	{
		constexpr int CHANNEL = 0;

		SteamNetworkingIdentity Identity(uint64_t steamId)
		{
			SteamNetworkingIdentity identity;
			identity.SetSteamID64(steamId);
			return identity;
		}

		DisconnectReason MapEndReason(int reason)
		{
			switch (reason)
			{
			case k_ESteamNetConnectionEnd_Remote_Timeout:
			case k_ESteamNetConnectionEnd_Misc_Timeout:
				return DisconnectReason::TimedOut;
			case k_ESteamNetConnectionEnd_Misc_PeerSentNoConnection:
				return DisconnectReason::Reset;
			case k_ESteamNetConnectionEnd_Misc_InternalError:
				return DisconnectReason::Error;
			case k_ESteamNetConnectionEnd_Misc_SteamConnectivity:
			case k_ESteamNetConnectionEnd_Misc_NoRelaySessionsToClient:
			case k_ESteamNetConnectionEnd_Misc_P2P_Rendezvous:
			case k_ESteamNetConnectionEnd_Misc_P2P_NAT_Firewall:
				return DisconnectReason::ConnectFailed;
			default:
				break;
			}
			if (reason >= k_ESteamNetConnectionEnd_App_Min && reason <= k_ESteamNetConnectionEnd_App_Max)
				return DisconnectReason::Closed;
			if ((reason >= k_ESteamNetConnectionEnd_Local_Min && reason <= k_ESteamNetConnectionEnd_Local_Max)
			 || (reason >= k_ESteamNetConnectionEnd_Remote_Min && reason <= k_ESteamNetConnectionEnd_Remote_Max))
				return DisconnectReason::ConnectFailed;
			return DisconnectReason::Reset;
		}
	}

	struct SteamTransport::Impl
	{
		ISteamNetworkingMessages* Messages = nullptr;
		uint64_t Host = 0;
		Allowed Allow;
		Log Logger;
		std::function<bool()> IsAlive;
		std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> Recent;
		std::unordered_set<uint64_t> Peers;
		std::vector<PeerEvent> Pending;
		bool Announced = false;
		bool Closed = false;

		bool Alive() const
		{
			return !IsAlive || IsAlive();
		}

		bool Usable() const
		{
			return !Closed && Alive();
		}

		void Say(LogLevel level, const std::string& message) const
		{
			if (Logger) Logger(level, message);
		}

		STEAM_CALLBACK(Impl, OnSessionRequest, SteamNetworkingMessagesSessionRequest_t);
		STEAM_CALLBACK(Impl, OnSessionFailed, SteamNetworkingMessagesSessionFailed_t);
	};

	void SteamTransport::Impl::OnSessionRequest(SteamNetworkingMessagesSessionRequest_t* request)
	{
		if (Closed) return;
		const uint64_t from = request->m_identityRemote.GetSteamID64();
		if (Host != 0 ? from == Host : (Allow && Allow(from)))
			Messages->AcceptSessionWithUser(request->m_identityRemote);
		else
			Say(LogLevel::Warn, std::format("refused a Steam session from {} (not in the lobby)", from));
	}

	void SteamTransport::Impl::OnSessionFailed(SteamNetworkingMessagesSessionFailed_t* failed)
	{
		const uint64_t who = failed->m_info.m_identityRemote.GetSteamID64();
		if (!Peers.erase(who)) return;
		PeerEvent event;
		event.Type = PeerEvent::Kind::Disconnected;
		event.Peer = who;
		event.Reason = MapEndReason(failed->m_info.m_eEndReason);
		event.Detail = std::format("STEAM {}: {}", static_cast<int>(failed->m_info.m_eEndReason), failed->m_info.m_szEndDebug);
		Pending.push_back(std::move(event));
	}

	std::unique_ptr<SteamTransport> SteamTransport::Create(ISteamNetworkingMessages* messages, uint64_t host, Allowed allowed, Log log, std::function<bool()> isAlive)
	{
		if (!messages || (isAlive && !isAlive())) return nullptr;

		// Steam sessions outlive a transport, so the shared queue may still hold the last game's messages.
		SteamNetworkingMessage_t* stale[64];
		int dropped = 0;
		for (int count; (count = messages->ReceiveMessagesOnChannel(CHANNEL, stale, 64)) > 0;)
		{
			for (int i = 0; i < count; ++i) stale[i]->Release();
			dropped += count;
		}
		if (dropped > 0 && log) log(LogLevel::Warn, std::format("dropped {} stale Steam messages", dropped));

		std::unique_ptr<SteamTransport> t(new SteamTransport());
		t->m_Impl = std::make_unique<Impl>();
		t->m_Impl->Messages = messages;
		t->m_Impl->Host = host;
		t->m_Impl->Allow = std::move(allowed);
		t->m_Impl->Logger = std::move(log);
		t->m_Impl->IsAlive = std::move(isAlive);
		if (host != 0) t->m_Impl->Peers.insert(host);
		return t;
	}

	SteamTransport::~SteamTransport()
	{
		Close();
	}

	void SteamTransport::Send(PeerId to, std::span<const uint8_t> data, Delivery delivery)
	{
		if (!m_Impl || !m_Impl->Usable()) return;
		if (delivery == Delivery::Unreliable && data.size() > MAX_UNRELIABLE_BYTES)
		{
			m_Impl->Say(LogLevel::Error, std::format("dropped an unreliable packet of {} bytes (max {})", data.size(), MAX_UNRELIABLE_BYTES));
			return;
		}
		if (delivery == Delivery::Reliable && data.size() > static_cast<size_t>(k_cbMaxSteamNetworkingSocketsMessageSizeSend))
		{
			m_Impl->Say(LogLevel::Error, std::format("dropped a reliable packet of {} bytes (max {})", data.size(), k_cbMaxSteamNetworkingSocketsMessageSizeSend));
			return;
		}
		const int flags = (delivery == Delivery::Reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_UnreliableNoNagle)
		                | k_nSteamNetworkingSend_AutoRestartBrokenSession;
		const EResult result = m_Impl->Messages->SendMessageToUser(Identity(to), data.data(), static_cast<uint32>(data.size()), flags, CHANNEL);
		if (result != k_EResultOK)
		{
			m_Impl->Say(LogLevel::Error, std::format("Steam send of {} bytes to {} failed (EResult {})", data.size(), to, static_cast<int>(result)));
			return;
		}
		BytesOut += data.size();
	}

	void SteamTransport::Poll(std::vector<Packet>& packets, std::vector<PeerEvent>& events)
	{
		if (!m_Impl || !m_Impl->Usable()) return;
		Impl& impl = *m_Impl;

		if (impl.Host != 0 && !impl.Announced)
		{
			impl.Announced = true;
			PeerEvent event;
			event.Type = PeerEvent::Kind::Connected;
			event.Peer = impl.Host;
			events.push_back(std::move(event));
		}
		for (PeerEvent& e : impl.Pending) events.push_back(std::move(e));
		impl.Pending.clear();

		const auto now = std::chrono::steady_clock::now();
		for (auto it = impl.Recent.begin(); it != impl.Recent.end();)
			it = it->second <= now ? impl.Recent.erase(it) : std::next(it);

		SteamNetworkingMessage_t* messages[64];
		while (true)
		{
			const int count = impl.Messages->ReceiveMessagesOnChannel(CHANNEL, messages, 64);
			for (int i = 0; i < count; ++i)
			{
				SteamNetworkingMessage_t* m = messages[i];
				const uint64_t from = m->m_identityPeer.GetSteamID64();
				const bool known = impl.Peers.contains(from);
				const bool welcome = !impl.Recent.contains(from) && (impl.Host != 0 ? from == impl.Host : (known || (impl.Allow && impl.Allow(from))));
				if (welcome)
				{
					if (!known)
					{
						impl.Peers.insert(from);
						PeerEvent event;
						event.Type = PeerEvent::Kind::Connected;
						event.Peer = from;
						events.push_back(std::move(event));
					}
					const uint8_t* bytes = static_cast<const uint8_t*>(m->m_pData);
					packets.push_back({ from, std::vector<uint8_t>(bytes, bytes + m->m_cbSize) });
					BytesIn += static_cast<uint64_t>(m->m_cbSize);
				}
				m->Release();
			}
			if (count < 64) break;
		}
	}

	void SteamTransport::Disconnect(PeerId peer)
	{
		// The Steam session is left to idle out: closing it now could drop a parting Bye still on its way.
		if (!m_Impl || m_Impl->Closed) return;
		m_Impl->Peers.erase(peer);
		m_Impl->Recent[peer] = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	}

	void SteamTransport::Close()
	{
		if (!m_Impl || m_Impl->Closed) return;
		m_Impl->Closed = true;
		if (m_Impl->Alive())
			for (uint64_t peer : m_Impl->Peers)
				m_Impl->Messages->CloseSessionWithUser(Identity(peer));
		m_Impl->Peers.clear();
		m_Impl->Recent.clear();
		m_Impl->Pending.clear();
	}

	std::string SteamTransport::Describe() const
	{
		if (!m_Impl) return "steam (off)";
		return m_Impl->Host != 0 ? std::format("steam p2p -> {}", m_Impl->Host) : std::string("steam p2p host");
	}

}
