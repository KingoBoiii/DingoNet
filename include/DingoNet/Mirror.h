#pragma once

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <functional>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Dingo::Net
{

	inline double Interpolate(double a, double b, double t)
	{
		return a + (b - a) * t;
	}

	inline double Extrapolate(double a, double b, double t)
	{
		return a + (b - a) * t;
	}

	template<typename T>
	concept Interpolable = requires(const T& a, const T& b, double t)
	{
		{ Interpolate(a, b, t) } -> std::convertible_to<T>;
	};

	template<typename T>
	concept Extrapolable = requires(const T& a, const T& b, double t)
	{
		{ Extrapolate(a, b, t) } -> std::convertible_to<T>;
	};

	struct RenderClockParams
	{
		double Delay = 0.1;
		double MaxSlew = 0.1;
		double CatchUp = 0.25;
		double SnapError = 0.5;
		double MaxDt = 0.25;
		double RewindError = 2.0;
		double RestartError = 2.0;
	};

	// Samples, steps and events carry no era: the game clears its Tracks, Timelines and EventQueues
	// at an epoch change or run start, and calls RenderClock::Reset() there (a stamp that jumps back
	// by more than RestartError restarts the clock by itself, see Restarts()). Render time goes back only
	// then, or when it is ahead of its target by more than RewindError; a smaller overshoot (network delay
	// whose backlog is still arriving) holds it until the target catches up. A hold begins beyond the slew
	// band, MaxSlew * CatchUp (0.025 s) ahead of the target. HostNow() can resync down to a newer stamp more than
	// SnapError behind it, so RenderTime() can sit ahead of HostNow() while held. Keep RewindError at most
	// TracksParams::Stale.
	class RenderClock
	{
	public:
		explicit RenderClock(RenderClockParams params = {})
			: m_Params(params)
		{
		}

		bool HasTime() const { return m_Started; }
		bool Settled() const { return m_Settled; }
		const RenderClockParams& Params() const { return m_Params; }

		void OnSnapshot(double hostTime)
		{
			if (!std::isfinite(hostTime))
			{
				return;
			}
			if (!m_Started)
			{
				m_Started = true;
				m_Latest = hostTime;
				m_Estimate = hostTime;
				m_Since = 0.0;
				m_Render = hostTime - m_Params.Delay;
				return;
			}
			if (hostTime < m_Latest - m_Params.RestartError)
			{
				m_Latest = hostTime;
				m_Estimate = hostTime;
				m_Since = 0.0;
				m_Render = hostTime - m_Params.Delay;
				m_Settled = false;
				++m_Restarts;
				return;
			}
			if (hostTime > m_Latest)
			{
				m_Latest = hostTime;
				m_Since = 0.0;
				m_Settled = false;
				if (m_Estimate - hostTime > m_Params.SnapError)
				{
					m_Estimate = hostTime;
				}
				else
				{
					m_Estimate = (std::max)(m_Estimate, hostTime);
				}
			}
		}

		void Advance(double dt)
		{
			if (!m_Started || !(dt > 0.0) || !std::isfinite(dt))
			{
				return;
			}
			dt = (std::min)(dt, m_Params.MaxDt);

			const double catchUp = (std::max)(m_Params.CatchUp, 1e-6);
			m_Since += dt;
			if (m_Settled)
			{
				m_Estimate = m_Latest;
			}
			else
			{
				const double ahead = m_Estimate - (m_Latest + m_Since);
				const double hostSlew = ahead > 0.0 ? (std::min)(ahead / catchUp, m_Params.MaxSlew) : 0.0;
				m_Estimate = (std::max)(m_Estimate + dt * (1.0 - hostSlew), m_Latest);
			}

			const double target = m_Settled ? m_Latest : m_Estimate - m_Params.Delay;
			const double error = target - m_Render;
			if (error > m_Params.SnapError || (!m_Settled && error < -m_Params.RewindError))
			{
				m_Render = target;
				return;
			}
			if (error < -m_Params.MaxSlew * catchUp)
			{
				return;
			}
			const double slew = (std::clamp)(error / catchUp, -m_Params.MaxSlew, m_Params.MaxSlew);
			double next = m_Render + dt * (1.0 + slew);
			next = (std::min)(next, m_Settled ? m_Latest : m_Estimate);
			m_Render = (std::max)(m_Render, next);
		}

		double HostNow() const { return m_Started ? m_Estimate : 0.0; }
		double RenderTime() const { return m_Started ? m_Render : 0.0; }
		double LatestStamp() const { return m_Started ? m_Latest : 0.0; }
		int Restarts() const { return m_Restarts; }

		// Render time moves forward to the latest stamp and stops there, or holds if already past it.
		void Settle()
		{
			if (m_Started)
			{
				m_Settled = true;
			}
		}

		void Reset()
		{
			const int restarts = m_Restarts;
			*this = RenderClock(m_Params);
			m_Restarts = restarts;
		}

	private:
		RenderClockParams m_Params;
		bool m_Started = false;
		bool m_Settled = false;
		double m_Latest = 0.0;
		double m_Since = 0.0;
		double m_Estimate = 0.0;
		double m_Render = 0.0;
		int m_Restarts = 0;
	};

	struct TracksParams
	{
		double MaxExtrapolate = 0.15;
		double Stale = 2.0;
		double History = 1.0;
		double MinExtrapolateGap = 0.001;
	};

	// Interpolate and Extrapolate are found by ADL (or the double overloads above; ints truncate through
	// them), so a third-party type such as glm::vec3 needs a wrapper or functions in an associated
	// namespace. A mistyped Extrapolate silently holds: static_assert(Tracks<K, T>::Extrapolates).
	// Push grows a key's history until Trim; the game clears the Tracks at an epoch change or run start.
	template<typename Key, typename T, typename Hash = std::hash<Key>>
		requires Interpolable<T>
	class Tracks
	{
	public:
		static constexpr bool Extrapolates = Extrapolable<T>;

		explicit Tracks(TracksParams params = {})
			: m_Params(params)
		{
		}

		const TracksParams& Params() const { return m_Params; }

		void Push(const Key& key, double hostTime, const T& value)
		{
			if (!std::isfinite(hostTime))
			{
				return;
			}
			std::vector<Point>& samples = m_Tracks[key];
			const auto it = std::lower_bound(samples.begin(), samples.end(), hostTime,
				[](const Point& s, double time) { return s.Time < time; });
			if (it != samples.end() && it->Time == hostTime)
			{
				it->Value = value;
			}
			else
			{
				samples.insert(it, Point{ hostTime, value });
			}
		}

		std::optional<T> Sample(const Key& key, double renderTime) const
		{
			const auto found = m_Tracks.find(key);
			if (found == m_Tracks.end() || found->second.empty())
			{
				return std::nullopt;
			}
			const std::vector<Point>& samples = found->second;

			if (renderTime <= samples.front().Time)
			{
				return samples.front().Value;
			}

			const auto upper = std::upper_bound(samples.begin(), samples.end(), renderTime,
				[](double time, const Point& s) { return time < s.Time; });
			if (upper != samples.end())
			{
				const Point& b = *upper;
				const Point& a = *(upper - 1);
				const double t = (renderTime - a.Time) / (b.Time - a.Time);
				return static_cast<T>(Interpolate(a.Value, b.Value, t));
			}

			const Point& newest = samples.back();
			if constexpr (Extrapolable<T>)
			{
				const double ahead = (std::min)(renderTime - newest.Time, m_Params.MaxExtrapolate);
				if (ahead > 0.0)
				{
					for (std::size_t i = samples.size() - 1; i-- > 0;)
					{
						const Point& previous = samples[i];
						const double gap = newest.Time - previous.Time;
						if (gap >= m_Params.MinExtrapolateGap)
						{
							return static_cast<T>(Extrapolate(previous.Value, newest.Value, 1.0 + ahead / gap));
						}
					}
				}
			}
			return newest.Value;
		}

		void Trim(double renderTime)
		{
			for (auto it = m_Tracks.begin(); it != m_Tracks.end();)
			{
				std::vector<Point>& samples = it->second;
				if (samples.empty() || samples.back().Time < renderTime - m_Params.Stale)
				{
					it = m_Tracks.erase(it);
					continue;
				}

				const double cutoff = renderTime - m_Params.History;
				const auto firstKept = std::upper_bound(samples.begin(), samples.end(), cutoff,
					[](double time, const Point& s) { return time < s.Time; });
				if (firstKept != samples.begin())
				{
					samples.erase(samples.begin(), firstKept - 1);
				}
				++it;
			}
		}

		bool Has(const Key& key) const { return m_Tracks.find(key) != m_Tracks.end(); }
		std::size_t Count() const { return m_Tracks.size(); }

		std::size_t SampleCount(const Key& key) const
		{
			const auto found = m_Tracks.find(key);
			return found == m_Tracks.end() ? 0 : found->second.size();
		}

		std::vector<Key> Keys() const
		{
			std::vector<Key> keys;
			keys.reserve(m_Tracks.size());
			for (const auto& entry : m_Tracks)
			{
				keys.push_back(entry.first);
			}
			return keys;
		}

		template<typename F>
		void ForEach(F&& fn) const
		{
			for (const auto& entry : m_Tracks)
			{
				fn(entry.first);
			}
		}

		void Forget(const Key& key) { m_Tracks.erase(key); }
		void Clear() { m_Tracks.clear(); }

	private:
		struct Point
		{
			double Time;
			T Value;
		};

		TracksParams m_Params;
		std::unordered_map<Key, std::vector<Point>, Hash> m_Tracks;
	};

	// Set grows until Trim; clear it at an epoch change or run start.
	template<typename T>
	class Timeline
	{
	public:
		void Set(double hostTime, const T& value)
		{
			if (!std::isfinite(hostTime))
			{
				return;
			}
			const auto it = std::lower_bound(m_Steps.begin(), m_Steps.end(), hostTime,
				[](const Step& s, double time) { return s.Time < time; });
			if (it != m_Steps.end() && it->Time == hostTime)
			{
				it->Value = value;
			}
			else
			{
				m_Steps.insert(it, Step{ hostTime, value });
			}
		}

		std::optional<T> At(double renderTime) const
		{
			const auto upper = std::upper_bound(m_Steps.begin(), m_Steps.end(), renderTime,
				[](double time, const Step& s) { return time < s.Time; });
			if (upper == m_Steps.begin())
			{
				return std::nullopt;
			}
			return (upper - 1)->Value;
		}

		void Trim(double renderTime)
		{
			const auto upper = std::upper_bound(m_Steps.begin(), m_Steps.end(), renderTime,
				[](double time, const Step& s) { return time < s.Time; });
			if (upper - m_Steps.begin() > 1)
			{
				m_Steps.erase(m_Steps.begin(), upper - 1);
			}
		}

		std::size_t Size() const { return m_Steps.size(); }
		bool Empty() const { return m_Steps.empty(); }
		void Clear() { m_Steps.clear(); }

	private:
		struct Step
		{
			double Time;
			T Value;
		};

		std::vector<Step> m_Steps;
	};

	// Drains only through TakeDue; clear it at an epoch change or run start.
	template<typename E>
	class EventQueue
	{
	public:
		void Schedule(double hostTime, const E& event)
		{
			if (!std::isfinite(hostTime))
			{
				return;
			}
			const auto it = std::upper_bound(m_Events.begin(), m_Events.end(), hostTime,
				[](double time, const Item& item) { return time < item.Time; });
			m_Events.insert(it, Item{ hostTime, event });
		}

		std::vector<E> TakeDue(double renderTime)
		{
			const auto end = std::upper_bound(m_Events.begin(), m_Events.end(), renderTime,
				[](double time, const Item& item) { return time < item.Time; });
			std::vector<E> due;
			due.reserve(static_cast<std::size_t>(end - m_Events.begin()));
			for (auto it = m_Events.begin(); it != end; ++it)
			{
				due.push_back(std::move(it->Event));
			}
			m_Events.erase(m_Events.begin(), end);
			return due;
		}

		std::size_t Size() const { return m_Events.size(); }
		bool Empty() const { return m_Events.empty(); }
		void Clear() { m_Events.clear(); }

	private:
		struct Item
		{
			double Time;
			E Event;
		};

		std::vector<Item> m_Events;
	};

}
