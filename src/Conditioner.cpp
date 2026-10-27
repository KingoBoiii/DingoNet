#include "DingoNet/Conditioner.h"

#include "Internal/SendLimits.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace Dingo::Net
{

	namespace
	{

		double ClampSeconds(double seconds)
		{
			return std::isnan(seconds) ? 0.0 : std::clamp(seconds, 0.0, 1e6);
		}

		std::chrono::steady_clock::duration Seconds(double seconds)
		{
			// Casting a huge or infinite double to the clock's integer ticks is undefined.
			return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(ClampSeconds(seconds)));
		}

	}

	Conditioner::Conditioner(std::unique_ptr<Transport> inner, ConditionerParams params)
		: m_Inner(std::move(inner)), m_Params(std::move(params)), m_Rng(m_Params.Seed)
	{
	}

	Conditioner::~Conditioner()
	{
		Close();
	}

	void Conditioner::Configure(double lagSeconds, float loss, double jitterSeconds)
	{
		m_Params.LagSeconds = lagSeconds;
		m_Params.Loss = loss;
		m_Params.JitterSeconds = jitterSeconds;
	}

	bool Conditioner::Active() const
	{
		return m_Params.LagSeconds > 0.0 || m_Params.JitterSeconds > 0.0 || m_Params.Loss > 0.0f;
	}

	Conditioner::TimePoint Conditioner::Now() const
	{
		return m_Params.Clock ? m_Params.Clock() : std::chrono::steady_clock::now();
	}

	std::string Conditioner::Describe() const
	{
		return std::format("{} (conditioned: {:.0f} ms lag, {:.0f} ms jitter, {:.1f}% loss)", m_Inner->Describe(),
			m_Params.LagSeconds * 1000.0, m_Params.JitterSeconds * 1000.0, m_Params.Loss * 100.0f);
	}

	void Conditioner::SyncCounters()
	{
		BytesIn = m_Inner->BytesIn;
		BytesOut = m_Inner->BytesOut;
	}

	Conditioner::Queue::iterator Conditioner::Remove(Queue::iterator it)
	{
		if (it->second.Mode == Delivery::Reliable)
		{
			m_HeldReliable--;
		}
		return m_Queue.erase(it);
	}

	Conditioner::Queue::iterator Conditioner::Release(Queue::iterator it)
	{
		const Held& held = it->second;
		if (held.Mode == Delivery::UnreliableSequenced)
		{
			uint64_t& newest = m_SequencedReleased[held.To];
			if (held.Sequence < newest)
			{
				Dropped++;
				return Remove(it);
			}
			newest = held.Sequence;
		}
		m_Inner->Send(held.To, held.Data, held.Mode);
		return Remove(it);
	}

	void Conditioner::Send(PeerId to, std::span<const uint8_t> data, Delivery delivery)
	{
		if (!Internal::CheckUnreliableSize(data.size(), delivery, "conditioner"))
		{
			return;
		}
		if (!Active() && m_Queue.empty())
		{
			m_Inner->Send(to, data, delivery);
			SyncCounters();
			return;
		}

		if (IsUnreliable(delivery) && m_Params.Loss > 0.0f && std::uniform_real_distribution<float>(0.0f, 1.0f)(m_Rng) < m_Params.Loss)
		{
			Dropped++;
			return;
		}

		double delay = ClampSeconds(m_Params.LagSeconds);
		const double jitter = ClampSeconds(m_Params.JitterSeconds);
		if (jitter > 0.0)
		{
			delay += std::uniform_real_distribution<double>(0.0, jitter)(m_Rng);
		}
		TimePoint release = Now() + Seconds(delay);
		if (delivery == Delivery::Reliable)
		{
			// Only against one still held: a clock that jumps back must not hold new packets behind an old time.
			if (m_HeldReliable > 0)
			{
				release = (std::max)(release, m_LastReliableRelease);
			}
			m_LastReliableRelease = release;
			m_HeldReliable++;
		}
		const uint64_t sequence = delivery == Delivery::UnreliableSequenced ? ++m_SequencedSent[to] : 0;
		m_Queue.emplace(std::make_pair(release, m_NextSerial++), Held{ to, delivery, sequence, std::vector<uint8_t>(data.begin(), data.end()) });
	}

	void Conditioner::Poll(std::vector<Packet>& packets, std::vector<PeerEvent>& events)
	{
		const TimePoint now = Now();
		while (!m_Queue.empty() && m_Queue.begin()->first.first <= now)
		{
			Release(m_Queue.begin());
		}

		const size_t firstEvent = events.size();
		m_Inner->Poll(packets, events);
		SyncCounters();

		// A peer id can come back (a SteamID does), so nothing held for a lost peer may reach its next connection.
		for (size_t i = firstEvent; i < events.size(); i++)
		{
			if (events[i].Type != PeerEvent::Kind::Disconnected)
			{
				continue;
			}
			for (auto it = m_Queue.begin(); it != m_Queue.end();)
			{
				it = it->second.To == events[i].Peer ? Remove(it) : std::next(it);
			}
			m_SequencedSent.erase(events[i].Peer);
			m_SequencedReleased.erase(events[i].Peer);
		}
	}

	void Conditioner::Disconnect(PeerId peer)
	{
		for (auto it = m_Queue.begin(); it != m_Queue.end();)
		{
			it = it->second.To == peer ? Release(it) : std::next(it);
		}
		m_SequencedSent.erase(peer);
		m_SequencedReleased.erase(peer);
		m_Inner->Disconnect(peer);
		SyncCounters();
	}

	void Conditioner::Close()
	{
		if (!m_Inner)
		{
			return;
		}
		while (!m_Queue.empty())
		{
			Release(m_Queue.begin());
		}
		m_Inner->Close();
		SyncCounters();
	}

}
