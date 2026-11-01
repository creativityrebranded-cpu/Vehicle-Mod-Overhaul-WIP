#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// The autopilot's stuck sense and trail memory (2026-10-31, the user: "understand what being stuck means ... moving and
// still having no change ... memory of where it came from ... backtrack and try alternative routes if the env can't
// recognize collisions sometimes").
//   Sense:       commanded to move but getting nowhere. Two rules: pinned (under a speed-scaled limit, capped at 4 m,
//                in 3.5 s) and getting nowhere (9 s of trying, net movement under 8 m and less than 2 m new best progress:
//                circling, sliding to and fro, pushing against something no contact reports).
//   Trail:       the points the vehicle actually drove through, 4 m apart, newest last, bounded.
//   Backtracker: on stuck, the trail point to back off to, the spot to mark blocked, and the alternative to try; escalates
//                and gives up cleanly after a bound (then pauses before trying again).
// No engine access; pure and unit-tested (tests/stucksense_test.cpp). Main thread.
namespace stucksense
{
	enum class Kind : std::uint8_t { None, Pinned, NoProgress };
	inline const char* Name(Kind k) { return k == Kind::Pinned ? "pinned" : k == Kind::NoProgress ? "getting nowhere" : "none"; }

	struct SenseParams
	{
		double pinnedWindow = 3.5, pinnedMove = 4.0;                            // s, m
		// Pinned scales with what is asked (2026-10-31 live run: hover turn-outs asked 2 m/s and moved ~2 m in 3.5 s, read as
		// pinned): under perSpeed x asked speed x window, at least pinnedMin, at most pinnedMove (4 m from 4.6 m/s up).
		double pinnedPerSpeed = 0.25, pinnedMin = 1.0;
		double progressWindow = 9.0, progressMove = 8.0, progressGain = 2.0;    // s, m, m
		double sampleEvery = 0.25;                                              // s between progress samples
		double gap = 1.0;                                                       // s between feeds that counts as a pause
	};

	struct Reading
	{
		Kind kind = Kind::None;
		bool movedOn = false;   // the pinned window just restarted because it moved far enough (good progress)
		double window = 0;      // s the verdict covers
		double moved = 0;       // m net position change over it
		double travelled = 0;   // m path length over it (getting nowhere only)
		double gain = 0;        // m the goal came closer over it (getting nowhere only)
		double reported = std::numeric_limits<double>::quiet_NaN();  // m/s mean speed the vehicle itself reported over it (NaN: none given)
	};

	class Sense
	{
	public:
		SenseParams p;

		void Restart(double now, double x, double y)
		{
			anchorT_ = now; anchorX_ = x; anchorY_ = y;
			repSum_ = repT_ = 0;
			n_ = 0; head_ = 0;
			bestBefore_ = std::numeric_limits<double>::infinity();
			have_ = true;
		}

		// One frame. trying: the pilot commands motion and is not waiting on purpose (arrived, holding, boxed waiting, a
		// manoeuvre timer). countProgress: this frame may count toward "getting nowhere" (trying, and not giving way to
		// another vehicle or creeping on purpose). goalDist: m to the goal the pilot steers to now (NaN: unknown).
		// commanded: m/s asked now (scales the pinned distance). reported: m/s the vehicle's own speed reading (NaN: none).
		Reading Feed(double now, double x, double y, double goalDist, bool trying, bool countProgress,
			double commanded = 1e9, double reported = std::numeric_limits<double>::quiet_NaN())
		{
			Reading r;
			const bool gap = !have_ || now - lastT_ > p.gap || now < lastT_;
			const double dt = now - lastT_;
			lastT_ = now;
			if (gap || !trying) {
				Restart(now, x, y);
				return r;
			}
			// ---- Pinned: under pinnedMove from the anchor for pinnedWindow ----
			if (std::isfinite(reported)) { repSum_ += reported * dt; repT_ += dt; }
			const double limit = std::clamp(p.pinnedPerSpeed * commanded * p.pinnedWindow, p.pinnedMin, p.pinnedMove);
			const double moved = std::hypot(x - anchorX_, y - anchorY_);
			if (moved > limit) {
				anchorT_ = now; anchorX_ = x; anchorY_ = y;
				repSum_ = repT_ = 0;
				r.movedOn = true;
			}
			else if (now - anchorT_ >= p.pinnedWindow) {
				r.kind = Kind::Pinned;
				r.window = now - anchorT_;
				r.moved = moved;
				if (repT_ > 0) r.reported = repSum_ / repT_;
				Restart(now, x, y);
				return r;
			}
			// ---- Getting nowhere: the samples of the last progressWindow ----
			if (!countProgress) { n_ = 0; head_ = 0; bestBefore_ = std::numeric_limits<double>::infinity(); return r; }
			if (n_ == 0 || now - Newest().t >= p.sampleEvery) Push({ now, x, y, goalDist });
			const Sample& old = Oldest();
			if (now - old.t < p.progressWindow) return r;
			// The oldest sample still inside the window (the ring holds a little more than the window).
			int first = 0;
			while (first + 1 < n_ && now - At(first + 1).t >= p.progressWindow) ++first;
			const Sample& s0 = At(first);
			double travelled = 0;
			for (int i = first; i + 1 < n_; ++i) travelled += std::hypot(At(i + 1).x - At(i).x, At(i + 1).y - At(i).y);
			travelled += std::hypot(x - Newest().x, y - Newest().y);
			const double net = std::hypot(x - s0.x, y - s0.y);
			// Gain: how much closer than its best before the window it got inside it (end-to-end differences swing with the
			// phase of a loop: a 5 m circle reads +-4 m; a loop over the same ground sets no new best).
			double inWindow = std::isfinite(goalDist) ? goalDist : std::numeric_limits<double>::infinity();
			for (int i = first; i < n_; ++i) if (std::isfinite(At(i).goalDist)) inWindow = std::min(inWindow, At(i).goalDist);
			const double before = std::isfinite(bestBefore_) ? bestBefore_ : s0.goalDist;
			const double gain = std::isfinite(before) && std::isfinite(inWindow) ? before - inWindow : 0.0;
			// Drop what fell out of the window (their best counts as "before").
			while (first-- > 0) { Forget(At(0)); head_ = (head_ + 1) % kRing; --n_; }
			if (net < p.progressMove && gain < p.progressGain) {
				r.kind = Kind::NoProgress;
				r.window = now - s0.t;
				r.moved = net;
				r.travelled = travelled;
				r.gain = gain;
				Restart(now, x, y);
			}
			return r;
		}

	private:
		struct Sample { double t = 0, x = 0, y = 0, goalDist = 0; };
		static constexpr int kRing = 64;   // 16 s at 0.25 s: more than the window
		Sample ring_[kRing];
		int head_ = 0, n_ = 0;
		bool have_ = false;
		double lastT_ = 0, anchorT_ = 0, anchorX_ = 0, anchorY_ = 0, repSum_ = 0, repT_ = 0;
		double bestBefore_ = std::numeric_limits<double>::infinity();  // closest to the goal among samples that left the window
		void Forget(const Sample& s) { if (std::isfinite(s.goalDist)) bestBefore_ = std::min(bestBefore_, s.goalDist); }

		const Sample& At(int i) const { return ring_[(head_ + i) % kRing]; }
		const Sample& Oldest() const { return At(0); }
		const Sample& Newest() const { return At(n_ - 1); }
		void Push(const Sample& s)
		{
			if (n_ == kRing) { Forget(At(0)); head_ = (head_ + 1) % kRing; --n_; }
			ring_[(head_ + n_) % kRing] = s;
			++n_;
		}
	};

	// The points the vehicle actually passed through, oldest first.
	class Trail
	{
	public:
		static constexpr int kMax = 48;
		double spacing = 4.0;
		struct Point { double x = 0, y = 0, t = 0; };

		void Clear() { n_ = 0; }
		int Size() const { return n_; }
		const Point& At(int i) const { return pts_[i]; }

		void Record(double now, double x, double y)
		{
			if (n_ && std::hypot(x - pts_[n_ - 1].x, y - pts_[n_ - 1].y) < spacing) return;
			if (n_ == kMax) { std::copy(pts_ + 1, pts_ + kMax, pts_); --n_; }
			pts_[n_++] = { x, y, now };
		}
		// Newest to oldest: the first point at least minBack (straight line) from (sx, sy) that `blocked` does not reject.
		// None that far: the oldest acceptable point when it is at least 3 m away. Nothing: -1.
		template <class BlockedFn>
		int PickBack(double sx, double sy, double minBack, BlockedFn&& blocked) const
		{
			int fallback = -1;
			for (int i = n_ - 1; i >= 0; --i) {
				const double d = std::hypot(pts_[i].x - sx, pts_[i].y - sy);
				if (blocked(pts_[i].x, pts_[i].y)) continue;
				if (d >= minBack) return i;
				if (d >= 3.0) fallback = i;
			}
			return fallback;
		}
		// m along the trail from point i to the newest point, plus the leg from the newest to (x, y).
		double Along(int i, double x, double y) const
		{
			double s = 0;
			for (int k = i; k + 1 < n_; ++k) s += std::hypot(pts_[k + 1].x - pts_[k].x, pts_[k + 1].y - pts_[k].y);
			if (n_) s += std::hypot(x - pts_[n_ - 1].x, y - pts_[n_ - 1].y);
			return s;
		}
		void TrimAfter(int i) { n_ = std::clamp(i + 1, 0, n_); }  // the newer points led into the stuck spot
		void DropSince(double t) { while (n_ && pts_[n_ - 1].t >= t) --n_; }  // recorded while getting nowhere: not a way out

	private:
		Point pts_[kMax];
		int n_ = 0;
	};

	enum class Alt : std::uint8_t { None, SideDetour, Climb, WiderDetour, OtherRoute };
	inline const char* Name(Alt a)
	{
		switch (a) {
		case Alt::SideDetour: return "a side detour (that heading avoided)";
		case Alt::Climb: return "climbing over it";
		case Alt::WiderDetour: return "a wider detour (that heading avoided more)";
		case Alt::OtherRoute: return "a different route (exploring, wide avoid)";
		default: return "none";
		}
	}

	struct Plan
	{
		bool ok = false;          // a backtrack attempt was made (crumb or not)
		bool haveCrumb = false;   // a trail point to back off to
		bool gaveUp = false;      // over the bound: no more backtracks for a while
		bool coolingOff = false;  // gave up recently: nothing done
		int attempt = 0;
		Alt alt = Alt::None;
		double tx = 0, ty = 0;    // the trail point
		double back = 0, along = 0;  // m straight / along the trail to it
		double markX = 0, markY = 0, markR = 0, markZ = 0;  // the spot marked blocked
		double heading = 0, half = 0;  // the blocked direction and the half width of the sector avoided (rad)
	};

	struct BacktrackParams
	{
		int maxAttempts = 3;
		double back[3] = { 8.0, 16.0, 30.0 };     // m back along the trail, per attempt
		double markR[3] = { 4.0, 6.0, 9.0 };      // m radius of the blocked spot, per attempt
		double half[3] = { 0.6, 1.0, 1.4 };       // rad half width of the avoided sector, per attempt
		double recoverGain = 15.0;                // m closer to the goal than at the first stuck: it worked
		double coolOff = 60.0;                    // s without backtracks after giving up
		double markSeconds = 90.0, sectorSeconds = 30.0;
		double timeout = 25.0;                    // s to reach the trail point
		double reach = 3.0;                       // m: at the trail point
	};

	class Backtracker
	{
	public:
		BacktrackParams p;
		Trail trail;

		enum class Update : std::uint8_t { None, Reached, TimedOut };

		bool Active() const { return active_; }
		double TargetX() const { return tx_; }
		double TargetY() const { return ty_; }
		double Since() const { return since_; }
		int Attempts() const { return attempts_; }
		bool CoolingOff(double now) const { return now < coolUntil_; }
		const Plan& Current() const { return plan_; }

		void Cancel() { active_ = false; }
		// Paused time (map, menu, loading screen) is not time spent backing off or cooling off.
		void Delay(double s) { since_ += s; if (coolUntil_ > 0) coolUntil_ += s; }
		// A new goal: earlier attempts were about another way.
		void NewGoal() { active_ = false; attempts_ = 0; coolUntil_ = 0; }

		// Stuck at (x, y) pushing along `heading`. blocked(x, y): a trail point the caller already knows is closed.
		// airAllowed: the vehicle may climb (permit, health, room above). stuckSince: when the stuck window began (trail points
		// from then on were laid while circling or pushing, and are dropped).
		template <class BlockedFn>
		Plan OnStuck(double now, double x, double y, double z, double heading, double halfLength, double goalDist, bool airAllowed, BlockedFn&& blocked,
			double stuckSince = 1e300)
		{
			Plan pl;
			trail.DropSince(stuckSince);
			if (now < coolUntil_) { pl.coolingOff = true; return pl; }
			if (attempts_ == 0) bestAtStuck_ = goalDist;
			++attempts_;
			if (attempts_ > p.maxAttempts) {
				pl.gaveUp = true;
				pl.attempt = attempts_;
				attempts_ = 0;
				active_ = false;
				coolUntil_ = now + p.coolOff;
				plan_ = pl;
				return pl;
			}
			const int k = attempts_ - 1;
			pl.ok = true;
			pl.attempt = attempts_;
			pl.alt = k == 0 ? Alt::SideDetour : k == 1 ? (airAllowed ? Alt::Climb : Alt::WiderDetour) : Alt::OtherRoute;
			pl.heading = heading;
			pl.half = p.half[k];
			// The blocked spot: just ahead of the nose, along the way it was pushing (never over the vehicle itself).
			pl.markR = p.markR[k];
			const double off = halfLength + 0.5 + pl.markR;
			pl.markX = x + std::sin(heading) * off;
			pl.markY = y + std::cos(heading) * off;
			pl.markZ = z;
			const int i = trail.PickBack(x, y, p.back[k], [&](double px, double py) {
				return std::hypot(px - pl.markX, py - pl.markY) < pl.markR + 2.0 || blocked(px, py);
			});
			if (i >= 0) {
				pl.haveCrumb = true;
				pl.tx = trail.At(i).x; pl.ty = trail.At(i).y;
				pl.back = std::hypot(pl.tx - x, pl.ty - y);
				pl.along = trail.Along(i, x, y);
				trail.TrimAfter(i);
				active_ = true;
				tx_ = pl.tx; ty_ = pl.ty;
				since_ = now;
			}
			else active_ = false;
			plan_ = pl;
			return pl;
		}

		Update Step(double now, double x, double y)
		{
			if (!active_) return Update::None;
			if (std::hypot(x - tx_, y - ty_) <= p.reach) { active_ = false; return Update::Reached; }
			if (now - since_ > p.timeout) { active_ = false; return Update::TimedOut; }
			return Update::None;
		}

		// The distance to the real goal, every frame: true once (and attempts reset) when it gained recoverGain on where
		// it first got stuck.
		bool NoteGoalDist(double goalDist)
		{
			if (attempts_ == 0 || active_ || !std::isfinite(goalDist) || !std::isfinite(bestAtStuck_)) return false;
			if (goalDist < bestAtStuck_ - p.recoverGain) {
				gained_ = bestAtStuck_ - goalDist;
				attempts_ = 0;
				return true;
			}
			return false;
		}
		double Gained() const { return gained_; }

	private:
		bool active_ = false;
		int attempts_ = 0;
		double tx_ = 0, ty_ = 0, since_ = 0, coolUntil_ = 0, bestAtStuck_ = 0, gained_ = 0;
		Plan plan_;
	};
}
