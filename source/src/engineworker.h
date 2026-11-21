#pragma once
// One long-lived thread for every FVO job that runs game code off the main thread (the car navmesh input and bake, the navmesh probe).
// 2026-11-06 12:04 run: the game numbers each thread the first time it takes one of its reader locks (FUN_1405abfe0: a TLS slot filled
// from a global counter, at most 64 numbers, never given back; the lock keeps one slot per number, FUN_14240a840). With one new thread
// per navmesh job, each job used up a number: job 25 was past 64 and it and every later job faulted reading a slot past the end
// (Starfield.exe+2404250); from then on the game clamps every new thread to number 64, its own included. One thread here = one number,
// for the whole session. Jobs run one at a time, in the order posted. Trace: OBS system=engineworker event=started/queued/job_exception.
#include "log.h"

#include <Windows.h>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace engineworker
{
	struct Queue { std::mutex m; std::condition_variable cv; std::deque<std::function<void()>> jobs; bool started = false; };
	inline Queue& Q() { static Queue q; return q; }
	inline void Loop()
	{
		auto& q = Q();
		logger::Write("OBS system=engineworker event=started thread=%lu", GetCurrentThreadId());
		for (;;) {
			std::function<void()> job;
			{
				std::unique_lock lk(q.m);
				q.cv.wait(lk, [&] { return !q.jobs.empty(); });
				job = std::move(q.jobs.front());
				q.jobs.pop_front();
			}
			try { job(); }
			catch (...) { logger::Write("OBS system=engineworker event=job_exception thread=%lu", GetCurrentThreadId()); }
		}
	}
	// Main thread: run `job` on the engine thread (started on first use). False only when the thread cannot be started.
	inline bool Post(std::function<void()> job, const char* what)
	{
		auto& q = Q();
		std::size_t waiting = 0;
		{
			std::lock_guard lk(q.m);
			if (!q.started) {
				try { std::thread(Loop).detach(); q.started = true; }
				catch (...) { return false; }
			}
			q.jobs.push_back(std::move(job));
			waiting = q.jobs.size();
		}
		q.cv.notify_one();
		if (waiting > 1) logger::Write("OBS system=engineworker event=queued what=%s waiting=%zu", what ? what : "-", waiting);
		return true;
	}
}
