////////////////////////////////////////////////////////////////////////////
//	Module 		: coop_creatures.h
//	Description : Anomaly Co-op: the host's creatures in the guest's game (r12). The host sends where each stalker
//	              and monster near either player is, its health, how calm it is, whether it crouches, where its
//	              weapon is, how it stands to the players and whom it fights; the guest's engine drives its own
//	              copies with it (CCoopNetPuppet), every frame, without the scripts.
////////////////////////////////////////////////////////////////////////////

#pragma once

#include <string>

namespace coop_creatures
{
// host: the creatures near either player as one frame's payload
void collect(std::string& out, u32 now);
// r20: the bytes of one creature's record (a frame is the u32 stamp and whole records: it may be cut between them)
size_t record_size();
// guest: the host's creatures as they came (the game's thread)
void apply(const std::string& payload);
// guest: copies the host stopped sending go back to their own AI; all of them when the link is gone
void update(u32 now);
void release_all();

// the scripts' id pairs (host id -> this game's id) and the partner's puppet (an id here)
void map(u16 host_id, u16 local_id);
void unmap(u16 host_id);
void clear_map();
void set_partner(u16 id);
// host (r16): the creatures near either player for the guest's mirror (coop_mirror), once in a while (false: not yet)
bool census(std::string& out, u32 now);
u16 partner_id(); // the partner's puppet here (65535 none)
// the pairs both ways (an id with no pair is the same in both games)
u16 local_id_of(u16 host_id);
u16 host_id(u16 local_id);
// guest: whom this creature fights on the host, as an id here (65535 none)
u16 target_of(u16 id);
} // namespace coop_creatures
