////////////////////////////////////////////////////////////////////////////
//	Module 		: coop_clock.h
//	Description : Anomaly Co-op: the host's time, weather and the scripts' world state (a surge) in the guest's
//	              game (r14). The host sends them once a second while both are in a game on one level; the guest's
//	              clock is put to the host's (forward or back), it runs at the host's pace, its sky is the host's.
////////////////////////////////////////////////////////////////////////////

#pragma once

#include <string>

namespace coop_clock
{
// host: the line for the guest, once a second (false: not yet)
bool collect(std::string& out, u32 now);
// guest: the host's newest line
void apply(const std::string& line);
// a new game or level: the next line is applied as the first
void reset();

// the scripts (host): the sky changed at once (a load, a forced change: the guest's jumps too), and the scripts'
// own world state (the guest's scripts get it through coop_world.engine_world_state when it changes)
void weather_jumped();
void set_state(const std::string& state);
} // namespace coop_clock
