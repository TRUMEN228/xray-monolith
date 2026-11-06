////////////////////////////////////////////////////////////////////////////
//	Module 		: coop_partner.cpp
//	Description : Anomaly Co-op: the partner's puppet and the players' hits in the engine (r15).
//	              The line: level|x|y|z|yaw|level vertex|game vertex|time|movement state|ax:ay:az|lx:ly:lz
//	              <tab> the scripts' part. A hit: host id|health lost|1 when it died
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "coop_partner.h"
#include "coop_creatures.h"
#include "Level.h"
#include "Actor.h"
#include "CustomMonster.h"
#include "entity_alive.h"
#include "ai_space.h"
#include "script_engine.h"
#include "ai_object_location.h"

namespace coop_partner
{
namespace
{
const u32 SEND_MS = 50;
const u16 NONE = u16(-1);

u32 next_send = 0;
std::string extras;
std::vector<std::string> hits_out;

std::string f2(float v)
{
	char b[32];
	xr_sprintf(b, "%.2f", v);
	return b;
}

std::string f4(float v)
{
	char b[32];
	xr_sprintf(b, "%.4f", v);
	return b;
}

std::string level_now()
{
	if (!g_pGameLevel)
		return "";
	LPCSTR n = Level().name().c_str();
	return n ? n : "";
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

bool vec3(const std::string& s, Fvector& v)
{
	return sscanf(s.c_str(), "%f:%f:%f", &v.x, &v.y, &v.z) == 3;
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
} // namespace

void set_extras(const std::string& e) { extras = e; }

bool collect(std::string& out, u32 now)
{
	if (!g_actor || now < next_send)
		return false;
	next_send = now + SEND_MS;
	CActor* a = g_actor;
	const Fvector& p = a->Position();
	Fvector look = Device.vCameraDirection;
	Fvector ang = a->coop_net_angles();
	out = level_now() + "|" + f2(p.x) + "|" + f2(p.y) + "|" + f2(p.z) + "|" + f4(look.getH()) + "|" +
		std::to_string(a->ai_location().level_vertex_id()) + "|" + std::to_string(a->ai_location().game_vertex_id()) + "|" +
		std::to_string(Device.dwTimeGlobal) + "|" + std::to_string(a->coop_net_mstate()) + "|" + f4(ang.x) + ":" +
		f4(ang.y) + ":" + f4(ang.z) + "|" + f4(look.x) + ":" + f4(look.y) + ":" + f4(look.z) + "\t" + extras;
	return true;
}

void apply(const std::string& line)
{
	if (!g_actor)
		return;
	size_t tab = line.find('\t');
	std::string motion = line.substr(0, tab);
	std::string rest = tab == std::string::npos ? "" : line.substr(tab + 1);
	auto m = split(motion, '|');
	if (m.size() < 11)
		return;
	// the puppet moves at once (on this level)
	u16 id = coop_creatures::partner_id();
	CObject* o = id != NONE && m[0] == level_now() ? Level().Objects.net_Find(id) : nullptr;
	if (o)
	{
		Fvector p = {(float)atof(m[1].c_str()), (float)atof(m[2].c_str()), (float)atof(m[3].c_str())};
		u32 stamp = (u32)strtoul(m[7].c_str(), nullptr, 10);
		Fvector ang, look;
		if (CActor* actor = smart_cast<CActor*>(o))
		{
			if (actor->coop_net_actor())
			{
				actor->coop_net_push(p, stamp);
				if (vec3(m[9], ang))
					actor->coop_net_set_state((u32)strtoul(m[8].c_str(), nullptr, 10), ang);
			}
		}
		else if (CCustomMonster* npc = smart_cast<CCustomMonster*>(o))
		{
			if (npc->net_puppet_active())
			{
				npc->net_puppet_push(p, stamp);
				if (vec3(m[10], look) && look.magnitude() > 0.5f)
					npc->net_puppet_set_look(look);
			}
		}
	}
	// the rest is the scripts' (the weapon, the outfit, shots, notes)
	call("coop_player.engine_partner", motion.c_str(), rest.c_str());
}

void actor_hit(CEntityAlive* victim, float lost, bool died)
{
	if (!victim || !smart_cast<CCustomMonster*>(victim) || victim->ID() == coop_creatures::partner_id())
		return;
	if (lost <= 0.0005f && !died)
		return;
	hits_out.push_back(std::to_string(coop_creatures::host_id(victim->ID())) + "|" + f4(lost) + "|" + (died ? "1" : "0"));
	Msg("~ coop hit out: %s (here %u)", hits_out.back().c_str(), victim->ID());
}

void hits(bool host, bool linked, std::vector<std::string>& out)
{
	// (the host's own hits reach the guest with the creatures' health and deaths)
	if (!host && linked)
		for (auto& h : hits_out)
			out.push_back("hit|" + h);
	else if (!hits_out.empty())
		Msg("~ coop hits not sent: %u (host %d, linked %d)", (u32)hits_out.size(), host ? 1 : 0, linked ? 1 : 0);
	hits_out.clear();
}

void hit_in(const std::string& line)
{
	auto a = split(line, '|');
	if (a.size() < 3 || !g_pGameLevel)
		return;
	u16 id = (u16)atoi(a[0].c_str());
	CEntityAlive* e = smart_cast<CEntityAlive*>(Level().Objects.net_Find(id));
	Msg("~ coop hit in: %s (%s)", line.c_str(), !e ? "not here" : (e->g_Alive() ? "alive" : "dead"));
	if (!e || !e->g_Alive())
		return;
	float h = e->GetfHealth() - (float)atof(a[1].c_str());
	u16 puppet = coop_creatures::partner_id();
	if (a[2] == "1" || h <= 0.005f)
		e->KillEntity(puppet != NONE ? puppet : id);
	else
		e->SetfHealth(h);
	call("coop_world.engine_partner_hit", (int)id);
}
} // namespace coop_partner
