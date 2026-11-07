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
#include "coop_items.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "../xrServerEntities/xrServer_Objects_ALife_Monsters.h"
#include "../xrServerEntities/xrServer_Objects_ALife_Items.h"
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
bool spawns_armed = false;
bool info_ready = false;
std::map<u16, u32> fresh;            // host: newly registered objects -> the frame they came (looked at a frame later)
std::map<std::string, u32> info_count; // a flag's changes sent this minute (flags that flicker are not synced)
u32 info_reset = 0;
std::set<std::string> info_skip;     // this player's own state, never sent
bool quiet_now()
{
	// the scripts' own copies of the partner's things (a drop, a gift, a stash) are no new spawns
	if (coop_items::busy())
		return true;
	try
	{
		luabind::functor<bool> f;
		if (ai().script_engine().functor("coop_spawn.engine_quiet", f))
			return f();
	}
	catch (...)
	{
	}
	return false;
}

std::string esc(const char* s)
{
	std::string r;
	for (; s && *s; s++)
	{
		if (*s == '\\') r += "\\\\";
		else if (*s == '|') r += "\\p";
		else if (*s == '\n') r += "\\n";
		else if (*s == '\t') r += "\\t";
		else if (*s != '\r') r += *s;
	}
	return r;
}

std::string unesc(const std::string& s)
{
	std::string r;
	for (size_t i = 0; i < s.size(); i++)
	{
		if (s[i] == '\\' && i + 1 < s.size())
		{
			char c = s[++i];
			r += c == 'p' ? '|' : (c == 'n' ? '\n' : (c == 't' ? '\t' : c));
		}
		else
			r += s[i];
	}
	return r;
}

bool removable(CSE_ALifeDynamicObject* o)
{
	return smart_cast<CSE_ALifeOnlineOfflineGroup*>(o) || smart_cast<CSE_ALifeMonsterAbstract*>(o) ||
		smart_cast<CSE_ALifeItemArtefact*>(o);
}

// a new object a frame after it came: an artefact, a thing on the ground or in a box, a squad member
void spawns_frame()
{
	if (fresh.empty())
		return;
	for (auto it = fresh.begin(); it != fresh.end();)
	{
		if (Device.dwFrame - it->second < 2)
		{
			++it;
			continue;
		}
		u16 id = it->first;
		it = fresh.erase(it);
		CSE_ALifeDynamicObject* se = ai().alife().objects().object(id, true);
		if (!se)
			continue;
		CSE_ALifeDynamicObject* parent = se->ID_Parent != u16(-1) ? ai().alife().objects().object(se->ID_Parent, true) : nullptr;
		if (CSE_ALifeMonsterAbstract* m = smart_cast<CSE_ALifeMonsterAbstract*>(se))
		{
			CSE_Abstract* squad = m->m_group_id != u16(-1) ? ai().alife().objects().object(m->m_group_id, true) : nullptr;
			if (!squad)
				continue; // (one without a squad is the mirror's to bring)
			char l[320];
			xr_sprintf(l, "sqm|%u|%s|%u|%s|%.2f|%.2f|%.2f|%u|%u|%s", squad->ID, squad->name(), id, se->name(), se->o_Position.x,
				se->o_Position.y, se->o_Position.z, se->m_tNodeID, (u32)se->m_tGraphID,
				m->m_smart_terrain_id != u16(-1) ? std::to_string(m->m_smart_terrain_id).c_str() : "");
			outgoing.push_back(l);
			continue;
		}
		if (smart_cast<CSE_ALifeInventoryBox*>(se) || !smart_cast<CSE_ALifeInventoryItem*>(se))
			continue;
		LPCSTR kind = nullptr;
		if (!parent)
			kind = smart_cast<CSE_ALifeItemArtefact*>(se) ? "art" : (pSettings->line_exist(se->name(), "inv_weight") ? "ground" : nullptr);
		else if (smart_cast<CSE_ALifeInventoryBox*>(parent))
			kind = "box";
		if (!kind)
			continue;
		std::string l = coop_items::world_line(id, kind);
		if (!l.empty())
			outgoing.push_back(l);
	}
}

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
void set_spawns_armed(bool on) { spawns_armed = on; }
void set_info_ready(bool on) { info_ready = on; }
void skip_info(LPCSTR info) { if (info) info_skip.insert(info); }

void registered(CSE_ALifeDynamicObject* o)
{
	if (!o || !is_host || !spawns_armed || !g_pGameLevel || o->ID == (g_actor ? g_actor->ID() : u16(-1)) || quiet_now())
		return;
	fresh[o->ID] = Device.dwFrame;
}

void unregistered(CSE_ALifeDynamicObject* o)
{
	if (!o || !is_host || !spawns_armed || !g_pGameLevel || !removable(o) || quiet_now())
		return;
	fresh.erase(o->ID);
	outgoing.push_back("unreg|" + std::to_string(o->ID) + "|" + o->name());
}

void info_changed(LPCSTR info, bool on)
{
	if (applying || !info_ready || !info || !*info || !g_actor || info_skip.count(info))
		return;
	if (Device.dwTimeGlobal > info_reset)
	{
		info_count.clear();
		info_reset = Device.dwTimeGlobal + 60000;
	}
	if (++info_count[info] <= 3)
		outgoing.push_back(std::string("info|") + info + "|" + (on ? "1" : "0"));
}

void traded(u16 trader, u16 item, LPCSTR section, bool bought)
{
	if (applying || !section)
		return;
	std::string th = std::to_string(coop_creatures::host_id(trader));
	if (bought)
		outgoing.push_back("tbuy|" + th + "|" + section);
	else
	{
		std::string st = coop_items::item_state(item);
		if (!st.empty())
			outgoing.push_back("tsell|" + th + "|" + section + "|" + st);
	}
}

void talk_line(LPCSTR speaker, LPCSTR text, LPCSTR other)
{
	if (!g_actor || !text || !*text)
		return;
	outgoing.push_back("dlg|" + esc(g_actor->Name()) + "|" + esc(speaker) + "|" + esc(text) + "|" + esc(other));
}

void talk_options(LPCSTR other, LPCSTR list, u16 npc)
{
	if (!g_actor)
		return;
	outgoing.push_back("dlgopts|" + esc(g_actor->Name()) + "|" + esc(other) + "|" +
		(npc != u16(-1) ? std::to_string(coop_creatures::host_id(npc)) : std::string("")) + "|" + esc(list));
}

void talk_pick(LPCSTR id) { outgoing.push_back(std::string("dlgpick|") + esc(id)); }
void talk_switch(LPCSTR kind) { outgoing.push_back(std::string("dlgswitch|") + esc(kind)); }
void talk_end() { outgoing.push_back("dlgend"); }
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
	if (host)
		spawns_frame();
	else
		fresh.clear();
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
	if (kind == "spawn")
	{
		if (!host)
			coop_items::receive(host, line);
	}
	else if (kind == "sqm" && a.size() >= 11)
	{
		if (!host)
			call("coop_spawn.engine_sqm", atoi(a[1].c_str()), a[2].c_str(), atoi(a[3].c_str()), a[4].c_str(),
				(float)atof(a[5].c_str()), (float)atof(a[6].c_str()), (float)atof(a[7].c_str()), (int)strtoul(a[8].c_str(), nullptr, 10),
				(int)strtoul(a[9].c_str(), nullptr, 10), a[10].empty() ? -1 : atoi(a[10].c_str()));
	}
	else if (kind == "unreg" && a.size() >= 3)
	{
		if (!host)
			call("coop_spawn.engine_unreg", atoi(a[1].c_str()), a[2].c_str());
	}
	else if (kind == "info" && a.size() >= 3)
	{
		// the partner's story flag: this player's too
		shared_str id = a[1].c_str();
		bool on = a[2] == "1";
		if (id.size() && on != !!g_actor->HasInfo(id))
		{
			applying = true;
			g_actor->TransferInfo(id, on);
			applying = false;
		}
	}
	else if (kind == "tbuy" && a.size() >= 3)
		call("coop_objects.engine_tbuy", (int)coop_creatures::local_id_of((u16)atoi(a[1].c_str())), a[2].c_str());
	else if (kind == "tsell" && a.size() >= 4)
	{
		applying = true;
		coop_items::trader_copy(coop_creatures::local_id_of((u16)atoi(a[1].c_str())), a[2], a[3]);
		applying = false;
	}
	else if (kind == "dlg" && a.size() >= 5)
		call("coop_dialog.engine_line", unesc(a[1]).c_str(), unesc(a[2]).c_str(), unesc(a[3]).c_str(), unesc(a[4]).c_str());
	else if (kind == "dlgopts" && a.size() >= 5)
		call("coop_dialog.engine_options", unesc(a[1]).c_str(), unesc(a[2]).c_str(),
			a[3].empty() ? -1 : (int)coop_creatures::local_id_of((u16)atoi(a[3].c_str())), unesc(a[4]).c_str());
	else if (kind == "dlgpick" && a.size() >= 2)
		call("coop_dialog.engine_pick", unesc(a[1]).c_str());
	else if (kind == "dlgswitch" && a.size() >= 2)
		call("coop_dialog.engine_switch", unesc(a[1]).c_str());
	else if (kind == "dlgend")
		call("coop_dialog.engine_end");
	else if (kind == "census")
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
