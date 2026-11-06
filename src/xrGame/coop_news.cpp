////////////////////////////////////////////////////////////////////////////
//	Module 		: coop_news.cpp
//	Description : Anomaly Co-op: the PDA's messages in both games (r14).
//	              The line: show time|icon|caption|text ('\' '|' and line breaks escaped)
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "coop_news.h"
#include "game_news.h"
#include "Actor.h"
#include "alife_registry_wrappers.h"

#include <deque>

namespace coop_news
{
namespace
{
const u32 SAME_MS = 30000;  // the same message from both games within this long shows once

struct Seen
{
	std::string key;
	u32 at;
};
std::deque<Seen> mine;      // this game's own messages lately
std::deque<Seen> theirs;    // the partner's lately
std::vector<std::string> outgoing;
bool applying = false;
bool own = false;           // the scripts' own messages (the mod's) stay here

std::string escape(const char* s)
{
	std::string r;
	for (; s && *s; s++)
	{
		if (*s == '\\') r += "\\\\";
		else if (*s == '|') r += "\\p";
		else if (*s == '\n') r += "\\n";
		else if (*s != '\r') r += *s;
	}
	return r;
}

std::string unescape(const std::string& s)
{
	std::string r;
	for (size_t i = 0; i < s.size(); i++)
	{
		if (s[i] == '\\' && i + 1 < s.size())
		{
			char c = s[++i];
			r += c == 'p' ? '|' : (c == 'n' ? '\n' : c);
		}
		else
			r += s[i];
	}
	return r;
}

std::string key_of(const std::string& caption, const std::string& text) { return caption + '\x01' + text; }

void forget(std::deque<Seen>& list, u32 now)
{
	while (!list.empty() && now - list.front().at > SAME_MS)
		list.pop_front();
}

bool seen(std::deque<Seen>& list, const std::string& key)
{
	for (auto& s : list)
		if (s.key == key)
			return true;
	return false;
}
} // namespace

bool local(const GAME_NEWS_DATA& news)
{
	if (applying || own || news.m_type != GAME_NEWS_DATA::eNews)
		return true;
	u32 now = Device.dwTimeGlobal;
	forget(theirs, now);
	forget(mine, now);
	std::string caption = news.news_caption.c_str() ? news.news_caption.c_str() : "";
	std::string text = news.news_text.c_str() ? news.news_text.c_str() : "";
	std::string key = key_of(caption, text);
	if (seen(theirs, key))
		return false; // (the partner's game made the same one and it is shown already)
	mine.push_back({key, now});
	outgoing.push_back(std::to_string(news.show_time) + "|" + escape(news.texture_name.c_str()) + "|" +
		escape(caption.c_str()) + "|" + escape(text.c_str()));
	return true;
}

void frame(bool linked, std::vector<std::string>& out)
{
	if (linked)
		for (auto& l : outgoing)
			out.push_back(l);
	outgoing.clear();
}

void receive(const std::string& line)
{
	if (!g_actor)
		return;
	std::string f[4];
	size_t at = 0;
	for (int i = 0; i < 3; i++)
	{
		size_t bar = line.find('|', at);
		if (bar == std::string::npos)
			return;
		f[i] = line.substr(at, bar - at);
		at = bar + 1;
	}
	f[3] = line.substr(at);
	std::string caption = unescape(f[2]), text = unescape(f[3]);
	std::string key = key_of(caption, text);
	u32 now = Device.dwTimeGlobal;
	forget(mine, now);
	forget(theirs, now);
	if (seen(mine, key))
		return; // (this game showed the same one moments ago)
	theirs.push_back({key, now});

	GAME_NEWS_DATA news;
	news.m_type = GAME_NEWS_DATA::eNews;
	news.news_caption = caption.c_str();
	news.news_text = text.c_str();
	int show = atoi(f[0].c_str());
	if (show > 0)
		news.show_time = show;
	std::string icon = unescape(f[1]);
	news.texture_name = icon.empty() ? "ui_inGame2_PD_Lider" : icon.c_str();
	applying = true;
	Actor()->AddGameNews(news);
	applying = false;
}

void keep_local(bool on) { own = on; }

int count(const char* text)
{
	if (!g_actor || !text)
		return 0;
	int n = 0;
	for (auto& d : Actor()->game_news_registry->registry().objects())
		if (d.news_text.c_str() && !xr_strcmp(d.news_text.c_str(), text))
			n++;
	return n;
}
} // namespace coop_news
