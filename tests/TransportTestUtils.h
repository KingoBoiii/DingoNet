#pragma once

#include <DingoNet/Log.h>
#include <DingoNet/Transport.h>

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace TransportTest
{

	struct Endpoint
	{
		Endpoint() = default;
		explicit Endpoint(Dingo::Net::Transport* link) : Link(link) {}

		Dingo::Net::Transport* Link = nullptr;
		std::vector<Dingo::Net::Packet> Packets;
		std::vector<Dingo::Net::PeerEvent> Events;

		void Poll()
		{
			Link->Poll(Packets, Events);
		}

		const Dingo::Net::PeerEvent* Find(Dingo::Net::PeerEvent::Kind kind) const
		{
			for (const Dingo::Net::PeerEvent& e : Events)
			{
				if (e.Type == kind)
				{
					return &e;
				}
			}
			return nullptr;
		}

		bool Has(Dingo::Net::PeerEvent::Kind kind) const
		{
			return Find(kind) != nullptr;
		}
	};

	// Polls every endpoint until done() holds or the time is up; never spins forever.
	template<typename Done>
	bool PumpUntil(std::initializer_list<Endpoint*> endpoints, Done done, std::chrono::milliseconds limit = std::chrono::milliseconds(3000))
	{
		const auto deadline = std::chrono::steady_clock::now() + limit;
		while (true)
		{
			for (Endpoint* e : endpoints)
			{
				e->Poll();
			}
			if (done())
			{
				return true;
			}
			if (std::chrono::steady_clock::now() > deadline)
			{
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	inline std::vector<uint8_t> Pattern(size_t size, uint8_t seed)
	{
		std::vector<uint8_t> data(size);
		for (size_t i = 0; i < size; i++)
		{
			data[i] = static_cast<uint8_t>(seed + i * 31 + (i >> 8));
		}
		return data;
	}

	inline std::vector<uint8_t> Text(std::string_view text)
	{
		return std::vector<uint8_t>(text.begin(), text.end());
	}

	// Counts error lines while alive; the default handler comes back when it goes.
	class LogCapture
	{
	public:
		LogCapture()
		{
			s_Errors = 0;
			s_Warnings = 0;
			s_Last.clear();
			Dingo::Net::SetLogHandler(&Handle);
		}

		~LogCapture()
		{
			Dingo::Net::SetLogHandler(nullptr);
		}

		LogCapture(const LogCapture&) = delete;
		LogCapture& operator=(const LogCapture&) = delete;

		int Errors() const { return s_Errors; }
		int Warnings() const { return s_Warnings; }
		const std::string& Last() const { return s_Last; }

	private:
		static void Handle(Dingo::Net::LogLevel level, std::string_view message)
		{
			if (level == Dingo::Net::LogLevel::Error)
			{
				s_Errors++;
				s_Last = std::string(message);
			}
			else if (level == Dingo::Net::LogLevel::Warn)
			{
				s_Warnings++;
			}
		}

		static inline int s_Errors = 0;
		static inline int s_Warnings = 0;
		static inline std::string s_Last;
	};

}
