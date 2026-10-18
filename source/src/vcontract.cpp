#include "vcontract.h"

#include "log.h"

#include <Windows.h>

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vcontract
{
	namespace
	{
		std::vector<const Profile*> g_profiles;
		struct Assignment
		{
			const Profile* profile = nullptr;
			Context ctx;
		};
		std::unordered_map<std::uint32_t, Assignment> g_assigned;
		const SeatProvider* g_seats = nullptr;
		std::mutex g_lock;  // main thread in practice; cheap insurance
	}

	void Register(const Profile* p)
	{
		std::lock_guard lk(g_lock);
		for (const auto* q : g_profiles) {
			if (q == p) return;
		}
		g_profiles.push_back(p);
	}

	int Count()
	{
		std::lock_guard lk(g_lock);
		return int(g_profiles.size());
	}

	const Profile* ProfileAt(int i)
	{
		std::lock_guard lk(g_lock);
		return i >= 0 && i < int(g_profiles.size()) ? g_profiles[i] : nullptr;
	}

	const Profile* Find(const char* name)
	{
		std::lock_guard lk(g_lock);
		for (const auto* p : g_profiles) {
			if (name && std::strcmp(p->name, name) == 0) return p;
		}
		return nullptr;
	}

	void Assign(std::uint32_t vehicleId, const Profile* p, double homeX, double homeY)
	{
		if (!vehicleId) return;
		if (!p) {
			Unassign(vehicleId);
			return;
		}
		std::lock_guard lk(g_lock);
		Assignment a;
		a.profile = p;
		a.ctx.vehicleId = vehicleId;
		a.ctx.homeX = homeX;
		a.ctx.homeY = homeY;
		a.ctx.since = a.ctx.stepAt = GetTickCount64();
		g_assigned[vehicleId] = a;
		logger::Write("Profiles: vehicle %08X -> %s (home %.0f, %.0f)", vehicleId, p->name, homeX, homeY);
	}

	void Unassign(std::uint32_t vehicleId)
	{
		std::lock_guard lk(g_lock);
		const auto it = g_assigned.find(vehicleId);
		if (it == g_assigned.end()) return;
		logger::Write("Profiles: vehicle %08X -> default (was %s)", vehicleId, it->second.profile ? it->second.profile->name : "?");
		g_assigned.erase(it);
	}

	const Profile* AssignedTo(std::uint32_t vehicleId)
	{
		std::lock_guard lk(g_lock);
		const auto it = g_assigned.find(vehicleId);
		return it == g_assigned.end() ? nullptr : it->second.profile;
	}

	bool GoalFor(User user, int pilot, std::uintptr_t vehicle, std::uintptr_t player, Goal& out)
	{
		if (!vehicle) return false;
		const std::uint32_t id = *reinterpret_cast<const std::uint32_t*>(vehicle + 0x28);
		const Profile* p = nullptr;
		Context ctx;
		{
			std::lock_guard lk(g_lock);
			const auto it = g_assigned.find(id);
			if (it == g_assigned.end() || !it->second.profile) return false;
			p = it->second.profile;
			if (!(p->users & std::uint8_t(user))) return false;  // not for this user (e.g. a remote profile while driving)
			if (p->needsSeats && !g_seats) return false;           // waits for the seat owner
			ctx = it->second.ctx;
		}
		ctx.user = user;
		ctx.pilot = pilot;
		ctx.vehicle = vehicle;
		ctx.player = player;
		out = Goal{};
		const bool have = p->goal(ctx, out);
		std::lock_guard lk(g_lock);
		const auto it = g_assigned.find(id);
		if (it == g_assigned.end() || it->second.profile != p) return have;
		if (ctx.done) {
			logger::Write("Profiles: vehicle %08X finished %s; default again", id, p->name);
			g_assigned.erase(it);
			return false;
		}
		it->second.ctx.step = ctx.step;
		it->second.ctx.stepAt = ctx.stepAt;
		return have;
	}

	void SetSeatProvider(const SeatProvider* p)
	{
		std::lock_guard lk(g_lock);
		g_seats = p;
		logger::Write("Profiles: seat provider %s", p ? "registered" : "removed");
	}

	const SeatProvider* Seats()
	{
		std::lock_guard lk(g_lock);
		return g_seats;
	}

	void Reset()
	{
		std::lock_guard lk(g_lock);
		g_assigned.clear();
	}
}
