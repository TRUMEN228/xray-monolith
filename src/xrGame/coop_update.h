////////////////////////////////////////////////////////////////////////////
//	Module 		: coop_update.h
//	Description : Anomaly Co-op: updates from the game (see coop_update.cpp)
////////////////////////////////////////////////////////////////////////////

#pragma once

namespace coop_update
{
// ask GitHub for the newest release (in the background), compared with the mod's version
void check(LPCSTR current);
// download its AnomalyCoop.zip into coop_update\ (in the background)
void download();
// unpack the downloaded archive over the game once this game has quit; false when there is none
bool apply();
// "idle", "checking", "none", "available|<ver>", "downloading|<ver>", "ready|<ver>", "error|<text>"
LPCSTR status();
} // namespace coop_update
