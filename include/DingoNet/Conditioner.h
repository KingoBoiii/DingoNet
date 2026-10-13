#pragma once

#include "DingoNet/Transport.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace Dingo::Net
{

	struct ConditionerParams
	{
		double LagSeconds = 0.0;
		// Each packet waits an extra uniform 0..JitterSeconds; reliable ones never overtake each other.
		double JitterSeconds = 0.0;
		// The share of unreliable packets dropped; reliable ones always arrive.
		float Loss = 0.0f;
		uint32_t Seed = 99;
		// Null reads std::chrono::steady_clock; a test or a game's own net clock can drive it instead.
		std::function<std::chrono::steady_clock::time_point()> Clock;
	};

	// Fake network weather on everything sent through the wrapped transport. Its random stream is its
	// own, so it never shifts a game's randomness.
	class Conditioner final : public Transport
	{
	public:
		explicit Conditioner(std::unique_ptr<Transport> inner, ConditionerParams params = {});
		~Conditioner() override;

		Conditioner(const Conditioner&) = delete;
		Conditioner& operator=(const Conditioner&) = delete;

		void Configure(double lagSeconds, float loss, double jitterSeconds = 0.0);
		bool Active() const;

		void Send(PeerId to, std::span<const uint8_t> data, Delivery delivery) override;
		void Poll(std::vector<Packet>& packets, std::vector<PeerEvent>& events) override;
		// Packets held for the peer leave at once, then the inner transport hangs up.
		void Disconnect(PeerId peer) override;
		void Close() override;
		std::string Describe() const override;

		Transport& GetInner() { return *m_Inner; }
		size_t GetQueuedCount() const { return m_Queue.size(); }

		uint64_t Dropped = 0;

	private:
		struct Held
		{
			PeerId To = 0;
			Delivery Mode = Delivery::Reliable;
			std::vector<uint8_t> Data;
		};

		using TimePoint = std::chrono::steady_clock::time_point;
		// Keyed by release time, then send order, so equal times keep the order they were sent in.
		using Queue = std::map<std::pair<TimePoint, uint64_t>, Held>;

		TimePoint Now() const;
		Queue::iterator Remove(Queue::iterator it);
		Queue::iterator Release(Queue::iterator it);
		void SyncCounters();

	private:
		std::unique_ptr<Transport> m_Inner;
		ConditionerParams m_Params;
		std::mt19937 m_Rng;
		Queue m_Queue;
		uint64_t m_NextSerial = 0;
		TimePoint m_LastReliableRelease{};
		size_t m_HeldReliable = 0;
	};

}
