#pragma once

#include <DingoNet/Log.h>
#include <DingoNet/Transport.h>

#include <functional>
#include <memory>
#include <string_view>

class ISteamNetworkingMessages;

namespace Dingo::Net
{

	// Peer-to-peer over Steam (ISteamNetworkingMessages): NAT punching and Valve's relay come free,
	// and players never see each other's IP. Peers are SteamIDs. Only users `Allowed` says yes to
	// (the Steam lobby's members) may open a session with a host. Compiled by the game against its
	// own Steamworks SDK, after SteamAPI_Init().
	class SteamTransport : public Transport
	{
	public:
		using Allowed = std::function<bool(uint64_t steamId)>;
		using Log = std::function<void(LogLevel level, std::string_view message)>;

		// Host (host = 0): waits for allowed users to reach out. Client: talks to `host`, the lobby
		// owner. Returns null when `messages` is null. Destroy (or Close) the transport before
		// SteamAPI_Shutdown(), or pass `isAlive`: Close makes no Steam call while it returns false.
		static std::unique_ptr<SteamTransport> Create(ISteamNetworkingMessages* messages, uint64_t host, Allowed allowed, Log log = {}, std::function<bool()> isAlive = {});
		~SteamTransport() override;

		void Send(PeerId to, std::span<const uint8_t> data, Delivery delivery) override;
		void Poll(std::vector<Packet>& packets, std::vector<PeerEvent>& events) override;
		void Disconnect(PeerId peer) override;
		void Close() override;
		std::string Describe() const override;

	private:
		SteamTransport() = default;
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};

}
