////////////////////////////////////////////////////////////////////////////
//	Module 		: coop_items.cpp
//	Description : Anomaly Co-op: items between the two games (see coop_items.h).
//	              Lines between the games (ids are the host's; the guest translates them with the scripts' pairs):
//	                take|item|section|x|y|z           up from the ground: the partner's copy goes
//	                drop|section|cond|ammo|x|y|z|lv|gv down to the ground: the partner gets one there
//	                btake|box|item|section|seq        out of a box or a body
//	                bput|box|section|cond|ammo|seq    into a box or a body
//	                bset|box|acked|sec:cond:ammo;...  host: what a box holds after a change (the guest's box is made so)
//	                bcond|item|section|cond           host: the state of what the guest took (each game rolls loot)
//	                bdeny|item|section                host: both took the same thing at once, the guest gives it up
//	                loot|body|section|sec:cond:ammo;... host: a body's things (each game rolls its own)
//	                throw|section|x|y|z|vx|vy|vz|n   a bolt or a grenade thrown: the partner's copy flies the same way
//	                land|n|x|y|z                     the thrower's bolt came to rest there: the copy is put there too
//	                                                 (the flights part after a hit: the partner's puppet is not where
//	                                                 the partner is, the physics differ)
//	                                                 (a grenade's copy goes before it would go off: the blast
//	                                                 comes from the thrower's game)
//	              The guest numbers its box changes (seq); a list from the host that does not have the guest's last
//	              change yet is not applied (the next one will have it).
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "coop_items.h"
#include "coop_creatures.h"
#include "Level.h"
#include "Actor.h"
#include "InventoryBox.h"
#include "inventory_item.h"
#include "InventoryOwner.h"
#include "Inventory.h"
#include "WeaponAmmo.h"
#include "ai_space.h"
#include "script_engine.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "ai_object_location.h"
#include "level_graph.h"
#include "../xrServerEntities/xrServer_Objects_ALife.h"
#include "PhysicsShellHolder.h"
#include "../xrphysics/PhysicsShell.h"
#include "Grenade.h"
#include "xrServer.h"
#include "../xrServerEntities/xrMessages.h"

#include <map>
#include <set>

// the scripts' alife():release: an object in the game is destroyed through the game (a plain release would only
// forget it on the server's side and leave it in the world)
void CALifeSimulator__release(CALifeSimulator* self, CSE_Abstract* object, bool);

namespace coop_items
{
namespace
{
const u32 SNAP_MS = 300;        // host: a box's list goes this long after its last change
const u32 LOOT_MS = 3000;       // host: a body's loot goes once death_manager has filled it
const u32 KEEP_FRAMES = 10;     // an item's last move counts for the next one this many frames
const float GROUND_RADIUS = 4.f;
const u32 THROW_WAIT_MS = 2000;     // a thrown copy is looked for this long after it was made
const u32 GRENADE_LIFE_MS = 3000;   // a grenade's copy goes then (it would not go off: it is no live one)
const u32 STILL_MS = 500;           // a thrown thing this long without moving has come to rest
const u32 FLIGHT_MS = 15000;        // a throw is followed this long at most

struct Throw
{
	std::string section;
	Fvector from, velocity;
	u32 at;
	u32 n;
};
std::vector<Throw> throws;          // copies of the partner's throws on their way into the world
struct Flight
{
	u16 id;
	u32 n, at, still_since;
};
std::vector<Flight> flights;        // this player's thrown bolts until they come to rest
u32 throw_n = 0;
std::map<u32, std::pair<u16, u32>> copies; // the partner's throw -> its copy here, when made
std::map<u16, u32> grenades;        // grenade copies -> when they go

struct Left
{
	u16 from;
	u32 frame;
};
std::map<u16, Left> last_left;              // item -> what it left last, when
std::map<u16, u32> drops;                   // the player's items let go of: checked a frame later (ground or box)
std::vector<std::string> outgoing;
std::map<u16, u32> snap_at;                 // host: box -> when its list goes
std::map<u16, u32> loot_at;                 // host: body -> when its loot goes
std::set<u16> ignored;
u32 paused_until = 0;
bool applying = false;
u32 box_seq = 0, box_acked = 0;
bool shared = true;                         // boxes and bodies shared; off: each player has the same, its own
std::function<void(const std::string&)> note_fn;

// appdata\coop_items.txt on a test install: every line in and out (the engine's own log is lost when a game is killed)
void trace(const char* dir, const std::string& line, const char* extra = "")
{
	static FILE* f = nullptr;
	static bool checked = false;
	if (!checked)
	{
		// only on a test install (appdata\coop_debug.txt)
		checked = true;
		string_path dbg, p;
		FS.update_path(dbg, "$app_data_root$", "coop_debug.txt");
		FS.update_path(p, "$app_data_root$", "coop_items.txt");
		if (GetFileAttributesA(dbg) != INVALID_FILE_ATTRIBUTES)
			f = fopen(p, "w");
	}
	if (f)
	{
		fprintf(f, "[%u] %s %s%s\n", Device.dwTimeGlobal, dir, line.c_str(), extra);
		fflush(f);
	}
}

std::vector<std::string> split(const std::string& s, char c)
{
	std::vector<std::string> out;
	size_t a = 0;
	for (;;)
	{
		size_t b = s.find(c, a);
		out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
		if (b == std::string::npos)
			return out;
		a = b + 1;
	}
}

std::string f2(float v)
{
	string32 b;
	xr_sprintf(b, "%.2f", v);
	return b;
}

std::string f3(float v)
{
	string32 b;
	xr_sprintf(b, "%.3f", v);
	return b;
}

bool is_actor(CObject* o) { return o && g_actor && o->ID() == g_actor->ID(); }

// a box, or a body (whose things are looted as from a box)
bool container(CObject* o)
{
	if (!o || ignored.count(o->ID()))
		return false;
	if (smart_cast<CInventoryBox*>(o))
		return true;
	CEntityAlive* e = smart_cast<CEntityAlive*>(o);
	return e && !e->g_Alive() && smart_cast<CInventoryOwner*>(o) && !smart_cast<CActor*>(o);
}

u32 last_frame = 0;               // the session's last frame: without one no session runs
bool off() { return Device.dwTimeGlobal - last_frame > 2000; }
bool quiet() { return applying || Device.dwTimeGlobal < paused_until; }

std::string props(CInventoryItem* item)
{
	int ammo = 0;
	if (CWeaponAmmo* a = smart_cast<CWeaponAmmo*>(item))
		ammo = a->m_boxCurr;
	return f3(item->GetCondition()) + "|" + std::to_string(ammo);
}

std::string sect(CObject* o) { return o->cNameSect().c_str(); }

// the items a box or a body holds
void items_of(CObject* o, xr_vector<CInventoryItem*>& out)
{
	out.clear();
	if (CInventoryBox* box = smart_cast<CInventoryBox*>(o))
	{
		for (u16 id : box->m_items)
			if (CInventoryItem* it = smart_cast<CInventoryItem*>(Level().Objects.net_Find(id)))
				out.push_back(it);
	}
	else if (CInventoryOwner* owner = smart_cast<CInventoryOwner*>(o))
	{
		for (PIItem it : owner->inventory().m_all)
			out.push_back(it);
	}
}

void release(u16 id)
{
	if (!ai().get_alife())
		return;
	CSE_Abstract* se = ai().alife().objects().object(id, true);
	string128 b;
	xr_sprintf(b, "release %u: %s, parent %u", id, se ? se->name() : "no server object", se ? se->ID_Parent : 0xffff);
	trace("-", b);
	applying = true;
	if (se)
		CALifeSimulator__release(const_cast<CALifeSimulator*>(&ai().alife()), se, true);
	else if (Level().Objects.net_Find(id))
	{
		// one the game has and the server not (an NPC's bolt): destroyed through the game all the same
		NET_Packet packet;
		packet.w_begin(M_EVENT);
		packet.w_u32(Level().timeServer());
		packet.w_u16(GE_DESTROY);
		packet.w_u16(id);
		Level().Send(packet, net_flags(TRUE, TRUE));
	}
	applying = false;
}

// a new item through the scripts' helper (it sets the condition and the rounds as the game does)
void spawn(const std::string& section, u16 owner, const std::string& cond, const std::string& ammo, const Fvector* pos = nullptr,
	u32 lv = u32(-1), u32 gv = u32(-1))
{
	if (!pSettings->section_exist(section.c_str()))
		return;
	try
	{
		luabind::functor<void> f;
		if (ai().script_engine().functor("coop_world.engine_spawn", f))
		{
			applying = true;
			f(section.c_str(), (int)(owner == u16(-1) ? -1 : owner), (float)atof(cond.c_str()), atoi(ammo.c_str()),
				pos ? pos->x : 0.f, pos ? pos->y : 0.f, pos ? pos->z : 0.f, (int)lv, (int)gv);
			applying = false;
		}
	}
	catch (...)
	{
		applying = false;
		Msg("! coop items: spawning %s failed", section.c_str());
	}
}

// an open box or body shows the partner's change (Anomaly's loot window is the scripts')
void refresh(u16 id)
{
	try
	{
		luabind::functor<void> f;
		if (ai().script_engine().functor("coop_world.engine_refresh", f))
			f((int)id);
	}
	catch (...)
	{
	}
}

void say(const std::string& s)
{
	if (note_fn)
		note_fn(s);
}

u16 host_id(u16 id) { return coop_creatures::host_id(id); }
u16 here_id(u16 hid) { return coop_creatures::local_id_of(hid); }

CObject* find(u16 id) { return Level().Objects.net_Find(id); }

// an item with this id and section, or else the first of this section, in a box or a body
CInventoryItem* in_container(CObject* box, u16 id, const std::string& section)
{
	xr_vector<CInventoryItem*> list;
	items_of(box, list);
	for (CInventoryItem* it : list)
		if (it->object().ID() == id && sect(&it->object()) == section)
			return it;
	for (CInventoryItem* it : list)
		if (sect(&it->object()) == section)
			return it;
	return nullptr;
}

// the player's own item of this id and section (it took the same thing a moment before the word came)
CInventoryItem* on_player(u16 id, const std::string& section)
{
	CObject* o = find(id);
	CInventoryItem* it = smart_cast<CInventoryItem*>(o);
	if (it && o->H_Parent() && is_actor(o->H_Parent()) && sect(o) == section)
		return it;
	return nullptr;
}

CObject* ground_item(u16 id, const std::string& section, const Fvector& pos)
{
	CObject* o = find(id);
	if (o && !o->H_Parent() && smart_cast<CInventoryItem*>(o) && sect(o) == section)
		return o;
	CObject* best = nullptr;
	float bd = GROUND_RADIUS;
	for (u32 i = 0, n = Level().Objects.o_count(); i < n; i++)
	{
		CObject* c = Level().Objects.o_get_by_iterator(i);
		if (!c || c->H_Parent() || !smart_cast<CInventoryItem*>(c) || sect(c) != section)
			continue;
		float d = c->Position().distance_to(pos);
		if (d < bd)
			best = c, bd = d;
	}
	return best;
}
} // namespace

void set_note(std::function<void(const std::string&)> note) { note_fn = note; }
void ignore_box(u16 id, bool ignore) { if (ignore) ignored.insert(id); else ignored.erase(id); }
void pause(u32 ms) { paused_until = Device.dwTimeGlobal + ms; }

namespace
{
std::string hex(const u8* b, u32 n)
{
	static const char* d = "0123456789abcdef";
	std::string r;
	r.reserve(n * 2);
	for (u32 i = 0; i < n; i++)
	{
		r += d[b[i] >> 4];
		r += d[b[i] & 15];
	}
	return r;
}

u32 unhex(const std::string& s, u8* out, u32 max)
{
	u32 n = 0;
	for (size_t i = 0; i + 1 < s.size() && n < max; i += 2)
	{
		auto v = [](char c) { return c >= 'a' ? c - 'a' + 10 : (c >= 'A' ? c - 'A' + 10 : c - '0'); };
		out[n++] = (u8)(v(s[i]) << 4 | v(s[i + 1]));
	}
	return n;
}

// the partner's item: made in this player's inventory from its whole state (condition, ammo, addons, upgrades)
void make_gift(const std::string& section, const std::string& state)
{
	if (!g_actor || !ai().get_alife() || !pSettings->section_exist(section.c_str()))
		return;
	CALifeSimulator& sim = const_cast<CALifeSimulator&>(ai().alife());
	const Fvector& p = g_actor->Position();
	u32 lv = g_actor->ai_location().level_vertex_id();
	GameGraph::_GRAPH_ID gv = g_actor->ai_location().game_vertex_id();
	CSE_Abstract* item = sim.spawn_item(section.c_str(), p, lv, gv, g_actor->ID(), false);
	if (!item)
		return;
	static u8 buf[NET_PacketSizeLimit];
	u32 n = unhex(state, buf, sizeof(buf));
	u16 id = item->ID;
	if (n > 4)
	{
		NET_Packet in;
		in.B.count = 0;
		in.w(buf, n);
		in.r_seek(0);
		u16 state_size = 0;
		in.r_u16(state_size);
		IPureServerObject* s = smart_cast<IPureServerObject*>(item);
		if (s)
		{
			s->STATE_Read(in, state_size);
			s->UPDATE_Read(in);
		}
		// (this game's own: where it is, whose it is, no story of the partner's world)
		item->ID = id;
		item->ID_Parent = g_actor->ID();
		item->o_Position = p;
		if (CSE_ALifeObject* o = smart_cast<CSE_ALifeObject*>(item))
		{
			o->m_tNodeID = lv;
			o->m_tGraphID = gv;
			o->m_story_id = ALife::_STORY_ID(-1);
		}
	}
	NET_Packet packet;
	packet.w_begin(M_SPAWN);
	packet.w_stringZ(section.c_str());
	item->Spawn_Write(packet, FALSE);
	sim.server().FreeID(item->ID, 0);
	F_entity_Destroy(item);
	ClientID client;
	client.set(0xffff);
	u16 dummy;
	packet.r_begin(dummy);
	applying = true;
	sim.server().Process_spawn(packet, client);
	applying = false;
}
} // namespace

bool gift(u16 id)
{
	if (!ai().get_alife())
		return false;
	CSE_Abstract* se = ai().alife().objects().object(id, true);
	IPureServerObject* s = smart_cast<IPureServerObject*>(se);
	if (!se || !s)
		return false;
	NET_Packet state, update, both;
	// (a packet's write position is not set by its constructor)
	state.B.count = update.B.count = both.B.count = 0;
	s->STATE_Write(state);
	s->UPDATE_Write(update);
	both.w_u16((u16)state.B.count);
	both.w(state.B.data, state.B.count);
	both.w(update.B.data, update.B.count);
	outgoing.push_back("gift|" + std::string(se->name()) + "|" + hex(both.B.data, both.B.count));
	return true;
}
void set_shared(bool on) { shared = on; }

void left(CObject* owner, CInventoryItem* item, bool destroyed)
{
	if (off() || !g_actor || !owner || !item)
		return;
	u16 id = item->object().ID();
	last_left[id] = {owner->ID(), Device.dwFrame};
	if (container(owner))
		snap_at[owner->ID()] = Device.dwTimeGlobal + SNAP_MS;
	if (is_actor(owner) && !destroyed && !quiet())
		drops[id] = Device.dwFrame;
}

void entered(CObject* owner, CInventoryItem* item)
{
	if (off() || !g_actor || !owner || !item)
		return;
	CObject* o = &item->object();
	u16 id = o->ID();
	if (container(owner))
		snap_at[owner->ID()] = Device.dwTimeGlobal + SNAP_MS;
	if (quiet())
		return;
	auto lf = last_left.find(id);
	bool recent = lf != last_left.end() && Device.dwFrame - lf->second.frame <= KEEP_FRAMES;
	if (is_actor(owner))
	{
		CObject* from = recent ? find(lf->second.from) : nullptr;
		if (from && container(from))
		{
			if (!shared)
				return; // (each player's own loot: a take from a box or a body is this player's alone)
			// out of a box or a body
			outgoing.push_back("btake|" + std::to_string(host_id(from->ID())) + "|" + std::to_string(host_id(id)) + "|" + sect(o) +
				"|" + std::to_string(++box_seq));
		}
		else if (!recent && Device.dwFrame - item->object().spawn_time() > 5)
		{
			// up from the ground (one just made - a gift, a reward, the scripts' own - is no take)
			const Fvector& p = g_actor->Position();
			outgoing.push_back("take|" + std::to_string(host_id(id)) + "|" + sect(o) + "|" + f2(p.x) + "|" + f2(p.y) + "|" + f2(p.z));
		}
	}
	else if (shared && container(owner) && recent && is_actor(find(lf->second.from)))
	{
		// into a box or a body
		drops.erase(id);
		outgoing.push_back("bput|" + std::to_string(host_id(owner->ID())) + "|" + sect(o) + "|" + props(item) + "|" +
			std::to_string(++box_seq));
	}
}

void died(u16 id) { loot_at[id] = Device.dwTimeGlobal + LOOT_MS; }

void thrown(CObject* missile, const Fvector& from, const Fvector& v)
{
	if (off() || !missile)
		return;
	u32 n = ++throw_n;
	outgoing.push_back("throw|" + sect(missile) + "|" + f2(from.x) + "|" + f2(from.y) + "|" + f2(from.z) + "|" + f2(v.x) + "|" +
		f2(v.y) + "|" + f2(v.z) + "|" + std::to_string(n));
	if (!smart_cast<CGrenade*>(missile))
		flights.push_back({missile->ID(), n, Device.dwTimeGlobal, 0});
}

namespace
{
// the partner's thrown things, once in the world: they fly as the partner's did
void launch(u32 now)
{
	for (auto it = throws.begin(); it != throws.end();)
	{
		CObject* found = nullptr;
		for (u32 i = 0, n = Level().Objects.o_count(); i < n && !found; i++)
		{
			CObject* o = Level().Objects.o_get_by_iterator(i);
			CGameObject* go = smart_cast<CGameObject*>(o);
			if (go && !o->H_Parent() && sect(o) == it->section && Device.dwFrame - go->spawn_time() < 60 &&
				o->Position().distance_to(it->from) < 1.5f)
				found = o;
		}
		CPhysicsShellHolder* h = smart_cast<CPhysicsShellHolder*>(found);
		if (h && h->PPhysicsShell())
		{
			CPhysicsShell* shell = h->PPhysicsShell();
			// as the thrower's own throw (CMissile::activate_physic_shell): no air drag, and the thrower - here the
			// partner's puppet, whose head the copy starts in - is not hit until the copy is out of it
			shell->SetAllGeomTraced();
			shell->SetAirResistance(0.f, 0.f);
			shell->set_DynamicScales(1.f, 1.f);
			CObject* thrower = coop_creatures::partner_id() != u16(-1) ?
				Level().Objects.net_Find(coop_creatures::partner_id()) : nullptr;
			if (smart_cast<CPhysicsShellHolder*>(thrower))
			{
				shell->add_ObjectContactCallback(CMissile::ExitContactCallback);
				shell->set_CallbackData(smart_cast<CPhysicsShellHolder*>(thrower));
			}
			shell->set_LinearVel(it->velocity);
			trace("~", "launched " + it->section, "");
			if (it->n)
				copies[it->n] = {found->ID(), now};
			if (smart_cast<CGrenade*>(found))
				grenades[found->ID()] = now + GRENADE_LIFE_MS;
			it = throws.erase(it);
		}
		else if (now - it->at > THROW_WAIT_MS)
		{
			trace("~", "no copy of a throw: " + it->section);
			it = throws.erase(it);
		}
		else
			++it;
	}
	// this player's bolts: where each came to rest goes to the partner
	for (auto it = flights.begin(); it != flights.end();)
	{
		CObject* o = find(it->id);
		CPhysicsShellHolder* h = smart_cast<CPhysicsShellHolder*>(o);
		if (!h || o->H_Parent() || now - it->at > FLIGHT_MS)
		{
			it = flights.erase(it);
			continue;
		}
		Fvector v;
		v.set(0.f, 0.f, 0.f);
		if (h->PPhysicsShell() && h->PPhysicsShell()->isActive())
			h->PPhysicsShell()->get_LinearVel(v);
		if (v.magnitude() > 0.05f)
			it->still_since = 0;
		else if (!it->still_since)
			it->still_since = now;
		else if (now - it->still_since > STILL_MS)
		{
			const Fvector& p = o->Position();
			outgoing.push_back("land|" + std::to_string(it->n) + "|" + f2(p.x) + "|" + f2(p.y) + "|" + f2(p.z));
			it = flights.erase(it);
			continue;
		}
		++it;
	}
	for (auto it = copies.begin(); it != copies.end();)
		it = now - it->second.second > FLIGHT_MS + 5000 ? copies.erase(it) : ++it;
	for (auto it = grenades.begin(); it != grenades.end();)
	{
		if (now < it->second)
		{
			++it;
			continue;
		}
		release(it->first);
		it = grenades.erase(it);
	}
}
} // namespace

void frame(bool host, bool linked, std::vector<std::string>& out)
{
	u32 now = Device.dwTimeGlobal;
	last_frame = now;
	if (!g_pGameLevel || !g_actor)
	{
		last_left.clear();
		drops.clear();
		throws.clear();
		grenades.clear();
		flights.clear();
		copies.clear();
		outgoing.clear();
		snap_at.clear();
		loot_at.clear();
		box_seq = box_acked = 0;
		return;
	}
	// let go of a frame ago: on the ground now (one in a box was told as such when it went in)
	for (auto it = drops.begin(); it != drops.end();)
	{
		if (Device.dwFrame - it->second < 2)
		{
			++it;
			continue;
		}
		CObject* o = find(it->first);
		CInventoryItem* item = smart_cast<CInventoryItem*>(o);
		if (item && !o->H_Parent())
		{
			const Fvector& p = o->Position();
			CGameObject* go = smart_cast<CGameObject*>(o);
			outgoing.push_back("drop|" + sect(o) + "|" + props(item) + "|" + f2(p.x) + "|" + f2(p.y) + "|" + f2(p.z) + "|" +
				std::to_string(go ? go->ai_location().level_vertex_id() : u32(-1)) + "|" +
				std::to_string(go ? go->ai_location().game_vertex_id() : u32(-1)));
		}
		it = drops.erase(it);
	}
	for (auto it = last_left.begin(); it != last_left.end();)
		it = Device.dwFrame - it->second.frame > 200 ? last_left.erase(it) : ++it;
	launch(now);

	if (host)
	{
		// what a box holds after a change; a body's loot once it is filled
		for (auto it = snap_at.begin(); it != snap_at.end();)
		{
			if (now < it->second)
			{
				++it;
				continue;
			}
			CObject* box = find(it->first);
			if (shared && box && smart_cast<CInventoryBox*>(box) && !ignored.count(box->ID()))
			{
				xr_vector<CInventoryItem*> list;
				items_of(box, list);
				std::string s = "bset|" + std::to_string(box->ID()) + "|" + std::to_string(box_acked) + "|";
				for (size_t i = 0; i < list.size(); i++)
				{
					std::string p = props(list[i]);
					p[p.find('|')] = ':';
					s += (i ? ";" : "") + sect(&list[i]->object()) + ":" + p;
				}
				outgoing.push_back(s);
			}
			it = snap_at.erase(it);
		}
		for (auto it = loot_at.begin(); it != loot_at.end();)
		{
			if (now < it->second)
			{
				++it;
				continue;
			}
			CObject* body = find(it->first);
			if (body && smart_cast<CInventoryOwner*>(body))
			{
				xr_vector<CInventoryItem*> list;
				items_of(body, list);
				std::string s = "loot|" + std::to_string(body->ID()) + "|" + sect(body) + "|";
				for (size_t i = 0; i < list.size(); i++)
				{
					std::string p = props(list[i]);
					p[p.find('|')] = ':';
					s += (i ? ";" : "") + sect(&list[i]->object()) + ":" + p;
				}
				outgoing.push_back(s);
			}
			it = loot_at.erase(it);
		}
	}
	else
	{
		snap_at.clear();
		loot_at.clear();
	}
	for (auto& l : outgoing)
		trace(">", l, linked ? "" : " (not sent: the partner is elsewhere)");
	if (linked)
		out.swap(outgoing);
	outgoing.clear();
}

void receive(bool host, const std::string& line)
{
	if (!g_pGameLevel || !g_actor)
		return;
	trace("<", line);
	auto a = split(line, '|');
	const std::string& kind = a[0];
	// (each player's own loot: the partner's box moves are not this game's; one from before the switch is dropped)
	if (!shared && (kind == "btake" || kind == "bput" || kind == "bset" || kind == "bcond" || kind == "bdeny"))
		return;
	if (kind == "take" && a.size() >= 6)
	{
		Fvector p = {(float)atof(a[3].c_str()), (float)atof(a[4].c_str()), (float)atof(a[5].c_str())};
		if (CObject* o = ground_item(here_id((u16)atoi(a[1].c_str())), a[2], p))
			release(o->ID());
	}
	else if (kind == "drop" && a.size() >= 9)
	{
		Fvector p = {(float)atof(a[4].c_str()), (float)atof(a[5].c_str()), (float)atof(a[6].c_str())};
		u32 lv = (u32)strtoul(a[7].c_str(), nullptr, 10), gv = (u32)strtoul(a[8].c_str(), nullptr, 10);
		if (lv == u32(-1) || !ai().level_graph().valid_vertex_id(lv))
			lv = ai().level_graph().vertex_id(p);
		if (gv == u32(-1) || gv > 0xffff)
			gv = g_actor->ai_location().game_vertex_id();
		spawn(a[1], u16(-1), a[2], a[3], &p, lv, gv);
	}
	else if (kind == "throw" && a.size() >= 8)
	{
		Throw t;
		t.section = a[1];
		t.from = {(float)atof(a[2].c_str()), (float)atof(a[3].c_str()), (float)atof(a[4].c_str())};
		t.velocity = {(float)atof(a[5].c_str()), (float)atof(a[6].c_str()), (float)atof(a[7].c_str())};
		t.at = Device.dwTimeGlobal;
		t.n = a.size() >= 9 ? (u32)atoi(a[8].c_str()) : 0;
		u32 lv = ai().level_graph().vertex_id(t.from);
		spawn(t.section, u16(-1), "1.000", "0", &t.from, lv, g_actor->ai_location().game_vertex_id());
		throws.push_back(t);
	}
	else if (kind == "gift" && a.size() >= 3)
	{
		pause(2000); // (not a pickup from the ground)
		make_gift(a[1], a[2]);
		try
		{
			luabind::functor<void> f;
			if (ai().script_engine().functor("coop_trade.engine_gift", f))
				f(a[1].c_str());
		}
		catch (...)
		{
		}
	}
	else if (kind == "land" && a.size() >= 5)
	{
		// the partner's bolt came to rest: its copy here goes there
		auto c = copies.find((u32)atoi(a[1].c_str()));
		CObject* o = c != copies.end() ? find(c->second.first) : nullptr;
		CPhysicsShellHolder* h = smart_cast<CPhysicsShellHolder*>(o);
		if (h && !o->H_Parent())
		{
			Fvector p = {(float)atof(a[2].c_str()), (float)atof(a[3].c_str()), (float)atof(a[4].c_str())};
			float off = o->Position().distance_to(p);
			Fmatrix m = o->XFORM();
			m.c = p;
			if (h->PPhysicsShell() && h->PPhysicsShell()->isActive())
			{
				Fvector zero;
				zero.set(0.f, 0.f, 0.f);
				h->PPhysicsShell()->SetTransform(m, mh_unspecified);
				h->PPhysicsShell()->set_LinearVel(zero);
				h->PPhysicsShell()->set_AngularVel(zero);
			}
			o->XFORM().set(m);
			trace("~", "landed copy " + a[1] + " moved " + f2(off), "");
		}
		if (c != copies.end())
			copies.erase(c);
	}
	else if (kind == "btake" && a.size() >= 5)
	{
		CObject* box = find(here_id((u16)atoi(a[1].c_str())));
		u16 id = here_id((u16)atoi(a[2].c_str()));
		const std::string& section = a[3];
		if (host)
		{
			box_acked = _max(box_acked, (u32)atoi(a[4].c_str()));
			if (box)
				snap_at[box->ID()] = Device.dwTimeGlobal + SNAP_MS;
		}
		// this player took the same thing a moment before the word came: it stays with the host
		if (CInventoryItem* mine = on_player(id, section))
		{
			if (host)
				outgoing.push_back("bdeny|" + a[2] + "|" + section);
			else
			{
				release(mine->object().ID());
				say("Напарник забрал это раньше вас");
			}
			return;
		}
		CInventoryItem* item = box ? in_container(box, id, section) : nullptr;
		if (item)
		{
			// the guest's copy may be in another state: it takes the host's
			if (host)
				outgoing.push_back("bcond|" + a[2] + "|" + section + "|" + f3(item->GetCondition()));
			release(item->object().ID());
			refresh(box->ID());
		}
		else if (box && host)
			outgoing.push_back("bdeny|" + a[2] + "|" + section);
	}
	else if (kind == "bcond" && a.size() >= 4 && !host)
	{
		if (CInventoryItem* item = on_player(here_id((u16)atoi(a[1].c_str())), a[2]))
			item->SetCondition((float)atof(a[3].c_str()));
	}
	else if (kind == "bdeny" && a.size() >= 3 && !host)
	{
		if (CInventoryItem* item = on_player(here_id((u16)atoi(a[1].c_str())), a[2]))
		{
			release(item->object().ID());
			say("Напарник забрал это раньше вас");
		}
	}
	else if (kind == "bput" && a.size() >= 6)
	{
		CObject* box = find(here_id((u16)atoi(a[1].c_str())));
		if (host)
		{
			box_acked = _max(box_acked, (u32)atoi(a[5].c_str()));
			if (box)
				snap_at[box->ID()] = Device.dwTimeGlobal + SNAP_MS;
		}
		if (box)
		{
			spawn(a[2], box->ID(), a[3], a[4]);
			refresh(box->ID());
		}
	}
	else if (kind == "bset" && a.size() >= 4 && !host)
	{
		// a change of this player's own is still on its way to the host: the next list has it
		if ((u32)atoi(a[2].c_str()) < box_seq)
			return;
		CObject* box = find(here_id((u16)atoi(a[1].c_str())));
		if (!box || !smart_cast<CInventoryBox*>(box))
			return;
		std::map<std::string, std::vector<std::pair<std::string, std::string>>> want;
		if (!a[3].empty())
			for (auto& e : split(a[3], ';'))
			{
				auto f = split(e, ':');
				if (f.size() >= 3 && pSettings->section_exist(f[0].c_str()))
					want[f[0]].push_back({f[1], f[2]});
			}
		std::map<std::string, std::vector<u16>> have;
		xr_vector<CInventoryItem*> list;
		items_of(box, list);
		for (CInventoryItem* it : list)
			have[sect(&it->object())].push_back(it->object().ID());
		int changed = 0;
		for (auto& h : have)
		{
			size_t keep = want.count(h.first) ? want[h.first].size() : 0;
			for (size_t i = keep; i < h.second.size(); i++, changed++)
				release(h.second[i]);
		}
		for (auto& w : want)
		{
			size_t got = have.count(w.first) ? have[w.first].size() : 0;
			for (size_t i = got; i < w.second.size(); i++, changed++)
				spawn(w.first, box->ID(), w.second[i].first, w.second[i].second);
		}
		if (changed)
			refresh(box->ID());
	}
	else if (kind == "loot" && a.size() >= 4 && !host)
	{
		// what the local death_manager put on the body gives way to the host's list
		CObject* body = find(here_id((u16)atoi(a[1].c_str())));
		CEntityAlive* e = smart_cast<CEntityAlive*>(body);
		if (!body || !e || e->g_Alive() || sect(body) != a[2])
			return;
		xr_vector<CInventoryItem*> list;
		items_of(body, list);
		std::vector<u16> old;
		for (CInventoryItem* it : list)
			old.push_back(it->object().ID());
		for (u16 id : old)
			release(id);
		if (!a[3].empty())
			for (auto& entry : split(a[3], ';'))
			{
				auto f = split(entry, ':');
				if (f.size() >= 3)
					spawn(f[0], body->ID(), f[1], f[2]);
			}
		refresh(body->ID());
	}
}
} // namespace coop_items
