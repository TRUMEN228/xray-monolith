////////////////////////////////////////////////////////////////////////////
//	Module 		: coop_link.h
//	Description : Anomaly Co-op: the link to the partner inside the engine, in place of CoopLauncher's bridge.
//	              The game script swaps with it the same text frames it writes to coop_out.txt and reads from
//	              coop_in.txt; the partner sees the same wire protocol as from the launcher.
////////////////////////////////////////////////////////////////////////////

#pragma once

namespace coop_link
{
// start a session: a host waits for its partner on the port, a guest connects (and reconnects) to the host
bool host(int port, LPCSTR name, LPCSTR version);
bool join(LPCSTR address, int port, LPCSTR name, LPCSTR version);
// the same through Steam: a friends-only lobby, or a friend's lobby (its id in decimal)
bool host_steam(LPCSTR name, LPCSTR version);
bool join_steam(LPCSTR lobby, LPCSTR name, LPCSTR version);
// friends hosting a co-op lobby now, "name|lobby" lines (starts Steam when it is not yet)
LPCSTR steam_friends();
// a lobby a friend invited us to ("Join game" in Steam), "" none; read once
LPCSTR steam_invite();
// guest: true once after the host's world arrived (the main menu then loads it)
bool take_session();
void stop();
bool active();

// the game's frame (the text of coop_out.txt): states and events for the partner
void game_out(LPCSTR text);
// the frame for the game (the text of coop_in.txt), or nullptr when nothing changed and it is not time to say alive
LPCSTR game_in();

// "idle", "listening", "connecting", or "connected|<partner>|<rtt ms>"; the last problem in "error|<text>"
LPCSTR status();
} // namespace coop_link
