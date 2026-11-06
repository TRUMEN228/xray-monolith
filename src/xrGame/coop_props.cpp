////////////////////////////////////////////////////////////////////////////
//	Module 		: coop_props.cpp
//	Description : Anomaly Co-op: money and the world's physical things in the engine (r15).
//	              The lines: money|change  mset|the host's total  give|sum  door|id|angle  break|id|section
//	                         boom|section|x|y|z  (hit|... are coop_partner's)
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "coop_props.h"
#include "coop_creatures.h"
#include "coop_partner.h"
#include "CustomMonster.h"
#include "Level.h"
#include "Actor.h"
#include "PhysicObject.h"
#include "DestroyablePhysicsObject.h"
#include "ai_space.h"
#include "script_engine.h"
#include "../xrphysics/PhysicsShell.h"
#include "Hit.h"
#include "../Include/xrRender/Kinematics.h"

#include <map>
#include <set>

namespace coop_props
{
namespace
{
const u32 DOOR_SCAN_MS = 300;
const float DOOR_RADIUS = 30.f;
const float DOOR_MOVED = 0.08f;   // a door turned this much (radians) is told
const float DOOR_THERE = 0.04f;

bool shared_money = false;
bool money_known = false;
u32 money_last = 0;
bool was_shared_linked = false;
std::vector<std::string> outgoing;

std::set<u16> scripted_doors;
std::map<u16, float> door_told;     // door -> the angle last told or reached
std::map<u16, float> door_target;   // door -> the partner's angle it is driven to
u32 next_door_scan = 0;
bool applying = false;
bool friendly_fire = false;
bool is_host = false;

std::string f3(float v)
{
	char b[32];
	xr_sprintf(b, "%.3f", v);
	return b;
}

std::vector<std::string> split(const std::string& s, char c)
{
	std::vector<std::string> r;
	size_t at = 0;
	for (;;)
	{
		size_t e = s.find(c, at);
		r.push_back(s.substr(at, e == std::string::npos ? std::string::npos : e - at));
		if (e == std::string::npos)
			return r;
		at = e + 1;
	}
}

template <typename... A>
void call(LPCSTR name, A... args)
{
	try
	{
		luabind::functor<void> f;
		if (ai().script_engine().functor(name, f))
			f(args...);
	}
	catch (...)
	{
	}
}

CPhysicsJoint* door_joint(CObject* o)
{
	CPhysicObject* p = smart_cast<CPhysicObject*>(o);
	if (!p || scripted_doors.count(o->ID()) || !p->PPhysicsShell() || !p->PPhysicsShell()->isActive())
		return nullptr;
	return p->PPhysicsShell()->get_Joint("door");
}

void set_money(u32 m)
{
	applying = true;
	g_actor->set_money(m, true);
	applying = false;
	money_last = g_actor->get_money();
	money_known = true;
}

void money_frame(bool host, bool in_game)
{
	u32 m = g_actor->get_money();
	if (money_known && m != money_last && shared_money && in_game)
		outgoing.push_back("money|" + std::to_string((long long)m - (long long)money_last));
	money_last = m;
	money_known = true;
	// the two meet (or the host shares from now): the guest's total is the host's
	bool now = shared_money && in_game;
	if (host && now && !was_shared_linked)
		outgoing.push_back("mset|" + std::to_string(m));
	was_shared_linked = now;
}

void doors_frame(bool same_level, u32 now)
{
	// the partner's doors: driven to its angle, then let go
	for (auto it = door_target.begin(); it != door_target.end();)
	{
		CObject* o = Level().Objects.net_Find(it->first);
		CPhysicsJoint* j = door_joint(o);
		if (!j)
		{
			it = door_target.erase(it);
			continue;
		}
		float diff = it->second - j->GetAxisAngle(0);
		if (_abs(diff) < DOOR_THERE)
		{
			j->SetForceAndVelocity(0.f, 0.f, 0);
			door_told[it->first] = it->second;
			it = door_target.erase(it);
			continue;
		}
		smart_cast<CPhysicObject*>(o)->PPhysicsShell()->Enable();
		j->SetForceAndVelocity(2000.f, _max(-3.f, _min(3.f, diff * 6.f)), 0);
		++it;
	}
	if (!same_level || now < next_door_scan)
		return;
	next_door_scan = now + DOOR_SCAN_MS;
	// this player's doors (near either player): one turned is told
	Fvector centers[2];
	int n = 0;
	centers[n++] = g_actor->Position();
	u16 puppet = coop_creatures::partner_id();
	if (CObject* p = puppet != u16(-1) ? Level().Objects.net_Find(puppet) : nullptr)
		centers[n++] = p->Position();
	for (u32 i = 0, count = Level().Objects.o_count(); i < count; i++)
	{
		CObject* o = Level().Objects.o_get_by_iterator(i);
		if (!o || door_target.count(o->ID()))
			continue;
		bool near_one = false;
		for (int c = 0; c < n && !near_one; c++)
			near_one = o->Position().distance_to(centers[c]) < DOOR_RADIUS;
		if (!near_one)
			continue;
		CPhysicsJoint* j = door_joint(o);
		if (!j)
			continue;
		float ang = j->GetAxisAngle(0);
		auto t = door_told.find(o->ID());
		if (t == door_told.end())
			door_told[o->ID()] = ang;
		else if (_abs(ang - t->second) > DOOR_MOVED)
		{
			t->second = ang;
			outgoing.push_back("door|" + std::to_string(coop_creatures::host_id(o->ID())) + "|" + f3(ang));
		}
	}
}
} // namespace

void set_shared_money(bool on) { shared_money = on; }
void set_friendly_fire(bool on) { friendly_fire = on; }

void creature_hit(const SHit* h, CObject* puppet, CObject* who)
{
	// (an anomaly or a fall hurts the guest in its own game; the host's own shots are friendly fire)
	if (!is_host || !h || h->damage() <= 0.f || !smart_cast<CCustomMonster*>(who))
		return;
	LPCSTR bone = "";
	IKinematics* k = puppet ? smart_cast<IKinematics*>(puppet->Visual()) : nullptr;
	if (k && h->boneID != BI_NONE && h->boneID < k->LL_BoneCount())
		bone = k->LL_BoneName_dbg(h->boneID);
	Fvector d = h->direction();
	outgoing.push_back("phit|" + f3(h->damage()) + "|" + std::to_string((int)h->hit_type) + "|" + bone + "|" +
		std::to_string(who->ID()) + "|" + f3(d.x) + "|" + f3(d.y) + "|" + f3(d.z) + "|" + f3(h->phys_impulse()));
}

void friendly(const SHit* h, CObject* puppet)
{
	if (!friendly_fire || !h || h->damage() <= 0.f)
		return;
	LPCSTR bone = "";
	IKinematics* k = puppet ? smart_cast<IKinematics*>(puppet->Visual()) : nullptr;
	if (k && h->boneID != BI_NONE && h->boneID < k->LL_BoneCount())
		bone = k->LL_BoneName_dbg(h->boneID);
	Fvector d = h->direction();
	outgoing.push_back("ff|" + f3(h->damage()) + "|" + std::to_string((int)h->hit_type) + "|" + bone + "|" + f3(d.x) + "|" +
		f3(d.y) + "|" + f3(d.z) + "|" + f3(h->phys_impulse()));
}
void skip_door(u16 id) { scripted_doors.insert(id); }

void give_money(int amount)
{
	if (amount > 0)
		outgoing.push_back("give|" + std::to_string(amount));
	if (g_actor)
		money_last = g_actor->get_money(); // (the taking is no change for shared money)
}

void destroyed(CDestroyablePhysicsObject* obj)
{
	if (applying || !obj)
		return;
	outgoing.push_back("break|" + std::to_string(coop_creatures::host_id(obj->ID())) + "|" + obj->cNameSect().c_str());
}

void exploded(CObject* explosive, const Fvector& at)
{
	if (!explosive)
		return;
	outgoing.push_back(std::string("boom|") + explosive->cNameSect().c_str() + "|" + f3(at.x) + "|" + f3(at.y) + "|" + f3(at.z));
}

void frame(bool host, bool same_level, bool in_game, std::vector<std::string>& out)
{
	if (!g_pGameLevel || !g_actor)
	{
		outgoing.clear();
		door_told.clear();
		door_target.clear();
		scripted_doors.clear();
		money_known = false;
		was_shared_linked = false;
		return;
	}
	is_host = host;
	money_frame(host, in_game);
	doors_frame(same_level, Device.dwTimeGlobal);
	coop_partner::hits(host, same_level, out);
	std::string census;
	if (host && same_level && coop_creatures::census(census, Device.dwTimeGlobal))
		out.push_back(census);
	if (in_game)
		for (auto& l : outgoing)
		{
			// (the world's things only while both are on this level)
			bool world = l.compare(0, 5, "door|") == 0 || l.compare(0, 6, "break|") == 0 || l.compare(0, 5, "boom|") == 0;
			if (!world || same_level)
				out.push_back(l);
		}
	outgoing.clear();
}

void receive(bool host, const std::string& line)
{
	if (!g_pGameLevel || !g_actor)
		return;
	auto a = split(line, '|');
	const std::string& kind = a[0];
	if (kind == "census")
	{
		// the host's creatures for the guest's mirror
		if (!host)
			call("coop_mirror.engine_census", line.substr(7).c_str());
	}
	else if (kind == "phit" && a.size() >= 9)
	{
		// the host's creature hit this player's puppet there: the hit is this player's here
		if (!host)
			call("coop_combat.engine_phit", (float)atof(a[1].c_str()), atoi(a[2].c_str()), a[3].c_str(),
				(int)coop_creatures::local_id_of((u16)atoi(a[4].c_str())), (float)atof(a[5].c_str()),
				(float)atof(a[6].c_str()), (float)atof(a[7].c_str()), (float)atof(a[8].c_str()));
	}
	else if (kind == "ff" && a.size() >= 8)
		call("coop_team.engine_ffhit", (float)atof(a[1].c_str()), atoi(a[2].c_str()), a[3].c_str(), (float)atof(a[4].c_str()),
			(float)atof(a[5].c_str()), (float)atof(a[6].c_str()), (float)atof(a[7].c_str()));
	else if (kind == "hit")
	{
		if (host)
			coop_partner::hit_in(line.substr(4));
	}
	else if (kind == "money" && a.size() >= 2)
	{
		// (sent only while the host shares: the guest may not know it yet)
		long long m = (long long)g_actor->get_money() + atoll(a[1].c_str());
		set_money((u32)_max(0ll, m));
	}
	else if (kind == "mset" && a.size() >= 2)
	{
		if (!host)
			set_money((u32)strtoul(a[1].c_str(), nullptr, 10));
	}
	else if (kind == "give" && a.size() >= 2)
	{
		int n = atoi(a[1].c_str());
		if (n <= 0)
			return;
		set_money(g_actor->get_money() + (u32)n);
		call("coop_trade.engine_money_given", n);
	}
	else if (kind == "door" && a.size() >= 3)
	{
		u16 id = coop_creatures::local_id_of((u16)atoi(a[1].c_str()));
		if (door_joint(Level().Objects.net_Find(id)))
			door_target[id] = (float)atof(a[2].c_str());
	}
	else if (kind == "break" && a.size() >= 3)
	{
		u16 id = coop_creatures::local_id_of((u16)atoi(a[1].c_str()));
		CDestroyablePhysicsObject* d = smart_cast<CDestroyablePhysicsObject*>(Level().Objects.net_Find(id));
		if (d && a[2] == d->cNameSect().c_str())
		{
			applying = true;
			d->coop_destroy();
			applying = false;
		}
	}
	else if (kind == "boom" && a.size() >= 5)
		call("coop_fx.engine_boom", a[1].c_str(), (float)atof(a[2].c_str()), (float)atof(a[3].c_str()),
			(float)atof(a[4].c_str()));
}
} // namespace coop_props
