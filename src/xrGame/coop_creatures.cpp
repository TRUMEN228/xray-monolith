////////////////////////////////////////////////////////////////////////////
//	Module 		: coop_creatures.cpp
//	Description : Anomaly Co-op: the host's creatures in the guest's game (see coop_creatures.h).
//	              A record: id, section crc, position, health, mental state, body, weapon strap, relation to the
//	              players, the enemy (another creature's id, the host or the guest), whether it is dead. Ids are the
//	              host's; the guest finds its copies through the scripts' id pairs (spawns made on both sides differ).
//	              One that dies on the host is sent on for a while: the guest's copy dies too, and its body is put
//	              where the host's came to rest (3 s and 12 s after, when it has stopped sliding).
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "ai_object_location.h"
#include "../xrServerEntities/xrServer_Objects_ALife_Monsters.h"
#include "coop_creatures.h"
#include "coop_items.h"
#include "Level.h"
#include "Actor.h"
#include "CustomMonster.h"
#include "InventoryOwner.h"
#include "ai/stalker/ai_stalker.h"
#include "stalker_movement_manager_smart_cover.h"
#include "script_game_object.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "movement_manager.h"
#include "detail_path_manager.h"
#include "ai_monster_space.h"

#include <map>

namespace coop_creatures
{
namespace
{
const float RADIUS = 150.f;     // as far as creatures come online: closer, the guest's own AI would have them elsewhere
const float JUMP = 12.f;        // a copy this far from the host's position is put there, not walked
const float SNAP = 3.f;         // a creature that becomes a copy is put in place when this far
const u32 HOLD_MS = 6500;       // without an update this long a copy goes back to its own AI
const u16 NONE = 0xffff, HOST = 0xfffe, GUEST = 0xfffd;
const u32 DEAD_SEND_MS = 15000; // a body's place goes on this long after the death
const u32 BODY_FIRST_MS = 3000, BODY_LAST_MS = 12000; // when the guest puts a body where the host's lies
const float BODY_OFF = 1.f;      // closer than this it stays where it fell
const u8 FLAG_DEAD = 1;

#pragma pack(push, 1)
struct Record
{
	u16 id;
	u32 sect;      // crc of the section: an id pair the scripts have not made yet must not drive another creature
	float x, y, z;
	u8 health;     // 0..255
	u8 mental;     // EMentalState, 255 not a stalker
	u8 body;       // 0 crouch, 1 stand, 255 not a stalker
	u8 strap;      // 1 the weapon on the back, 0 in the hands, 255 not a stalker
	u8 rel;        // 0 friend, 1 neutral, 2 enemy (to the host)
	u16 enemy;     // a creature's host id, HOST, GUEST or NONE
	u8 flags;      // FLAG_DEAD
	s16 yaw;       // r16: where it looks (a stalker's head, a monster's body), the engine's angles * 32767 / pi
	s16 pitch;
};
#pragma pack(pop)

std::map<u16, u16> h2g;           // host id -> id here
std::map<u16, u16> g2h;           // id here -> host id
u16 partner = NONE;               // the partner's puppet, an id here
std::map<u16, u32> copies;        // guest: copies driven now -> time of their last update
std::map<u16, u16> targets;       // guest: copy -> whom it fights (an id here)
std::map<u16, u32> sent_alive;    // host: creatures sent alive -> when last
std::map<u16, u32> died;          // host: those of them that died -> when
struct Body
{
	u32 since;
	int moves;
};
std::map<u16, Body> bodies;       // guest: copies that died by the host's word -> when, how often put in place

u16 local_id(u16 host_id)
{
	auto it = h2g.find(host_id);
	return it == h2g.end() ? host_id : it->second;
}

u32 sect_crc(const CObject* o)
{
	LPCSTR s = o->cNameSect().c_str();
	return s ? crc32(s, xr_strlen(s)) : 0;
}

void release(u16 id)
{
	if (CCustomMonster* m = smart_cast<CCustomMonster*>(Level().Objects.net_Find(id)))
		m->net_puppet(false, 0);
	copies.erase(id);
	targets.erase(id);
}
} // namespace

void collect(std::string& out, u32 now)
{
	out.clear();
	if (!g_pGameLevel || !g_actor)
		return;
	u32 t = Device.dwTimeGlobal;
	Fvector me = g_actor->Position();
	CObject* p = partner != NONE ? Level().Objects.net_Find(partner) : nullptr;
	Fvector them = p ? p->Position() : me;
	out.append((const char*)&now, 4);
	CScriptGameObject* actor_lua = g_actor->lua_game_object();
	for (u32 i = 0, n = Level().Objects.o_count(); i < n; i++)
	{
		CObject* o = Level().Objects.o_get_by_iterator(i);
		CCustomMonster* m = smart_cast<CCustomMonster*>(o);
		if (!m || m->getDestroy())
			continue;
		bool alive = !!m->g_Alive();
		if (!alive)
		{
			// a body: only one sent alive before, and only for a while after its death
			auto d = died.find(m->ID());
			if (d == died.end())
			{
				if (sent_alive.find(m->ID()) == sent_alive.end())
					continue;
				d = died.emplace(m->ID(), t).first;
				coop_items::died(m->ID());
				sent_alive.erase(m->ID());
			}
			if (t - d->second > DEAD_SEND_MS)
				continue;
		}
		const Fvector& pos = m->Position();
		if (alive && pos.distance_to(me) > RADIUS && pos.distance_to(them) > RADIUS)
			continue;
		Record r;
		r.flags = alive ? 0 : FLAG_DEAD;
		r.id = m->ID();
		r.sect = sect_crc(m);
		r.x = pos.x;
		r.y = pos.y;
		r.z = pos.z;
		r.health = (u8)iFloor(clampr(m->GetfHealth(), 0.f, 1.f) * 255.f + 0.5f);
		r.mental = r.body = r.strap = 255;
		r.rel = 2;
		r.enemy = NONE;
		// where it looks: a stalker's head, a monster's body
		{
			float yaw = m->movement().m_body.current.yaw, pitch = 0.f;
			if (CAI_Stalker* s = smart_cast<CAI_Stalker*>(m))
			{
				yaw = s->movement().head_orientation().current.yaw;
				pitch = s->movement().head_orientation().current.pitch;
			}
			r.yaw = (s16)iFloor(angle_normalize_signed(yaw) / PI * 32767.f);
			r.pitch = (s16)iFloor(angle_normalize_signed(pitch) / PI * 32767.f);
		}
		if (!alive)
		{
			out.append((const char*)&r, sizeof(r));
			continue;
		}
		sent_alive[m->ID()] = t;
		CScriptGameObject* lua = m->lua_game_object();
		if (smart_cast<CAI_Stalker*>(m))
		{
			r.mental = (u8)lua->mental_state();
			r.body = lua->body_state() == MonsterSpace::eBodyStateCrouch ? 0 : 1;
			r.strap = lua->weapon_strapped() ? 1 : 0;
			ALife::ERelationType rel = lua->GetRelationType(actor_lua);
			r.rel = rel == ALife::eRelationTypeFriend ? 0 : (rel == ALife::eRelationTypeEnemy ? 2 : 1);
		}
		if (const CEntityAlive* e = m->memory().enemy().selected())
			r.enemy = e->ID() == g_actor->ID() ? HOST : (e->ID() == partner ? GUEST : e->ID());
		out.append((const char*)&r, sizeof(r));
	}
	// the gone ones are forgotten now and then
	for (auto it = sent_alive.begin(); it != sent_alive.end();)
		it = t - it->second > 60000 ? sent_alive.erase(it) : ++it;
	for (auto it = died.begin(); it != died.end();)
		it = t - it->second > 60000 ? died.erase(it) : ++it;
}

void apply(const std::string& payload)
{
	if (!g_pGameLevel || !g_actor || payload.size() < 4)
		return;
	u32 stamp = *(const u32*)payload.data();
	u32 now = Device.dwTimeGlobal;
	CScriptGameObject* actor_lua = g_actor->lua_game_object();
	for (size_t off = 4; off + sizeof(Record) <= payload.size(); off += sizeof(Record))
	{
		const Record& r = *(const Record*)(payload.data() + off);
		u16 id = local_id(r.id);
		CCustomMonster* m = smart_cast<CCustomMonster*>(Level().Objects.net_Find(id));
		if (!m || m->getDestroy() || sect_crc(m) != r.sect)
			continue;
		Fvector pos = {r.x, r.y, r.z};
		if (r.flags & FLAG_DEAD)
		{
			if (m->g_Alive())
			{
				// dead on the host: dead here, by the host's hand (no credit to this player)
				release(id);
				m->KillEntity(partner != NONE ? partner : id);
				bodies[id] = {now, 0};
			}
			else
			{
				// the body where the host's came to rest, once it has stopped sliding (twice: it may still slide)
				auto b = bodies.find(id);
				if (b == bodies.end())
					b = bodies.emplace(id, Body{now, 0}).first;
				u32 wait = b->second.moves == 0 ? BODY_FIRST_MS : BODY_LAST_MS;
				if (b->second.moves < 2 && now - b->second.since >= wait)
				{
					b->second.moves++;
					if (m->Position().distance_to(pos) > BODY_OFF)
						m->lua_game_object()->ForceSetPosition(pos, false);
				}
			}
			continue;
		}
		if (!m->g_Alive())
			continue;
		bool driven = m->net_puppet_active();
		// one far from the host's position (its own AI took it elsewhere, a load, a jump) is put there first
		if (m->Position().distance_to(pos) > (driven ? JUMP : SNAP))
		{
			Fmatrix F = m->XFORM();
			F.c = pos;
			m->movement().detail().make_inactual();
			if (m->animation_movement_controlled())
				m->destroy_anim_mov_ctrl();
			m->ForceTransform(F);
		}
		if (!driven)
			m->net_puppet(true, 0);
		m->net_puppet_push(pos, stamp);
		// the host's look (the engine's yaw and pitch are the negated heading and pitch of a direction)
		{
			Fvector look;
			look.setHP(-(r.yaw * PI / 32767.f), -(r.pitch * PI / 32767.f));
			m->net_puppet_set_look(look);
		}
		copies[id] = now;
		// hurt on the host: hurt here (a health that went up comes with the creature's own healing)
		float h = r.health / 255.f;
		if (h < m->GetfHealth() - 0.05f)
			m->SetfHealth(h);
		if (r.mental != 255)
		{
			m->net_puppet_set_mental(r.mental);
			m->net_puppet_set_body(r.body);
			m->net_puppet_set_strap(r.strap);
			// whoever it is hostile to on the host, it is hostile to the whole team
			CScriptGameObject* lua = m->lua_game_object();
			if (r.rel == 2 && lua->GetRelationType(actor_lua) != ALife::eRelationTypeEnemy)
				lua->SetRelation(ALife::eRelationTypeEnemy, actor_lua);
		}
		if (r.enemy == NONE)
			targets.erase(id);
		else
			targets[id] = r.enemy == HOST ? partner : (r.enemy == GUEST ? g_actor->ID() : local_id(r.enemy));
	}
}

void update(u32 now)
{
	if (!g_pGameLevel || !g_actor)
	{
		copies.clear();
		targets.clear();
		bodies.clear();
		return;
	}
	for (auto it = bodies.begin(); it != bodies.end();)
		it = now - it->second.since > 60000 ? bodies.erase(it) : ++it;
	for (auto it = copies.begin(); it != copies.end();)
	{
		u16 id = it->first;
		CCustomMonster* m = smart_cast<CCustomMonster*>(Level().Objects.net_Find(id));
		// a dead body falls by itself; one the host no longer sends goes back to its own AI
		bool gone = !m || !m->g_Alive() || now - it->second > HOLD_MS;
		++it;
		if (gone)
			release(id);
	}
}

void release_all()
{
	while (!copies.empty())
		release(copies.begin()->first);
}

void map(u16 host_id, u16 id)
{
	h2g[host_id] = id;
	g2h[id] = host_id;
}
void unmap(u16 host_id)
{
	auto it = h2g.find(host_id);
	if (it != h2g.end())
		g2h.erase(it->second);
	h2g.erase(host_id);
}
void clear_map()
{
	h2g.clear();
	g2h.clear();
}
u16 local_id_of(u16 hid) { return local_id(hid); }
u16 host_id(u16 id)
{
	auto it = g2h.find(id);
	return it == g2h.end() ? id : it->second;
}
void set_partner(u16 id) { partner = id; }
u16 partner_id() { return partner; }

// the mirror's list: host id:section:x:y:z:level vertex:game vertex:alive:squad:squad section:smart, as coop_mirror's
// own list was (r16: the engine sends it)
bool census(std::string& out, u32 now)
{
	static u32 next = 0;
	const u32 CENSUS_MS = 1500;
	const float CENSUS_RADIUS = 130.f;
	if (!g_actor || !ai().get_alife() || now < next)
		return false;
	next = now + CENSUS_MS;
	Fvector me = g_actor->Position(), them = me;
	CObject* p = partner != NONE ? Level().Objects.net_Find(partner) : nullptr;
	if (p)
		them = p->Position();
	LPCSTR lname = Level().name().c_str();
	char head[160];
	xr_sprintf(head, "census|%u|%s|%.1f|%.1f|%.1f", now, lname ? lname : "", me.x, me.y, me.z);
	out = head;
	for (u32 i = 0, n = Level().Objects.o_count(); i < n; i++)
	{
		CObject* o = Level().Objects.o_get_by_iterator(i);
		CCustomMonster* m = smart_cast<CCustomMonster*>(o);
		// (a psy dog's phantoms live seconds and each game makes its own)
		if (!m || o->ID() == partner || o->getDestroy() || strstr(o->cNameSect().c_str(), "phantom"))
			continue;
		const Fvector& pos = o->Position();
		if (pos.distance_to(me) >= CENSUS_RADIUS && pos.distance_to(them) >= CENSUS_RADIUS)
			continue;
		u16 sq = NONE, smart = NONE;
		std::string sq_sec;
		CSE_ALifeMonsterAbstract* se = smart_cast<CSE_ALifeMonsterAbstract*>(ai().alife().objects().object(o->ID(), true));
		if (se && m->g_Alive())
		{
			sq = se->m_group_id;
			smart = se->m_smart_terrain_id;
			CSE_Abstract* squad = sq != NONE ? ai().alife().objects().object(sq, true) : nullptr;
			if (squad)
				sq_sec = squad->name();
			else
				sq = NONE;
		}
		// the look and the name (a new copy picks its own at random from the section's profile)
		std::string look = o->cNameVisual().c_str() ? o->cNameVisual().c_str() : "";
		std::string name;
		if (CInventoryOwner* io = smart_cast<CInventoryOwner*>(o))
			name = io->Name() ? io->Name() : "";
		for (auto* s : {&look, &name})
			for (char& c : *s)
				if (c == ':' || c == '|' || c == ';')
					c = ' ';
		char e[256];
		xr_sprintf(e, "|%u:%s:%.1f:%.1f:%.1f:%u:%u:%d:%s:%s:%s:", o->ID(), o->cNameSect().c_str(), pos.x, pos.y, pos.z,
			m->ai_location().level_vertex_id(), (u32)m->ai_location().game_vertex_id(), m->g_Alive() ? 1 : 0,
			sq != NONE ? std::to_string(sq).c_str() : "", sq_sec.c_str(), smart != NONE ? std::to_string(smart).c_str() : "");
		out += e;
		out += look + ":" + name;
	}
	return true;
}

u16 target_of(u16 id)
{
	auto it = targets.find(id);
	return it == targets.end() ? NONE : it->second;
}
} // namespace coop_creatures
