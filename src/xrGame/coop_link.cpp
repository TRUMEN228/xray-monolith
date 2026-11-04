////////////////////////////////////////////////////////////////////////////
//	Module 		: coop_link.cpp
//	Description : Anomaly Co-op: the link to the partner inside the engine (see coop_link.h).
//	              A port of CoopLauncher's Bridge: frames are a type byte and a payload, over TCP a 4-byte
//	              length goes first. States are coalesced (the newest line of each tag, at most every 45 ms),
//	              events keep their order and are numbered for the game, which acknowledges them.
//	              The session lives here too (r12): where each player is (main menu, loading, in the game, which
//	              level), the host's world going to a guest in its menu, both games reloading, the host's travel
//	              reloading the guest, the guest's game ending with the host's. It runs on the engine's frame, in
//	              the main menu as in a game; the scripts only ask for a reload and are told before a load.
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "coop_link.h"
#include "coop_link_net.h"
#include "Level.h"
#include "Actor.h"
#include "UIGameCustom.h"
#include "ai_space.h"
#include "script_engine.h"
#include "../xrEngine/XR_IOConsole.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <map>
#include <vector>
#include <string>
#include <chrono>
#include <memory>

#pragma comment(lib, "ws2_32.lib")

namespace coop_link
{
namespace
{
// protocol 8: the session in the engine (presence frames; the world and reloads no longer pass the scripts).
// Protocol 7 (Engine 8-11, the 2.10 launcher) does not understand it
const char* PROTOCOL = "8";
const char* ENGINE_REVISION = "12"; // coop_engine_version(), told to the partner in the hello
enum : u8 { MsgHello = 1, MsgState = 2, MsgEvent = 3, MsgFile = 4, MsgPing = 5, MsgStateZ = 6, MsgPong = 7, MsgPresence = 8 };

const u64 HOST_SETTLE_MS = 5000;     // the host's game runs this long on a level before its world goes anywhere
const u64 TRAVEL_RECENT_MS = 120000; // a load this soon after the last moment in a game is a travel
const u64 TRAVEL_WAIT_MS = 90000;    // how long a travel waits for the guest to be in a game
const float TRAVEL_FAR = 100.f;      // a load this far from the last place, on the same level, is a travel too
const float JUMP_FAR = 60.f;         // a move this far within a second, without a load (the PDA's travel)
const u64 SAVE_RETRY_MS = 2500, SAVE_GIVE_UP_MS = 10000;
const u64 LOST_KICK_MS = 8000;       // the host gone this long: the guest's game ends

// where this game is: the main menu, a level that is loading, or a game
std::string where_now()
{
	if (!g_pGameLevel)
		return "menu";
	if (!g_actor || !g_loading_events.empty())
		return "loading";
	return "game";
}

void register_frame();

// the level's name ("" while a level is still loading and has none yet)
std::string level_name()
{
	if (!g_pGameLevel)
		return "";
	LPCSTR n = Level().name().c_str();
	return n ? n : "";
}

// a script function, if there is one ("module.function"); a failure there is not the session's
void call_script(LPCSTR name, LPCSTR arg)
{
	try
	{
		luabind::functor<void> f;
		if (ai().script_engine().functor(name, f))
			f(arg);
	}
	catch (...)
	{
		Msg("! coop link: %s failed", name);
	}
}

typedef std::chrono::steady_clock clock_t_;

u64 now_ms() { return (u64)std::chrono::duration_cast<std::chrono::milliseconds>(clock_t_::now().time_since_epoch()).count(); }

std::vector<std::string> split(const std::string& s, char c)
{
	std::vector<std::string> out;
	size_t from = 0;
	for (;;)
	{
		size_t at = s.find(c, from);
		out.push_back(s.substr(from, at == std::string::npos ? std::string::npos : at - from));
		if (at == std::string::npos)
			return out;
		from = at + 1;
	}
}

std::string clean(std::string s)
{
	for (char& c : s)
		if (c == '|' || c == '\n' || c == '\r')
			c = ' ';
	return s;
}

// names go in UTF-8 on the wire, the game's text is in cp1251
std::string recode(const std::string& s, UINT from, UINT to)
{
	if (s.empty())
		return s;
	int wn = MultiByteToWideChar(from, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(wn, L'\0');
	MultiByteToWideChar(from, 0, s.data(), (int)s.size(), &w[0], wn);
	int n = WideCharToMultiByte(to, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
	std::string out(n, '\0');
	WideCharToMultiByte(to, 0, w.data(), wn, &out[0], n, nullptr, nullptr);
	return out;
}

// the texts here are UTF-8 (this file is), the game shows cp1251
std::string game_text(const std::string& utf8) { return recode(utf8, CP_UTF8, 1251); }
std::string utf8(const std::string& cp1251) { return recode(cp1251, 1251, CP_UTF8); }

std::string frame(u8 type, const std::string& payload)
{
	std::string f(1, (char)type);
	f += payload;
	return f;
}

////////////////////////////////////////////////////////////////////////// transport
struct TcpLink : Link
{
	SOCKET s;
	std::mutex send_lock;

	explicit TcpLink(SOCKET sock) : s(sock)
	{
		BOOL on = TRUE;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on));
		DWORD timeout = 15000; // pings come every 3 s
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
	}
	~TcpLink() override { close(); }

	bool send(const std::string& f) override
	{
		std::lock_guard<std::mutex> g(send_lock);
		u32 len = (u32)f.size();
		std::string buf((const char*)&len, 4);
		buf += f;
		size_t off = 0;
		while (off < buf.size())
		{
			int n = ::send(s, buf.data() + off, (int)(buf.size() - off), 0);
			if (n <= 0)
				return false;
			off += n;
		}
		return true;
	}

	bool read_exact(char* p, size_t n)
	{
		while (n)
		{
			int r = ::recv(s, p, (int)n, 0);
			if (r <= 0)
				return false;
			p += r;
			n -= r;
		}
		return true;
	}

	bool receive(std::string& f) override
	{
		u32 len = 0;
		if (!read_exact((char*)&len, 4) || len < 1 || len > 64u * 1024 * 1024)
			return false;
		f.resize(len);
		return read_exact(&f[0], len);
	}

	void close() override
	{
		SOCKET old = s;
		s = INVALID_SOCKET;
		if (old != INVALID_SOCKET)
		{
			shutdown(old, SD_BOTH);
			closesocket(old);
		}
	}
};

////////////////////////////////////////////////////////////////////////// the bridge
struct Bridge
{
	std::mutex m;
	std::condition_variable wake_send;
	std::atomic<bool> running{false};
	std::string role, my_name, version, saves_dir;
	std::string state_text = "idle";

	SOCKET listener = INVALID_SOCKET;
	std::shared_ptr<Link> link;
	std::thread net_thread;
	bool steam = false;

	// partner -> game
	std::string sid;
	bool connected = false;
	std::string peer_name;
	int rtt = 0;
	std::map<std::string, std::string> peer_state;
	std::deque<std::pair<s64, std::string>> to_game;
	s64 to_game_seq = 0;

	// game -> partner
	std::string game_sid;
	s64 last_game_event = 0;
	std::map<std::string, std::string> last_sent_state;
	std::map<std::string, std::string> pending_state;
	std::deque<std::string> outbox;
	bool game_in_guest_session = false, resume_sent = false;
	bool session_sent = false;         // host: the world went to this partner (or it came back still in it)
	u64 connected_since = 0, game_seen_since = 0;
	std::string session_token, received_token;
	bool session_arrived = false;      // guest: the host's world came, not yet taken by the menu
	bool saves_written = false;        // files came: the game's file list must learn them (on its own thread)

	// ---------------------------------------------------------------- the session
	// what this game tells the partner, and what the partner told: "where|level|world|ready|in the host's world"
	struct Presence
	{
		std::string where, level, world;
		bool ready = false, hosted = false;
	};
	Presence theirs;                   // under m (the net thread writes it)
	std::string their_prev_where;      // the game's thread
	std::string sent_presence;         // under m
	std::string lobby_world;           // under m: this game's world, from the scripts' lobby state
	bool lobby_ready = false;
	std::string my_where;              // the game's thread
	// host
	bool world_sent = false;           // a guest waiting in its menu was sent the world (once per wait)
	std::string save_name, save_mode;  // a save being made to go to the guest: "join", "reload" or "guest"
	u64 save_retry = 0, save_until = 0;
	bool reload_saved = false;         // the scripts asked: both games load what the host just sent
	bool expect_reloading = false, load_own = false;
	std::string reload_save;
	u64 level_up_at = 0;
	std::string last_level;            // where and when this game last was in a game (a load soon after is a travel)
	Fvector last_pos = {};
	u64 last_seen = 0, next_pos = 0;
	bool own_load = false;             // the next load is the session's own (a reload), no travel
	bool travel = false;
	u64 travel_until = 0;
	// guest
	std::string arrived_name, arrived_mode; // the world that came: its save and what it is for
	std::string load_name;
	u64 load_at = 0;
	bool hosted = false;               // this game runs the host's world (the session loaded it)
	u64 lost_since = 0;

	// the game knows only the saves it found when it started: a world written since then must be shown to it
	void rescan_saves() // under m, on the game's thread
	{
		if (!saves_written)
			return;
		saves_written = false;
		FS.rescan_path(saves_dir.c_str(), FALSE);
	}

	// the frame for the game
	s64 heartbeat = 0;
	std::string last_in_content, in_frame;
	u64 last_in_write = 0;

	~Bridge()
	{
		// the process is going away: its threads are already gone or about to be
		running = false;
		if (net_thread.joinable())
			net_thread.detach();
	}

	void log(const std::string& s)
	{
		Msg("~ coop link: %s", s.c_str());
	}

	void note(const std::string& s) // shown to the player as the launcher's window would show it
	{
		log(s);
		to_game.emplace_back(++to_game_seq, "note|" + clean(game_text(s)));
	}

	void queue(u8 type, const std::string& payload) // under m
	{
		if (!link)
			return;
		outbox.push_back(frame(type, payload));
		wake_send.notify_one();
	}

	// ---------------------------------------------------------------- the game's side
	void game_out(const std::string& raw)
	{
		std::lock_guard<std::mutex> g(m);
		std::vector<std::string> states, events;
		s64 ack = 0;
		for (auto& line : split(raw, '\n'))
		{
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			auto p = split(line, '|');
			if (p[0] == "g" && p.size() >= 3)
			{
				if (p[1] != game_sid)
				{
					// game (re)loaded: its event counter restarted, and it forgot our events
					game_sid = p[1];
					last_game_event = 0;
					to_game.clear();
				}
				ack = _atoi64(p[2].c_str());
			}
			else if (p[0] == "s" && p.size() >= 2)
			{
				if (p[1] == "g")
					game_in_guest_session = p.size() > 2 && p[2] == "1";
				else
				{
					// "s|lobby|ready|where|ng_ready|new/save|world": the menu's and the game's own
					if (p[1] == "lobby" && p.size() >= 4)
					{
						lobby_ready = p[2] == "1";
						lobby_world = p.size() > 6 ? p[6] : "";
					}
					states.push_back(line);
				}
			}
			else if (p[0] == "e" && p.size() >= 3)
			{
				s64 id = _atoi64(p[1].c_str());
				if (id > last_game_event)
				{
					last_game_event = id;
					events.push_back(line.substr(p[0].size() + p[1].size() + 2));
				}
			}
		}
		while (!to_game.empty() && to_game.front().first <= ack)
			to_game.pop_front();

		for (auto& e : events)
		{
			if (e[0] == '@')
			{
				auto p = split(e, '|');
				if (p[0] == "@saved" && p.size() > 1 && connected)
					send_save(p[1], "reload");
			}
			else if (connected)
				queue(MsgEvent, e);
		}

		if (connected && role == "client" && game_in_guest_session && !resume_sent && !received_token.empty())
		{
			resume_sent = true;
			queue(MsgEvent, "resume|" + received_token);
		}

		if (connected)
			for (auto& line : states)
			{
				std::string tag = split(line, '|')[1];
				auto it = last_sent_state.find(tag);
				if (it == last_sent_state.end() || it->second != line)
				{
					last_sent_state[tag] = line;
					pending_state[tag] = line;
				}
			}
		if (!pending_state.empty())
			wake_send.notify_one();
	}

	LPCSTR game_in()
	{
		std::lock_guard<std::mutex> g(m);
		rescan_saves();
		std::string content;
		for (auto& kv : peer_state)
			content += kv.second + "\n";
		for (auto& e : to_game)
			content += "e|" + std::to_string(e.first) + "|" + e.second + "\n";
		content += "end\n";
		u64 now = now_ms();
		if (content == last_in_content && now - last_in_write < 100)
			return nullptr;
		last_in_content = content;
		last_in_write = now;
		// the last field changes on every frame: the game takes an unchanged one for a closed link
		in_frame = "b|" + sid + "|" + std::to_string(last_game_event) + "|" + role + "|" + (connected ? "1" : "0") + "|" +
			clean(peer_name) + "|" + clean(my_name) + "|" + std::to_string(++heartbeat) + "\n" + content;
		return in_frame.c_str();
	}

	// mode: "join" (a guest in its menu loads it), "reload" (both load it), "guest" (the guest loads it)
	void send_save(const std::string& name, const std::string& mode) // under m
	{
		for (LPCSTR ext : {".scop", ".scoc", ".dds"})
		{
			std::string file = name + ext;
			IReader* r = FS.r_open("$game_saves$", file.c_str());
			if (!r)
				continue;
			u32 len = (u32)file.size();
			std::string payload((const char*)&len, 4);
			payload += file;
			payload.append((const char*)r->pointer(), r->length());
			FS.r_close(r);
			queue(MsgFile, payload);
		}
		char token[16];
		xr_sprintf(token, "%08x%04x", (u32)now_ms(), (u32)rand() & 0xffff);
		session_token = token;
		queue(MsgEvent, "session_ready|" + session_token + "|" + mode + "|" + name);
		note("Сейв «" + utf8(name) + "» отправлен напарнику");
		log("world " + name + " sent (" + mode + ")");
	}

	// ---------------------------------------------------------------- the partner's side
	void handle(const std::string& f)
	{
		u8 type = (u8)f[0];
		std::string body = f.substr(1);
		std::lock_guard<std::mutex> g(m);
		switch (type)
		{
		case MsgHello:
			{
				auto p = split(body, '|');
				if (p[0] != PROTOCOL)
				{
					note("Версии не совпадают: у вас протокол " + std::string(PROTOCOL) + ", у напарника " + p[0] + ". Обновитесь оба.");
					if (link)
						link->close();
					return;
				}
				peer_name = p.size() > 1 ? recode(p[1], CP_UTF8, 1251) : "Напарник";
				// the partner started its link anew while ours carried on: it hears us again, and a guest
				// tells it once more that it is still in the world (no reload)
				if (connected)
				{
					queue(MsgHello, std::string(PROTOCOL) + "|" + recode(my_name, 1251, CP_UTF8) + "|" + version + "|" +
						ENGINE_REVISION);
					resume_sent = false;
					note(utf8(peer_name) + " переподключился");
					break;
				}
				connected = true;
				connected_since = now_ms();
				session_sent = false;
				// the new partner hears where this game is, and is sent the world again if it waits for it
				sent_presence.clear();
				theirs = Presence();
				world_sent = false;
				state_text = "connected";
				note("Подключён: " + utf8(peer_name));
				if (p.size() > 2 && p[2] != version)
					note("У " + utf8(peer_name) + " версия " + p[2] + ", у вас " + version + ". Обновитесь оба до последней версии.");
				break;
			}
		case MsgPresence:
			{
				auto p = split(body, '|');
				theirs.where = p[0];
				theirs.level = p.size() > 1 ? p[1] : "";
				theirs.world = p.size() > 2 ? p[2] : "";
				theirs.ready = p.size() > 3 && p[3] == "1";
				theirs.hosted = p.size() > 4 && p[4] == "1";
				break;
			}
		case MsgState:
			for (auto& line : split(body, '\n'))
			{
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				auto p = split(line, '|');
				if (p.size() >= 2 && p[0] == "s")
					peer_state[p[1]] = line;
			}
			break;
		case MsgEvent:
			if (body.compare(0, 6, "resume") == 0)
			{
				// only a guest still in the world we sent this run may skip the resend
				std::string token = body.size() > 7 ? body.substr(7) : "";
				if (!session_token.empty() && token == session_token)
				{
					session_sent = true;
					note(utf8(peer_name) + " вернулся в сессию, мир не перезагружается");
				}
				break;
			}
			if (body.compare(0, 13, "session_ready") == 0)
			{
				// "session_ready|token|mode|save": the files came first (one ordered stream)
				auto p = split(body, '|');
				received_token = p.size() > 1 ? p[1] : "";
				arrived_mode = p.size() > 2 ? p[2] : "join";
				arrived_name = p.size() > 3 ? p[3] : "";
				session_arrived = true;
				log("world " + arrived_name + " came (" + arrived_mode + ")");
				break;
			}
			if (body == "reloading")
			{
				// the guest loads the world just sent: the host loads the same
				if (expect_reloading)
				{
					expect_reloading = false;
					load_own = true;
				}
				break;
			}
			to_game.emplace_back(++to_game_seq, body);
			break;
		case MsgPing:
			if (body.size() == 8)
				queue(MsgPong, body);
			break;
		case MsgPong:
			if (body.size() == 8)
			{
				// the launcher sends .NET ticks; ours are milliseconds since the start of this clock
				u64 sent = *(const u64*)body.data();
				u64 ms = now_ms() - sent;
				if (ms < 60000)
					rtt = (int)ms;
			}
			break;
		case MsgFile:
			if (body.size() > 4)
			{
				u32 name_len = *(const u32*)body.data();
				if (name_len == 0 || 4 + name_len > body.size())
					break;
				std::string name = body.substr(4, name_len);
				size_t slash = name.find_last_of("\\/");
				if (slash != std::string::npos)
					name = name.substr(slash + 1);
				std::string path = saves_dir + name;
				if (FILE* out = fopen(path.c_str(), "wb"))
				{
					fwrite(body.data() + 4 + name_len, 1, body.size() - 4 - name_len, out);
					fclose(out);
					saves_written = true;
				}
				log("received " + name);
			}
			break;
		}
	}

	// ---------------------------------------------------------------- serving one partner
	void run(std::shared_ptr<Link> l)
	{
		{
			std::lock_guard<std::mutex> g(m);
			link = l;
			outbox.clear();
			pending_state.clear();
			resume_sent = false;
			rtt = 0;
		}
		l->send(frame(MsgHello, std::string(PROTOCOL) + "|" + recode(my_name, 1251, CP_UTF8) + "|" + version + "|" + ENGINE_REVISION));

		// the only thread that writes to the link: a slow network delays it, not the game
		std::thread sender([this, l]()
		{
			u64 last_state = 0, last_ping = 0;
			while (running)
			{
				std::string f;
				{
					std::unique_lock<std::mutex> g(m);
					wake_send.wait_for(g, std::chrono::milliseconds(5));
					if (link != l)
						return;
					u64 now = now_ms();
					if (!outbox.empty())
					{
						f = outbox.front();
						outbox.pop_front();
					}
					else if (now - last_ping >= 3000)
					{
						last_ping = now;
						f = frame(MsgPing, std::string((const char*)&now, 8));
					}
					// the game writes the partner's position every 50 ms, everything else every 100 ms
					else if (!pending_state.empty() && now - last_state >= 45)
					{
						last_state = now;
						std::string text;
						for (auto& kv : pending_state)
							text += kv.second + "\n";
						pending_state.clear();
						f = frame(MsgState, text);
					}
				}
				if (!f.empty() && !l->send(f))
				{
					l->close(); // wakes the receive loop, which then cleans up
					return;
				}
			}
		});

		std::string f;
		while (running && l->receive(f))
			if (!f.empty())
				handle(f);

		bool was;
		{
			std::lock_guard<std::mutex> g(m);
			if (link == l)
				link.reset();
			was = connected;
			connected = false;
			theirs = Presence();
			peer_state.clear();
			last_sent_state.clear();
			pending_state.clear();
			outbox.clear();
			rtt = 0;
			state_text = role == "host" ? "listening" : "connecting";
			if (was)
				note(utf8(peer_name) + " отключился" + (l->why().empty() ? "" : " (" + l->why() + ")"));
			else if (!l->why().empty())
				note(l->why());
		}
		wake_send.notify_all();
		l->close();
		sender.join();
	}

	// ---------------------------------------------------------------- the session, on the engine's frame
	std::string session_name() const { return lobby_world.empty() ? "coop_session" : "coop_" + lobby_world; }

	bool save_exists(const std::string& name) const
	{
		return GetFileAttributesA((saves_dir + name + ".scop").c_str()) != INVALID_FILE_ATTRIBUTES;
	}

	void start_save(const std::string& mode, std::vector<std::string>& cmds) // under m
	{
		if (!save_mode.empty())
			return;
		save_name = session_name();
		save_mode = mode;
		DeleteFileA((saves_dir + save_name + ".scop").c_str());
		cmds.push_back("save " + save_name);
		u64 now = now_ms();
		save_retry = now + SAVE_RETRY_MS;
		save_until = now + SAVE_GIVE_UP_MS;
		log("saving " + save_name + " for the guest (" + mode + ")");
	}

	// the guest's game ends with the host's: back to the main menu, which says why (coop_menu reads the note)
	void kick(const std::string& why) // the game's thread, not under m
	{
		log("kicked: " + why);
		call_script("coop_session.engine_before_load", "kick");
		string_path p;
		FS.update_path(p, "$app_data_root$", "coop_kicked.txt");
		if (FILE* f = fopen(p, "wb"))
		{
			std::string text = game_text(why);
			fwrite(text.data(), 1, text.size(), f);
			fclose(f);
		}
		{
			std::lock_guard<std::mutex> g(m);
			session_arrived = false;
			hosted = false;
		}
		// the guest joins again from the menu when the host is back
		stop();
		Console->Execute("disconnect");
	}

	void session_frame()
	{
		if (!running)
			return;
		u64 now = now_ms();
		std::string where = where_now();
		std::string level = level_name();
		std::vector<std::string> cmds;
		std::string kick_why, before_load, sent_mode;
		bool came_up = where == "game" && my_where != "game";
		bool host_role;
		{
			std::lock_guard<std::mutex> g(m);
			host_role = role == "host";
			if (where == "menu")
				hosted = false;

			// where this game is, for the partner
			std::string text = where + "|" + level + "|" + clean(lobby_world) + "|" + (lobby_ready ? "1" : "0") + "|" +
				(hosted ? "1" : "0");
			if (connected && text != sent_presence)
			{
				sent_presence = text;
				queue(MsgPresence, text);
			}
			if (!connected)
				world_sent = false;

			if (host_role)
			{
				// a travel: this game came up on another level (or far away) moments after it was last in a game
				if (came_up)
				{
					level_up_at = now;
					Fvector pos = g_actor->Position();
					bool soon = last_seen && now - last_seen < TRAVEL_RECENT_MS;
					bool moved = !last_level.empty() && (last_level != level || last_pos.distance_to(pos) > TRAVEL_FAR);
					if (!own_load && soon && moved)
					{
						travel = true;
						travel_until = now + TRAVEL_WAIT_MS;
						log("travel " + last_level + " -> " + level);
					}
					own_load = false;
					last_level = level;
					last_pos = pos;
					last_seen = now;
					next_pos = now + 1000;
				}
				if (where == "game" && now >= next_pos)
				{
					// a jump on this level (the PDA's travel to a place in sight) is a travel as well
					Fvector pos = g_actor->Position();
					if (last_level == level && last_pos.distance_to(pos) > JUMP_FAR)
					{
						travel = true;
						travel_until = now + TRAVEL_WAIT_MS;
						log("jump on " + level);
					}
					last_level = level;
					last_pos = pos;
					last_seen = now;
					next_pos = now + 1000;
				}

				bool settled = where == "game" && now - level_up_at > HOST_SETTLE_MS;
				// the guest waits in its menu: it gets the world as it is now (a travel wanted before is done by it)
				if (theirs.where != "menu")
					world_sent = false;
				if (connected && settled && theirs.where == "menu" && theirs.ready && !world_sent)
				{
					world_sent = true;
					travel = false;
					start_save("join", cmds);
				}
				// after a travel, once the guest is in a game and neither is busy: the guest loads the world here
				if (travel && now > travel_until)
					travel = false;
				bool busy = (g_actor && g_actor->IsTalking()) || (CurrentGameUI() && CurrentGameUI()->TopInputReceiver());
				if (travel && connected && settled && theirs.where == "game" && !busy)
				{
					travel = false;
					start_save("guest", cmds);
				}
				if (reload_saved)
				{
					// the scripts made the session save themselves (the last save, Num3): it goes as it is
					reload_saved = false;
					if (connected && save_exists(session_name()))
					{
						send_save(session_name(), "reload");
						sent_mode = "reload";
						expect_reloading = true;
						reload_save = session_name();
					}
				}
				// the save being made: once it is on disk it goes
				if (!save_mode.empty())
				{
					if (save_exists(save_name))
					{
						if (connected)
						{
							send_save(save_name, save_mode);
							sent_mode = save_mode;
							if (save_mode == "reload")
							{
								expect_reloading = true;
								reload_save = save_name;
							}
						}
						save_mode.clear();
					}
					else if (now > save_until)
					{
						note("Сессия не сохранилась, повторите");
						save_mode.clear();
					}
					else if (now > save_retry)
					{
						save_retry = now + SAVE_RETRY_MS;
						cmds.push_back("save " + save_name);
					}
				}
				if (load_own)
				{
					load_own = false;
					own_load = true;
					cmds.push_back("load " + reload_save);
					log("the guest reloads: this game loads " + reload_save + " too");
				}
			}
			else
			{
				// the host's world came: from the menu it loads at once, in a game when it is a reload
				if (session_arrived && where != "loading")
				{
					session_arrived = false;
					rescan_saves();
					if (where == "menu")
					{
						hosted = true;
						before_load = "menu";
						cmds.push_back("disconnect");
						cmds.push_back("start server(" + arrived_name + "/single/alife/load) client(localhost)");
						log("loading the host's world " + arrived_name + " from the menu");
					}
					else if (arrived_mode == "reload" || arrived_mode == "guest")
					{
						if (arrived_mode == "reload")
							queue(MsgEvent, "reloading");
						before_load = "reload";
						load_name = arrived_name;
						load_at = now + 1000; // a moment for the word to leave before loading stops the game
					}
				}
				if (load_at && now > load_at)
				{
					load_at = 0;
					hosted = true;
					cmds.push_back("load " + load_name);
					log("loading the host's world " + load_name);
				}
				// the host left for the main menu, or is gone: this game ends
				if (hosted && where == "game")
				{
					if (their_prev_where == "game" && theirs.where == "menu")
						kick_why = "Хост вышел в главное меню";
					if (connected)
						lost_since = 0;
					else if (!lost_since)
						lost_since = now;
					else if (now - lost_since > LOST_KICK_MS)
						kick_why = "Хост разорвал соединение";
				}
				else
					lost_since = 0;
			}
			if (!theirs.where.empty())
				their_prev_where = theirs.where;
		}
		my_where = where;
		if (!sent_mode.empty())
			call_script("coop_session.engine_world_sent", sent_mode.c_str());
		if (!before_load.empty())
			call_script(before_load == "menu" ? "coop_menu.engine_loading" : "coop_session.engine_before_load", before_load.c_str());
		if (!kick_why.empty())
		{
			kick(kick_why);
			return;
		}
		for (auto& c : cmds)
			Console->Execute(c.c_str());
	}

	// the scripts' wishes (the host's keys)
	void reload(bool save_first) // the game's thread
	{
		std::vector<std::string> cmds;
		{
			std::lock_guard<std::mutex> g(m);
			if (role != "host" || !connected)
				return;
			if (save_first)
				start_save("reload", cmds);
			else
				reload_saved = true;
		}
		for (auto& c : cmds)
			Console->Execute(c.c_str());
	}

	// the scripts moved this game's actor (a test's jump): no travel
	void moved()
	{
		if (g_actor)
		{
			last_pos = g_actor->Position();
			last_level = level_name();
		}
	}

	void start(const std::string& r, LPCSTR name, LPCSTR ver)
	{
		stop();
		steam = false;
		role = r;
		my_name = name ? name : "";
		version = ver ? ver : "";
		char id[16];
		xr_sprintf(id, "%06d", 100000 + (int)(now_ms() % 900000));
		sid = id;
		string_path dir;
		FS.update_path(dir, "$game_saves$", "");
		saves_dir = dir;
		peer_state.clear();
		to_game.clear();
		received_token.clear();
		session_arrived = false;
		session_token.clear();
		theirs = Presence();
		sent_presence.clear();
		world_sent = false;
		save_mode.clear();
		expect_reloading = load_own = reload_saved = false;
		load_at = lost_since = 0;
		running = true;
		register_frame();
		static bool wsa = false;
		if (!wsa)
		{
			WSADATA d;
			wsa = WSAStartup(MAKEWORD(2, 2), &d) == 0;
		}
	}

	void stop()
	{
		running = false;
		{
			std::lock_guard<std::mutex> g(m);
			if (listener != INVALID_SOCKET)
			{
				closesocket(listener);
				listener = INVALID_SOCKET;
			}
			if (link)
				link->close();
		}
		wake_send.notify_all();
		if (net_thread.joinable())
			net_thread.join();
		if (steam)
			coop_steam::leave();
		std::lock_guard<std::mutex> g(m);
		link.reset();
		connected = false;
		// a world that came over the stopped link is not loaded by the main menu
		session_arrived = false;
		state_text = "idle";
	}
};

// never destroyed: its threads may still run while the game quits
Bridge& bridge()
{
	static Bridge* b = new Bridge;
	return *b;
}

// the session's clock: the engine's frame, in the main menu as in a game
struct SessionFrame : pureFrame
{
	void _BCL OnFrame() override { bridge().session_frame(); }
};

void register_frame()
{
	static SessionFrame* f = nullptr;
	if (f)
		return;
	f = new SessionFrame;
	Device.seqFrame.Add(f, REG_PRIORITY_LOW);
}
} // namespace

bool host(int port, LPCSTR name, LPCSTR version)
{
	Bridge& b = bridge();
	b.start("host", name, version);
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_port = htons((u_short)port);
	a.sin_addr.s_addr = INADDR_ANY;
	if (s == INVALID_SOCKET || bind(s, (sockaddr*)&a, sizeof(a)) != 0 || listen(s, 2) != 0)
	{
		if (s != INVALID_SOCKET)
			closesocket(s);
		b.state_text = "error|Порт " + std::to_string(port) + " занят или недоступен";
		b.running = false;
		return false;
	}
	b.listener = s;
	b.state_text = "listening";
	b.log("hosting on port " + std::to_string(port));
	b.net_thread = std::thread([&b, s]()
	{
		while (b.running)
		{
			SOCKET c = accept(s, nullptr, nullptr);
			if (c == INVALID_SOCKET)
				break;
			// one partner at a time: a second one waits until the first leaves
			b.run(std::make_shared<TcpLink>(c));
		}
	});
	return true;
}

bool join(LPCSTR address, int port, LPCSTR name, LPCSTR version)
{
	Bridge& b = bridge();
	b.start("client", name, version);
	b.state_text = "connecting";
	std::string addr = address ? address : "";
	b.log("joining " + addr + ":" + std::to_string(port));
	b.net_thread = std::thread([&b, addr, port]()
	{
		while (b.running)
		{
			addrinfo hints = {}, *res = nullptr;
			hints.ai_family = AF_INET;
			hints.ai_socktype = SOCK_STREAM;
			if (getaddrinfo(addr.c_str(), std::to_string(port).c_str(), &hints, &res) == 0 && res)
			{
				SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
				if (c != INVALID_SOCKET && connect(c, res->ai_addr, (int)res->ai_addrlen) == 0)
				{
					freeaddrinfo(res);
					b.run(std::make_shared<TcpLink>(c));
					res = nullptr;
				}
				else if (c != INVALID_SOCKET)
					closesocket(c);
				if (res)
					freeaddrinfo(res);
			}
			// reconnect: the host may not be up yet, or the link broke
			for (int i = 0; i < 30 && b.running; i++)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	});
	return true;
}

// ------------------------------------------------------------------ Steam
namespace
{
bool steam_start(Bridge& b)
{
	std::string error;
	if (!coop_steam::init(error))
	{
		std::lock_guard<std::mutex> g(b.m);
		b.state_text = "error|" + error;
		b.note(error);
		return false;
	}
	if (b.my_name.empty() || b.my_name == "Stalker")
		b.my_name = recode(coop_steam::my_name(), CP_UTF8, 1251);
	return true;
}
} // namespace

bool host_steam(LPCSTR name, LPCSTR version)
{
	Bridge& b = bridge();
	b.start("host", name, version);
	b.steam = true;
	b.state_text = "starting";
	b.net_thread = std::thread([&b]()
	{
		if (!steam_start(b))
			return;
		// each new peer that talks to us becomes the partner, one at a time
		coop_steam::on_new_peer([&b](std::shared_ptr<Link> l)
		{
			bool busy;
			{
				std::lock_guard<std::mutex> g(b.m);
				busy = !b.running || b.link != nullptr;
			}
			if (busy)
				l->close();
			else
				b.run(l);
		});
		coop_steam::host_lobby(PROTOCOL);
		std::lock_guard<std::mutex> g(b.m);
		b.state_text = "listening";
		b.note("Steam-лобби создано: друг выбирает вас в списке друзей в своей игре или жмёт «Присоединиться» в Steam");
	});
	return true;
}

bool join_steam(LPCSTR lobby_id, LPCSTR name, LPCSTR version)
{
	Bridge& b = bridge();
	b.start("client", name, version);
	b.steam = true;
	b.state_text = "connecting";
	u64 lobby = lobby_id ? _strtoui64(lobby_id, nullptr, 10) : 0;
	b.net_thread = std::thread([&b, lobby]()
	{
		if (!steam_start(b))
			return;
		while (b.running)
		{
			// a connection through Steam's relays times out until the relay network is ready (status 100)
			int relay = coop_steam::relay_status();
			for (int i = 0; i < 600 && b.running && relay != 100 && relay > -100; i++)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				relay = coop_steam::relay_status();
			}
			if (!b.running)
				break;
			u64 owner = coop_steam::join_lobby(lobby);
			if (!owner)
			{
				std::lock_guard<std::mutex> g(b.m);
				b.note("Не удалось войти в лобби друга (он вышел из игры или лобби полное)");
			}
			else
				b.run(coop_steam::connect(owner));
			coop_steam::leave();
			for (int i = 0; i < 30 && b.running; i++)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	});
	return true;
}

LPCSTR steam_friends()
{
	static std::string out;
	out.clear();
	std::string error;
	if (!coop_steam::ready() && !coop_steam::init(error))
		return "";
	for (auto& f : coop_steam::friend_lobbies())
		out += clean(recode(f.name, CP_UTF8, 1251)) + "|" + std::to_string(f.lobby) + "\n";
	return out.c_str();
}

LPCSTR steam_invite()
{
	static string32 out;
	u64 lobby = coop_steam::ready() ? coop_steam::take_join_request() : 0;
	if (lobby)
		xr_sprintf(out, "%llu", lobby);
	else
		out[0] = 0;
	return out;
}

bool take_session()
{
	Bridge& b = bridge();
	std::lock_guard<std::mutex> g(b.m);
	b.rescan_saves();
	bool arrived = b.session_arrived;
	b.session_arrived = false;
	return arrived;
}

void stop() { bridge().stop(); }

void session_reload(bool save_first) { bridge().reload(save_first); }

void session_moved() { bridge().moved(); }

bool active() { return bridge().running; }

void game_out(LPCSTR text)
{
	if (text && bridge().running)
		bridge().game_out(text);
}

LPCSTR game_in() { return bridge().running ? bridge().game_in() : nullptr; }

LPCSTR status()
{
	Bridge& b = bridge();
	static string512 out;
	std::lock_guard<std::mutex> g(b.m);
	if (b.connected)
		xr_sprintf(out, "connected|%s|%d", clean(b.peer_name).c_str(), b.rtt);
	else
		xr_strcpy(out, game_text(b.state_text).c_str());
	return out;
}
} // namespace coop_link
