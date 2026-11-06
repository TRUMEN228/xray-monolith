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
class CDestroyablePhysicsObject;

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
} // namespace coop_props
