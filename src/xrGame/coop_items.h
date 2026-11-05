////////////////////////////////////////////////////////////////////////////
//	Module 		: coop_items.h
//	Description : Anomaly Co-op: items between the two games (r13). The engine sees where an item goes: out of a
//	              box or a body into the player's hands, up from the ground, down to the ground, into a box. Each
//	              such move of this player's is told to the partner, whose game does the same; a box's contents
//	              follow the host's (the host sends them after each change), a body's loot is the host's.
////////////////////////////////////////////////////////////////////////////

#pragma once

#include <string>
#include <vector>
#include <functional>

class CObject;
class CInventoryItem;

namespace coop_items
{
// from the engine's inventories: an item entered or left a box, an NPC or the player
void entered(CObject* owner, CInventoryItem* item);
void left(CObject* owner, CInventoryItem* item, bool destroyed);

// the session's frame: this game's moves go out (lines for the partner), the partner's are done here
void frame(bool host, bool linked, std::vector<std::string>& out);
void receive(bool host, const std::string& line);
// a creature the host's engine saw die: its loot goes to the guest a moment later
void died(u16 id);
// this player threw a bolt or a grenade: from where, how fast (the partner's copy flies the same way)
void thrown(CObject* missile, const Fvector& from, const Fvector& velocity);
// a message for this player
void set_note(std::function<void(const std::string&)> note);

// the scripts: a box that is not the world's (the exchange box), a while of changes that are the scripts' own
void ignore_box(u16 id, bool ignore);
void pause(u32 ms);
} // namespace coop_items
