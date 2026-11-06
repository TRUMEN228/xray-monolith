////////////////////////////////////////////////////////////////////////////
//	Module 		: coop_clock.cpp
//	Description : Anomaly Co-op: the host's time, weather and the scripts' world state in the guest's game (r14).
//	              The line: game time (ms)|time factor|weather|the host's count of sudden sky changes|scripts' state
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "coop_clock.h"
#include "Level.h"
#include "ai_space.h"
#include "script_engine.h"
#include "alife_simulator.h"
#include "alife_time_manager.h"
#include "game_sv_base.h"
#include "xrServer.h"
#include "../xrEngine/Environment.h"
#include "../xrEngine/IGame_Persistent.h"

namespace coop_clock
{
namespace
{
const u32 SEND_MS = 1000;
// the guest's clock is put to the host's when this far off (game ms): a minute, a few seconds of real time
const s64 OFF_MS = 60 * 1000;

u32 next_send = 0;
u32 jumps = 0;               // host: sudden sky changes so far
std::string state;           // host: the scripts' own world state
std::string last_weather;    // host: the weather cycle (not a weather effect playing over it)

bool first = true;           // guest: the next line is the first in this game
std::string host_weather;
std::string host_jumps;
std::string host_state;

bool alife_ready() { return ai().get_alife() && ai().alife().initialized(); }

std::string weather_now()
{
	CEnvironment& env = g_pGamePersistent->Environment();
	if (!env.IsWFXPlaying() && env.GetWeather().size())
		last_weather = env.GetWeather().c_str();
	return last_weather;
}

void call_state(const std::string& s)
{
	try
	{
		luabind::functor<void> f;
		if (ai().script_engine().functor("coop_world.engine_world_state", f))
			f(s.c_str());
	}
	catch (...)
	{
	}
}
} // namespace

void weather_jumped() { jumps++; }
void set_state(const std::string& s) { state = s; }

void reset()
{
	first = true;
	host_weather.clear();
	host_jumps.clear();
	host_state.clear();
	next_send = 0;
}

bool collect(std::string& out, u32 now)
{
	if (!alife_ready() || now < next_send)
		return false;
	next_send = now + SEND_MS;
	char head[96];
	xr_sprintf(head, "%llu|%.3f|", (unsigned long long)ai().alife().time_manager().game_time(),
		ai().alife().time_manager().time_factor());
	out = head + weather_now() + "|" + std::to_string(jumps) + "|" + state;
	return true;
}

void apply(const std::string& line)
{
	if (!alife_ready())
		return;
	// the fields (the scripts' state is the rest of the line)
	std::string f[5];
	size_t at = 0;
	for (int i = 0; i < 4; i++)
	{
		size_t bar = line.find('|', at);
		if (bar == std::string::npos)
			return;
		f[i] = line.substr(at, bar - at);
		at = bar + 1;
	}
	f[4] = line.substr(at);

	// the clock: the host's when off by more than a minute (forward or back), the host's pace
	CALifeTimeManager& tm = const_cast<CALifeTimeManager&>(ai().alife().time_manager());
	ALife::_TIME_ID host_time = (ALife::_TIME_ID)_strtoui64(f[0].c_str(), nullptr, 10);
	float factor = (float)atof(f[1].c_str());
	s64 diff = (s64)host_time - (s64)tm.game_time();
	if (host_time && (diff > OFF_MS || diff < -OFF_MS))
		tm.set_game_time(host_time);
	if (factor > 0.f && _abs(tm.time_factor() - factor) > 0.01f && Level().Server && Level().Server->game)
		Level().Server->game->SetGameTimeFactor(factor);

	// the sky: at once the first time and when the host's own jumped, gradually otherwise
	if (!f[2].empty() && (f[2] != host_weather || f[3] != host_jumps))
	{
		bool at_once = first || (f[3] != host_jumps && !host_jumps.empty());
		host_weather = f[2];
		host_jumps = f[3];
		CEnvironment& env = g_pGamePersistent->Environment();
		if (at_once || env.GetWeather() != shared_str(f[2].c_str()))
			env.SetWeather(f[2].c_str(), at_once);
	}

	// the scripts' state (a surge)
	if (first || f[4] != host_state)
	{
		host_state = f[4];
		call_state(host_state);
	}
	first = false;
}
} // namespace coop_clock
