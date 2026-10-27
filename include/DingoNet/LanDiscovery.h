#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Dingo::Net
{

	inline constexpr uint16_t LAN_DEFAULT_PORT = 27961;
	inline constexpr size_t LAN_MAX_GAME_DATA = 256;
	inline constexpr size_t LAN_MAX_HOSTS = 64;

	struct LanHostParams
	{
		// The discovery port browsers query. A second host on one machine needs its own.
		uint16_t Port = LAN_DEFAULT_PORT;
		// "0.0.0.0" listens on every adapter; a test binds "127.0.0.1".
		std::string BindAddress = "0.0.0.0";
		uint16_t Protocol = 0;
		uint32_t ContentHash = 0;
		// Where the game's own transport listens: what a joiner connects to.
		uint16_t GamePort = 27960;
		std::string Name;
		// Applied to the host's name, and to every name a browser takes in. Null: DefaultNameFilter.
		std::function<std::string(std::string_view)> NameFilter;
	};

	struct LanHostStatus
	{
		int SeatsTaken = 1;
		int SeatsMax = 4;
		bool Accepting = true;
		// A map name, a mode: at most LAN_MAX_GAME_DATA bytes, cut beyond that.
		std::vector<uint8_t> GameData;
	};

	// Answers LanBrowser queries by unicast. Pumped by Poll() once a frame: no threads.
	class LanHost
	{
	public:
		static std::unique_ptr<LanHost> Open(const LanHostParams& params, std::string& error);
		~LanHost();
		LanHost(const LanHost&) = delete;
		LanHost& operator=(const LanHost&) = delete;

		void SetStatus(const LanHostStatus& status);
		// Answers at most 32 queries; the rest of what has arrived is dropped (Throttled).
		void Poll();

		uint16_t GetPort() const { return m_Port; }
		uint64_t Answered() const { return m_Answered; }
		uint64_t Malformed() const { return m_Malformed; }
		uint64_t Throttled() const { return m_Throttled; }

	private:
		LanHost() = default;

		uint64_t m_Socket = ~uint64_t(0);
		uint16_t m_Port = 0;
		LanHostParams m_Params;
		LanHostStatus m_Status;
		uint64_t m_Answered = 0;
		uint64_t m_Malformed = 0;
		uint64_t m_Throttled = 0;
	};

	struct LanBrowserParams
	{
		// The port hosts listen on.
		uint16_t Port = LAN_DEFAULT_PORT;
		// "255.255.255.255" asks every host on the LAN; a test names one host's address.
		std::string QueryAddress = "255.255.255.255";
		uint16_t Protocol = 0;
		uint32_t ContentHash = 0;
		double QueryInterval = 1.0;
		// A host that has not answered for this long drops off the list.
		double HostTimeout = 3.0;
		std::function<std::string(std::string_view)> NameFilter;
		// Null reads std::chrono::steady_clock.
		std::function<std::chrono::steady_clock::time_point()> Clock;
	};

	struct LanHostInfo
	{
		// The host's IP as text; with GamePort it is what a joiner connects to.
		std::string Address;
		uint16_t GamePort = 0;
		// The answering socket, "address:port": tells two hosts on one address apart.
		std::string Source;
		std::string Name;
		uint16_t Protocol = 0;
		uint32_t ContentHash = 0;
		int SeatsTaken = 0;
		int SeatsMax = 0;
		bool Accepting = false;
		std::vector<uint8_t> GameData;
		// Whether this host runs the browser's own Protocol and ContentHash: a game greys out the rest.
		bool ProtocolMatches = false;
		bool ContentMatches = false;
	};

	// Broadcasts a query once a second while open and lists who answers. Pumped by Poll() once a frame.
	class LanBrowser
	{
	public:
		static std::unique_ptr<LanBrowser> Open(const LanBrowserParams& params, std::string& error);
		~LanBrowser();
		LanBrowser(const LanBrowser&) = delete;
		LanBrowser& operator=(const LanBrowser&) = delete;

		void Poll();

		// In the order each host was first seen.
		const std::vector<LanHostInfo>& Hosts() const { return m_Hosts; }
		// The ephemeral port replies come back to.
		uint16_t GetPort() const { return m_Port; }
		uint64_t QueriesSent() const { return m_QueriesSent; }
		uint64_t Malformed() const { return m_Malformed; }

	private:
		LanBrowser() = default;

		double Now() const;

		uint64_t m_Socket = ~uint64_t(0);
		uint16_t m_Port = 0;
		LanBrowserParams m_Params;
		std::chrono::steady_clock::time_point m_Start;
		double m_NextQuery = 0.0;
		std::vector<LanHostInfo> m_Hosts;
		std::vector<double> m_LastSeen;
		uint64_t m_QueriesSent = 0;
		bool m_SendFailureLogged = false;
		uint64_t m_Malformed = 0;
	};

}
