////////////////////////////////////////////////////////////////////////////
//	Module 		: coop_props.h
//	Description : Anomaly Co-op: money and the world's physical things in the engine (r15).
//	              Money: with the host's "shared money" the player's money changes go to the partner as they
//	              happen (the host's total when the two meet); a sum given to the partner. Push doors (a physics
//	              hinge without a script) near the players turn in both games; a breakable thing broken here
//	              breaks there; this player's explosion is seen and heard in the partner's game.
////////////////////////////////////////////////////////////////////////////

#pragma once

#include <string>
#include <vector>

class CObject;
struct SHit;
class CDestroyablePhysicsObject;
class CSE_ALifeDynamicObject;

namespace coop_props
{
// the session's frame: this game's changes go out (same_level: both on this level; in_game: both in a game),
// the partner's are done here
void frame(bool host, bool same_level, bool in_game, std::vector<std::string>& out);
void receive(bool host, const std::string& line);

// from the engine
void destroyed(CDestroyablePhysicsObject* obj);       // a breakable thing broke here
void exploded(CObject* explosive, const Fvector& at);  // this player's explosion

// the scripts
void set_shared_money(bool on);  // the host's "shared money"
void give_money(int amount);     // a sum for the partner (already taken from this player)
void skip_door(u16 id);          // a scripted door (ph_door): its own scripts move it in both games
// r16: this player hit the partner's puppet (friendly fire, when the host allows it: set_friendly_fire)
void friendly(const SHit* hit, CObject* puppet);
void set_friendly_fire(bool on);
// r17 (host): a creature hit the guest's puppet - the hit goes to the guest's game as it is
void creature_hit(const SHit* hit, CObject* puppet, CObject* who);

// r18: the simulation's registry (host): what comes and goes in the world
void registered(CSE_ALifeDynamicObject* object);
void unregistered(CSE_ALifeDynamicObject* object);
void set_spawns_armed(bool on);
// r18: this player's story flags (the scripts say when the world is ready for them)
void info_changed(LPCSTR info, bool on);
void set_info_ready(bool on);
// r18: this player traded with an NPC: bought (the item leaves the partner's trader) or sold (the partner's gets it)
void traded(u16 trader, u16 item, LPCSTR section, bool bought);
// r18: the talk with an NPC: lines and answers of the starter, the partner's choice, switches and the end
void talk_line(LPCSTR speaker, LPCSTR text, LPCSTR other);
void talk_options(LPCSTR other, LPCSTR list, u16 npc);
void talk_pick(LPCSTR id);
void talk_switch(LPCSTR kind);
void talk_end();
} // namespace coop_props
