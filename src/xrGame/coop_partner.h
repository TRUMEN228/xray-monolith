////////////////////////////////////////////////////////////////////////////
//	Module 		: coop_partner.h
//	Description : Anomaly Co-op: the partner's puppet and the players' hits in the engine (r15).
//	              Each game sends where its player is 20 times a second - position, heading, look, the movement
//	              state and angles - with the scripts' slower part (weapon, outfit, health, shots: set by
//	              coop_player); the partner's game moves the puppet at once and gives the scripts the rest
//	              (coop_player.engine_partner). A creature the guest's player hits loses the same health in the
//	              host's game (and dies there, by the guest's puppet, when it died here).
////////////////////////////////////////////////////////////////////////////

#pragma once

#include <string>
#include <vector>

class CEntityAlive;

namespace coop_partner
{
// the session's frame: this player's line for the partner (false: not yet), and the partner's newest
bool collect(std::string& out, u32 now);
void apply(const std::string& line);
// the scripts' slower part of this player's state
void set_extras(const std::string& extras);

// CEntityAlive::Hit: this player's hit took this much health (died: the hit killed it)
void actor_hit(CEntityAlive* victim, float lost, bool died);
// the session's frame: the hits for the host (guest), the guest's hits done here (host)
void hits(bool host, bool linked, std::vector<std::string>& out);
void hit_in(const std::string& line);
} // namespace coop_partner
