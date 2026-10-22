#include "TestFramework.h"

#include <DingoNet/Mirror.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace Dingo::Net;

namespace
{

	struct Pos
	{
		double X = 0.0;
	};

	Pos Interpolate(const Pos& a, const Pos& b, double t)
	{
		return { a.X + (b.X - a.X) * t };
	}

	Pos Extrapolate(const Pos& a, const Pos& b, double t)
	{
		return { a.X + (b.X - a.X) * t };
	}

	struct Held
	{
		double X = 0.0;
	};

	Held Interpolate(const Held& a, const Held& b, double t)
	{
		return { a.X + (b.X - a.X) * t };
	}

	bool Near(double a, double b, double eps = 1e-9)
	{
		return std::fabs(a - b) <= eps;
	}

	using PosTracks = Tracks<uint32_t, Pos>;

}

TEST(MirrorInterpolatesBetweenSamples)
{
	PosTracks tracks;
	tracks.Push(1, 1.0, { 10.0 });
	tracks.Push(1, 2.0, { 20.0 });
	const auto mid = tracks.Sample(1, 1.25);
	CHECK(mid.has_value());
	CHECK(Near(mid->X, 12.5));
	CHECK(!tracks.Sample(2, 1.5).has_value());
}

TEST(MirrorSingleSampleAndBeforeOldestHold)
{
	PosTracks tracks;
	tracks.Push(1, 5.0, { 7.0 });
	CHECK(Near(tracks.Sample(1, 4.0)->X, 7.0));
	CHECK(Near(tracks.Sample(1, 9.0)->X, 7.0));
	tracks.Push(1, 6.0, { 8.0 });
	CHECK(Near(tracks.Sample(1, 1.0)->X, 7.0));
}

TEST(MirrorExtrapolatesUpToTheCapThenHolds)
{
	PosTracks tracks;
	tracks.Push(1, 1.0, { 0.0 });
	tracks.Push(1, 2.0, { 10.0 });
	CHECK(Near(tracks.Sample(1, 2.1)->X, 11.0));
	CHECK(Near(tracks.Sample(1, 2.15)->X, 11.5));
	CHECK(Near(tracks.Sample(1, 2.5)->X, 11.5));
	CHECK(Near(tracks.Sample(1, 50.0)->X, 11.5));
}

TEST(MirrorHoldsWithoutExtrapolate)
{
	Tracks<uint32_t, Held> tracks;
	tracks.Push(1, 1.0, { 0.0 });
	tracks.Push(1, 2.0, { 10.0 });
	CHECK(Near(tracks.Sample(1, 2.1)->X, 10.0));
	CHECK(Near(tracks.Sample(1, 1.5)->X, 5.0));
}

TEST(MirrorOutOfOrderAndDuplicateSamples)
{
	PosTracks tracks;
	tracks.Push(1, 1.0, { 0.0 });
	tracks.Push(1, 3.0, { 30.0 });
	tracks.Push(1, 2.0, { 20.0 });
	CHECK(tracks.SampleCount(1) == 3);
	CHECK(Near(tracks.Sample(1, 2.5)->X, 25.0));
	tracks.Push(1, 2.0, { 22.0 });
	CHECK(tracks.SampleCount(1) == 3);
	CHECK(Near(tracks.Sample(1, 2.0)->X, 22.0));
}

TEST(MirrorStaleKeysAreForgotten)
{
	PosTracks tracks;
	tracks.Push(1, 1.0, { 0.0 });
	tracks.Push(2, 2.5, { 0.0 });
	tracks.Trim(3.0);
	CHECK(tracks.Has(1) && tracks.Has(2));
	tracks.Trim(3.1);
	CHECK(!tracks.Has(1));
	CHECK(tracks.Has(2));
	tracks.Trim(4.6);
	CHECK(tracks.Keys().empty());
}

TEST(MirrorTrimKeepsTheBracket)
{
	PosTracks tracks;
	for (int i = 0; i <= 40; ++i)
	{
		tracks.Push(1, i * 0.05, { i * 1.0 });
	}
	tracks.Trim(2.0);
	CHECK(tracks.SampleCount(1) < 41);
	CHECK(Near(tracks.Sample(1, 1.0)->X, 20.0));
	CHECK(Near(tracks.Sample(1, 1.03)->X, 20.6));
	CHECK(Near(tracks.Sample(1, 2.0)->X, 40.0));

	PosTracks sparse;
	sparse.Push(1, 0.0, { 0.0 });
	sparse.Push(1, 1.8, { 18.0 });
	sparse.Trim(2.0);
	CHECK(sparse.SampleCount(1) == 2);
	CHECK(Near(sparse.Sample(1, 0.9)->X, 9.0));
}

TEST(MirrorKeysForgetAndClear)
{
	PosTracks tracks;
	tracks.Push(1, 1.0, {});
	tracks.Push(2, 1.0, {});
	CHECK(tracks.Keys().size() == 2);
	tracks.Forget(1);
	CHECK(!tracks.Has(1));
	tracks.Clear();
	CHECK(tracks.Count() == 0);
}

TEST(MirrorLostPartOnlyCostsItsKeys)
{
	PosTracks tracks;
	for (int i = 0; i <= 4; ++i)
	{
		tracks.Push(1, i * 0.05, { i * 1.0 });
		if (i < 3)
		{
			tracks.Push(2, i * 0.05, { i * 1.0 });
		}
	}
	CHECK(Near(tracks.Sample(1, 0.15)->X, 3.0));
	CHECK(Near(tracks.Sample(2, 0.15)->X, 3.0));
}

TEST(MirrorTimelineSteps)
{
	Timeline<int> timeline;
	CHECK(!timeline.At(1.0).has_value());
	timeline.Set(2.0, 20);
	timeline.Set(1.0, 10);
	timeline.Set(3.0, 30);
	CHECK(!timeline.At(0.5).has_value());
	CHECK(*timeline.At(1.0) == 10);
	CHECK(*timeline.At(1.99) == 10);
	CHECK(*timeline.At(2.0) == 20);
	CHECK(*timeline.At(9.0) == 30);
	timeline.Trim(2.5);
	CHECK(timeline.Size() == 2);
	CHECK(*timeline.At(2.5) == 20);
	timeline.Set(2.0, 21);
	CHECK(*timeline.At(2.5) == 21);
}

TEST(MirrorEventsAreDueInOrderOnce)
{
	EventQueue<std::string> events;
	events.Schedule(3.0, "c");
	events.Schedule(1.0, "a");
	events.Schedule(2.0, "b1");
	events.Schedule(2.0, "b2");
	CHECK(events.TakeDue(0.5).empty());
	const auto first = events.TakeDue(2.0);
	CHECK(first.size() == 3);
	CHECK(first[0] == "a" && first[1] == "b1" && first[2] == "b2");
	CHECK(events.TakeDue(2.0).empty());
	events.Schedule(1.0, "late");
	const auto rest = events.TakeDue(10.0);
	CHECK(rest.size() == 2);
	CHECK(rest[0] == "late" && rest[1] == "c");
	CHECK(events.Empty());
}

TEST(MirrorClockStartsAtFirstSnapshot)
{
	RenderClock clock;
	CHECK(!clock.HasTime());
	clock.Advance(0.5);
	CHECK(!clock.HasTime());
	clock.OnSnapshot(10.0);
	CHECK(clock.HasTime());
	CHECK(Near(clock.HostNow(), 10.0));
	CHECK(Near(clock.RenderTime(), 9.9));
	clock.Advance(0.05);
	CHECK(Near(clock.HostNow(), 10.05));
	clock.Reset();
	CHECK(!clock.HasTime());
}

TEST(MirrorClockSettleHoldsAtTheLastStamp)
{
	RenderClock clock;
	clock.OnSnapshot(10.0);
	clock.Settle();
	for (int i = 0; i < 120; ++i)
	{
		clock.Advance(1.0 / 60.0);
	}
	CHECK(Near(clock.RenderTime(), 10.0));
	clock.OnSnapshot(10.5);
	clock.Advance(1.0 / 60.0);
	CHECK(clock.RenderTime() > 10.0);
	for (int i = 0; i < 300; ++i)
	{
		clock.Advance(1.0 / 60.0);
	}
	CHECK(clock.RenderTime() > 10.5);
}

TEST(MirrorClockSnapsOnLargeForwardError)
{
	RenderClock clock;
	clock.OnSnapshot(1.0);
	clock.Advance(0.01);
	clock.OnSnapshot(100.0);
	clock.Advance(0.01);
	CHECK(Near(clock.RenderTime(), clock.HostNow() - 0.1, 0.05));
}

TEST(MirrorClockIgnoresOldStamps)
{
	RenderClock clock;
	clock.OnSnapshot(10.0);
	clock.OnSnapshot(9.9);
	CHECK(Near(clock.LatestStamp(), 10.0));
	CHECK(clock.HostNow() >= 10.0);
	CHECK(clock.Restarts() == 0);
}

TEST(MirrorExtrapolationNeedsASpanBetweenSamples)
{
	static_assert(PosTracks::Extrapolates);
	static_assert(!Tracks<uint32_t, Held>::Extrapolates);
	PosTracks close;
	close.Push(1, 1.0, { 0.0 });
	close.Push(1, 1.000001, { 1.0 });
	CHECK(Near(close.Sample(1, 1.1)->X, 1.0));

	PosTracks three;
	three.Push(1, 0.5, { 0.0 });
	three.Push(1, 1.0, { 5.0 });
	three.Push(1, 1.000001, { 5.0 });
	CHECK(Near(three.Sample(1, 1.1)->X, 6.0, 1e-3));
}

TEST(MirrorForEachVisitsEveryKey)
{
	PosTracks tracks;
	tracks.Push(1, 1.0, {});
	tracks.Push(5, 1.0, {});
	uint32_t sum = 0;
	tracks.ForEach([&sum](uint32_t key) { sum += key; });
	CHECK(sum == 6);
}

TEST(MirrorClockRecoversAfterSettleAndSilence)
{
	RenderClock clock;
	clock.OnSnapshot(10.0);
	clock.Settle();
	for (int i = 0; i < 40; ++i)
	{
		clock.Advance(0.25);
	}
	CHECK(Near(clock.HostNow(), 10.0));
	clock.OnSnapshot(10.05);
	for (int i = 0; i < 60; ++i)
	{
		clock.Advance(1.0 / 60.0);
	}
	CHECK(clock.RenderTime() <= clock.HostNow());
	CHECK(std::fabs(clock.RenderTime() - (clock.LatestStamp() - 0.1)) < 1.2);
	CHECK(clock.RenderTime() > 10.0);
}

TEST(MirrorClockConvergesAfterAHostStall)
{
	RenderClock clock;
	double host = 10.0;
	for (int i = 0; i < 60; ++i)
	{
		if (i % 3 == 0)
		{
			clock.OnSnapshot(host);
			host += 0.05;
		}
		clock.Advance(1.0 / 60.0);
	}
	for (int i = 0; i < 180; ++i)
	{
		clock.Advance(1.0 / 60.0);
	}
	clock.OnSnapshot(host);
	clock.Advance(1.0 / 60.0);
	CHECK(std::fabs(clock.RenderTime() - (host - 0.1)) < 0.05);
	CHECK(clock.RenderTime() <= clock.HostNow());
}

TEST(MirrorClockRestartsWhenStampsJumpBack)
{
	RenderClock clock;
	clock.OnSnapshot(100.0);
	clock.Advance(0.1);
	CHECK(clock.Restarts() == 0);
	clock.OnSnapshot(2.0);
	CHECK(clock.Restarts() == 1);
	CHECK(Near(clock.LatestStamp(), 2.0));
	CHECK(Near(clock.RenderTime(), 1.9));
	clock.OnSnapshot(2.05);
	clock.Advance(0.05);
	CHECK(clock.Restarts() == 1);
	CHECK(clock.RenderTime() > 1.9 && clock.RenderTime() <= clock.HostNow());
}

TEST(MirrorClockClampsHugeDt)
{
	RenderClock clock;
	clock.OnSnapshot(10.0);
	clock.Advance(1.0e9);
	CHECK(clock.HostNow() <= 10.0 + RenderClockParams{}.MaxDt + 1e-9);
	CHECK(clock.RenderTime() <= clock.HostNow());
}

namespace
{

	constexpr double SPEED = 10.0;
	constexpr double FAST_SPEED = 14.0;
	constexpr double SPEED_CHANGE = 110.0;
	constexpr double FRAME = 1.0 / 60.0;

	double PositionAt(double stamp)
	{
		return stamp <= SPEED_CHANGE ? (stamp - 100.0) * SPEED
			: (SPEED_CHANGE - 100.0) * SPEED + (stamp - SPEED_CHANGE) * FAST_SPEED;
	}

	struct Arrival
	{
		double Local;
		double Stamp;
	};

	std::vector<Arrival> MakeFeed(uint32_t seed, double jitter, int seconds, double loss, double gapStart, double gapEnd)
	{
		std::mt19937 rng(seed);
		std::uniform_real_distribution<double> noise(-jitter, jitter);
		std::uniform_real_distribution<double> unit(0.0, 1.0);
		std::vector<Arrival> arrivals;
		for (int i = 0; i < seconds * 20; ++i)
		{
			const double stamp = 100.0 + i * 0.05;
			const double local = stamp - 100.0 + 0.05 + noise(rng);
			const bool lost = i > 0 && unit(rng) < loss;
			if (!lost && !(local >= gapStart && local < gapEnd))
			{
				arrivals.push_back({ local, stamp });
			}
		}
		std::stable_sort(arrivals.begin(), arrivals.end(),
			[](const Arrival& a, const Arrival& b) { return a.Local < b.Local; });
		return arrivals;
	}

}

TEST(MirrorClockMonotonicAndNeverPastHostNowUnderJitter)
{
	for (uint32_t seed = 1; seed <= 5; ++seed)
	{
		const auto feed = MakeFeed(seed, 0.03, 20, 0.0, 0.0, 0.0);
		RenderClock clock;
		size_t next = 0;
		double previous = -1.0e300;
		double previousHost = -1.0e300;
		bool ok = true;
		for (int frame = 0; frame < 20 * 60; ++frame)
		{
			const double local = frame * FRAME;
			while (next < feed.size() && feed[next].Local <= local)
			{
				clock.OnSnapshot(feed[next++].Stamp);
			}
			clock.Advance(FRAME);
			if (!clock.HasTime())
			{
				continue;
			}
			ok = ok && clock.RenderTime() >= previous;
			ok = ok && clock.RenderTime() <= clock.HostNow();
			ok = ok && clock.HostNow() >= clock.LatestStamp();
			ok = ok && clock.HostNow() >= previousHost;
			previous = clock.RenderTime();
			previousHost = clock.HostNow();
		}
		CHECK(ok);
	}
}

// With a render rate capped at 1 + MaxSlew a frame moves the object at most (1 + MaxSlew) * speed * dt,
// so 1.2 leaves rounding room only; the window around the gap is bounded by the distance the host covers
// while render time holds at the extrapolation cap.
static void RunSmoothness(double loss, double gapStart, double gapEnd, double maxSpeed)
{
	const double normalMax = 1.2 * maxSpeed * FRAME;
	const double gapMax = maxSpeed * (gapEnd - gapStart);
	for (uint32_t seed = 1; seed <= 5; ++seed)
	{
		const auto feed = MakeFeed(seed, 0.015, 30, loss, gapStart, gapEnd);
		RenderClock clock;
		PosTracks tracks;
		size_t next = 0;
		double last = 0.0;
		bool haveLast = false;
		bool ok = true;
		for (int frame = 0; frame < 30 * 60; ++frame)
		{
			const double local = frame * FRAME;
			while (next < feed.size() && feed[next].Local <= local)
			{
				const double stamp = feed[next++].Stamp;
				clock.OnSnapshot(stamp);
				tracks.Push(7, stamp, { PositionAt(stamp) });
			}
			clock.Advance(FRAME);
			if (!clock.HasTime())
			{
				continue;
			}
			tracks.Trim(clock.RenderTime());
			const auto pos = tracks.Sample(7, clock.RenderTime());
			CHECK(pos.has_value());
			if (!pos)
			{
				return;
			}
			if (haveLast && frame > 60)
			{
				const double step = pos->X - last;
				const bool nearGap = gapEnd > gapStart && local >= gapStart && local < gapEnd + 0.6;
				ok = ok && step >= 0.0 && step <= (nearGap ? gapMax : normalMax);
				if (!nearGap)
				{
					ok = ok && step >= 0.5 * SPEED * FRAME;
				}
			}
			last = pos->X;
			haveLast = true;
		}
		CHECK(ok);
	}
}

TEST(MirrorJitteredFeedRendersSmoothly)
{
	RunSmoothness(0.0, 0.0, 0.0, FAST_SPEED);
}

TEST(MirrorLossyFeedRendersSmoothly)
{
	RunSmoothness(0.1, 0.0, 0.0, FAST_SPEED);
}

TEST(MirrorFeedWithAGapRendersSmoothly)
{
	RunSmoothness(0.1, 15.0, 15.3, FAST_SPEED);
}
