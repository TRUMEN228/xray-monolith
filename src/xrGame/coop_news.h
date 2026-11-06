////////////////////////////////////////////////////////////////////////////
//	Module 		: coop_news.h
//	Description : Anomaly Co-op: the PDA's messages in both games (r14). A news item that comes to this player's
//	              PDA (tasks, news, tips: CActor::AddGameNews) comes to the partner's too; one both games make
//	              (a shared task's update) shows once - the second within 30 s is dropped. Dialog lines are not sent.
////////////////////////////////////////////////////////////////////////////

#pragma once

#include <string>
#include <vector>

struct GAME_NEWS_DATA;

namespace coop_news
{
// from CActor::AddGameNews: false - the partner's same message came moments ago, this one is not shown
bool local(const GAME_NEWS_DATA& news);
// the session's frame: this player's messages go out while both are in a game
void frame(bool linked, std::vector<std::string>& out);
// the partner's message
void receive(const std::string& line);
// the scripts: messages that are this player's own (the mod's), not the partner's, while on
void keep_local(bool on);
// the tests: how many messages with this text this player's PDA has
int count(const char* text);
} // namespace coop_news
