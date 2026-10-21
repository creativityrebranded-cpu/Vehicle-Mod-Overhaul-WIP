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
		// One assignment per class (Navigation / Combat / Weapons): each class is set and cleared on its own.
		struct Slots
		{
			Assignment cls[kClasses];
			bool Empty() const
			{
				for (const auto& a : cls) if (a.profile) return false;
				return true;
			}
		};
		std::unordered_map<std::uint32_t, Slots> g_assigned;
		const SeatProvider* g_seats = nullptr;
		std::mutex g_lock;  // main thread in practice; cheap insurance

		int Index(Class c) { return int(c) >= 0 && int(c) < kClasses ? int(c) : 0; }
		const char* ClassName(Class c) { return c == Class::Combat ? "combat" : c == Class::Weapons ? "weapons" : "navigation"; }
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
			Unassign(vehicleId, Class::Navigation);
			return;
		}
		std::lock_guard lk(g_lock);
		auto& slot = g_assigned[vehicleId].cls[Index(p->cls)];
		Assignment a;
		a.profile = p;
		a.ctx.vehicleId = vehicleId;
		a.ctx.homeX = homeX;
		a.ctx.homeY = homeY;
		a.ctx.since = a.ctx.stepAt = GetTickCount64();
		slot = a;
		logger::Write("Profiles: vehicle %08X %s -> %s (home %.0f, %.0f)", vehicleId, ClassName(p->cls), p->name, homeX, homeY);
	}

	void Unassign(std::uint32_t vehicleId)
	{
		std::lock_guard lk(g_lock);
		const auto it = g_assigned.find(vehicleId);
		if (it == g_assigned.end()) return;
		const auto name = [&](int c) { return it->second.cls[c].profile ? it->second.cls[c].profile->name : "-"; };
		logger::Write("Profiles: vehicle %08X -> default (was %s / %s / %s)", vehicleId, name(0), name(1), name(2));
		g_assigned.erase(it);
	}

	void Unassign(std::uint32_t vehicleId, Class cls)
	{
		std::lock_guard lk(g_lock);
		const auto it = g_assigned.find(vehicleId);
		if (it == g_assigned.end()) return;
		auto& slot = it->second.cls[Index(cls)];
		if (!slot.profile) return;
		logger::Write("Profiles: vehicle %08X %s -> default (was %s)", vehicleId, ClassName(cls), slot.profile->name);
		slot = Assignment{};
		if (it->second.Empty()) g_assigned.erase(it);
	}

	const Profile* AssignedTo(std::uint32_t vehicleId) { return AssignedTo(vehicleId, Class::Navigation); }

	const Profile* AssignedTo(std::uint32_t vehicleId, Class cls)
	{
		std::lock_guard lk(g_lock);
		const auto it = g_assigned.find(vehicleId);
		return it == g_assigned.end() ? nullptr : it->second.cls[Index(cls)].profile;
	}

	bool GoalFor(User user, int pilot, std::uintptr_t vehicle, std::uintptr_t player, Goal& out, Class cls)
	{
		if (!vehicle) return false;
		const std::uint32_t id = *reinterpret_cast<const std::uint32_t*>(vehicle + 0x28);
		const int c = Index(cls);
		const Profile* p = nullptr;
		Context ctx;
		{
			std::lock_guard lk(g_lock);
			const auto it = g_assigned.find(id);
			if (it == g_assigned.end() || !it->second.cls[c].profile) return false;
			p = it->second.cls[c].profile;
			if (!p->goal) return false;                            // a selector (combat / weapons) without a goal of its own
			if (!(p->users & std::uint8_t(user))) return false;  // not for this user (e.g. a remote profile while driving)
			if (p->needsSeats && !g_seats) return false;           // waits for the seat owner
			ctx = it->second.cls[c].ctx;
		}
		ctx.user = user;
		ctx.pilot = pilot;
		ctx.vehicle = vehicle;
		ctx.player = player;
		out = Goal{};
		const bool have = p->goal(ctx, out);
		std::lock_guard lk(g_lock);
		const auto it = g_assigned.find(id);
		if (it == g_assigned.end() || it->second.cls[c].profile != p) return have;
		if (ctx.done) {
			logger::Write("Profiles: vehicle %08X finished %s; %s default again", id, p->name, ClassName(cls));
			it->second.cls[c] = Assignment{};  // its own class only: a finished trip leaves the combat and weapons choices alone
			if (it->second.Empty()) g_assigned.erase(it);
			return false;
		}
		it->second.cls[c].ctx.step = ctx.step;
		it->second.cls[c].ctx.stepAt = ctx.stepAt;
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
