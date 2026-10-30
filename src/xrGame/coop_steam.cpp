////////////////////////////////////////////////////////////////////////////
//	Module 		: coop_steam.cpp
//	Description : Anomaly Co-op: Steam for the link in the engine (see coop_link_net.h), a port of
//	              CoopLauncher's SteamNet. The flat Steam API is loaded by hand: the game does not link Steam.
//	              Steam binds its callback queue to the thread that initialises it, so one pump thread
//	              does all matchmaking and friends calls; messages are sent from the link's own thread.
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "coop_link_net.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <map>
#include <chrono>

namespace coop_steam
{
namespace
{
typedef unsigned long long u64_;
enum
{
	CbLobbyJoinRequested = 333, CbRichPresenceJoinRequested = 337, CbLobbyEnter = 504, CbLobbyChatUpdate = 506,
	CbLobbyCreated = 513, CbCallCompleted = 703, CbSessionRequest = 1251, CbSessionFailed = 1252
};
const int SendReliableNoNagle = 8 | 1 | 32; // reliable, no Nagle, restart a broken session instead of failing
const int Channel = 0;
const size_t Piece = 60000; // a frame goes in pieces: header byte 0 whole, 1 part, 2 last

struct CallbackMsg
{
	int user;
	int id;
	u8* param;
	int size;
};

// the flat API, by name
struct Api
{
	bool (__cdecl* IsSteamRunning)();
	int (__cdecl* InitFlat)(char* err);
	int (__cdecl* GetHSteamPipe)();
	void (__cdecl* ManualDispatch_Init)();
	void (__cdecl* ManualDispatch_RunFrame)(int);
	bool (__cdecl* ManualDispatch_GetNextCallback)(int, CallbackMsg*);
	void (__cdecl* ManualDispatch_FreeLastCallback)(int);
	bool (__cdecl* ManualDispatch_GetAPICallResult)(int, u64_, void*, int, int, bool*);
	void* (__cdecl* SteamUser)();
	u64_ (__cdecl* User_GetSteamID)(void*);
	void* (__cdecl* SteamMatchmaking)();
	u64_ (__cdecl* CreateLobby)(void*, int, int);
	u64_ (__cdecl* JoinLobby)(void*, u64_);
	void (__cdecl* LeaveLobby)(void*, u64_);
	bool (__cdecl* SetLobbyData)(void*, u64_, const char*, const char*);
	u64_ (__cdecl* GetLobbyOwner)(void*, u64_);
	void* (__cdecl* SteamFriends)();
	bool (__cdecl* SetRichPresence)(void*, const char*, const char*);
	void (__cdecl* ClearRichPresence)(void*);
	int (__cdecl* GetFriendCount)(void*, int);
	u64_ (__cdecl* GetFriendByIndex)(void*, int, int);
	const char* (__cdecl* GetFriendPersonaName)(void*, u64_);
	const char* (__cdecl* GetFriendRichPresence)(void*, u64_, const char*);
	const char* (__cdecl* GetPersonaName)(void*);
	void* (__cdecl* SteamNetworkingMessages)();
	int (__cdecl* SendMessageToUser)(void*, const void*, const void*, u32, int, int);
	int (__cdecl* ReceiveMessagesOnChannel)(void*, int, void**, int);
	bool (__cdecl* AcceptSessionWithUser)(void*, const void*);
	bool (__cdecl* CloseSessionWithUser)(void*, const void*);
	void (__cdecl* Identity_SetSteamID64)(void*, u64_);
	u64_ (__cdecl* Identity_GetSteamID64)(const void*);
	void (__cdecl* Message_Release)(void*);
	void* (__cdecl* SteamNetworkingUtils)();
	int (__cdecl* GetRelayNetworkStatus)(void*, void*);
	void (__cdecl* InitRelayNetworkAccess)(void*);
};

Api api;
void *user, *mm, *friends, *msgs, *utils;
int pipe;
std::atomic<bool> is_ready{false};
std::string persona;
std::thread pump_thread;
std::thread::id pump_id;

std::mutex calls_lock;
std::deque<std::function<void()>> calls;

std::atomic<u64_> current_lobby{0}, last_created{0}, join_request{0}, accept_only{0};
std::string lobby_protocol;

struct Entered
{
	std::mutex m;
	std::condition_variable cv;
	u64_ lobby = 0;
	int response = 0;
} entered;

template <typename F>
bool load(HMODULE dll, F& f, const char* name)
{
	f = (F)GetProcAddress(dll, name);
	return f != nullptr;
}

bool load_api(std::string& error)
{
	HMODULE dll = LoadLibraryA("steam_api64.dll");
	if (!dll)
	{
		error = "не найден steam_api64.dll рядом с игрой";
		return false;
	}
	bool ok = true;
#define L(field, name) ok &= load(dll, api.field, name)
	L(IsSteamRunning, "SteamAPI_IsSteamRunning");
	L(InitFlat, "SteamAPI_InitFlat");
	L(GetHSteamPipe, "SteamAPI_GetHSteamPipe");
	L(ManualDispatch_Init, "SteamAPI_ManualDispatch_Init");
	L(ManualDispatch_RunFrame, "SteamAPI_ManualDispatch_RunFrame");
	L(ManualDispatch_GetNextCallback, "SteamAPI_ManualDispatch_GetNextCallback");
	L(ManualDispatch_FreeLastCallback, "SteamAPI_ManualDispatch_FreeLastCallback");
	L(ManualDispatch_GetAPICallResult, "SteamAPI_ManualDispatch_GetAPICallResult");
	L(SteamUser, "SteamAPI_SteamUser_v023");
	L(User_GetSteamID, "SteamAPI_ISteamUser_GetSteamID");
	L(SteamMatchmaking, "SteamAPI_SteamMatchmaking_v009");
	L(CreateLobby, "SteamAPI_ISteamMatchmaking_CreateLobby");
	L(JoinLobby, "SteamAPI_ISteamMatchmaking_JoinLobby");
	L(LeaveLobby, "SteamAPI_ISteamMatchmaking_LeaveLobby");
	L(SetLobbyData, "SteamAPI_ISteamMatchmaking_SetLobbyData");
	L(GetLobbyOwner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
	L(SteamFriends, "SteamAPI_SteamFriends_v018");
	L(SetRichPresence, "SteamAPI_ISteamFriends_SetRichPresence");
	L(ClearRichPresence, "SteamAPI_ISteamFriends_ClearRichPresence");
	L(GetFriendCount, "SteamAPI_ISteamFriends_GetFriendCount");
	L(GetFriendByIndex, "SteamAPI_ISteamFriends_GetFriendByIndex");
	L(GetFriendPersonaName, "SteamAPI_ISteamFriends_GetFriendPersonaName");
	L(GetFriendRichPresence, "SteamAPI_ISteamFriends_GetFriendRichPresence");
	L(GetPersonaName, "SteamAPI_ISteamFriends_GetPersonaName");
	L(SteamNetworkingMessages, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
	L(SendMessageToUser, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
	L(ReceiveMessagesOnChannel, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
	L(AcceptSessionWithUser, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
	L(CloseSessionWithUser, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");
	L(Identity_SetSteamID64, "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
	L(Identity_GetSteamID64, "SteamAPI_SteamNetworkingIdentity_GetSteamID64");
	L(Message_Release, "SteamAPI_SteamNetworkingMessage_t_Release");
	L(SteamNetworkingUtils, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
	L(GetRelayNetworkStatus, "SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus");
	L(InitRelayNetworkAccess, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
#undef L
	if (!ok)
		error = "steam_api64.dll другой версии";
	return ok;
}

// run on the pump thread and wait (up to 20 s); false if Steam did not get to it
bool on_pump(std::function<void()> f)
{
	if (!is_ready)
		return false;
	if (std::this_thread::get_id() == pump_id)
	{
		f();
		return true;
	}
	auto done = std::make_shared<std::atomic<bool>>(false);
	{
		std::lock_guard<std::mutex> g(calls_lock);
		calls.push_back([f, done]() { f(); *done = true; });
	}
	for (int i = 0; i < 4000 && !*done; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	return *done;
}

struct Identity
{
	char raw[160] = {};
	explicit Identity(u64_ id) { api.Identity_SetSteamID64(raw, id); }
};

////////////////////////////////////////////////////////////////////////// one peer
struct SteamLink : coop_link::Link
{
	u64_ peer;
	std::mutex m;
	std::condition_variable cv;
	std::deque<std::string> frames;
	std::string partial;
	std::atomic<bool> closed{false};
	bool heard = false;
	std::chrono::steady_clock::time_point last_heard = std::chrono::steady_clock::now();
	std::string reason;

	explicit SteamLink(u64_ p) : peer(p) {}
	// not close(): the last reference may go while the peers' map itself is being torn down
	~SteamLink() override { closed = true; }

	void feed(const u8* data, int len) // pump thread
	{
		if (len < 1)
			return;
		std::lock_guard<std::mutex> g(m);
		last_heard = std::chrono::steady_clock::now();
		heard = true;
		if (data[0] == 0)
			frames.emplace_back((const char*)data + 1, len - 1);
		else
		{
			partial.append((const char*)data + 1, len - 1);
			if (data[0] == 2)
			{
				frames.push_back(partial);
				partial.clear();
			}
		}
		cv.notify_one();
	}

	bool send_piece(const std::string& m_)
	{
		Identity id(peer);
		for (int attempt = 0; attempt < 400 && !closed; attempt++)
		{
			int r = api.SendMessageToUser(msgs, id.raw, m_.data(), (u32)m_.size(), SendReliableNoNagle, Channel);
			if (r == 1)
				return true; // k_EResultOK
			if (r != 25 && r != 3)
				return false; // not LimitExceeded / NoConnection yet
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return false;
	}

	bool send(const std::string& f) override
	{
		if (closed)
			return false;
		if (f.size() <= Piece)
			return send_piece(std::string(1, '\0') + f);
		for (size_t off = 0; off < f.size(); off += Piece)
		{
			size_t n = std::min(Piece, f.size() - off);
			std::string piece(1, (char)(off + n >= f.size() ? 2 : 1));
			piece.append(f, off, n);
			if (!send_piece(piece))
			{
				reason = "Steam не доставил сообщение";
				return false;
			}
		}
		return true;
	}

	bool receive(std::string& f) override
	{
		std::unique_lock<std::mutex> g(m);
		for (;;)
		{
			if (closed)
				return false;
			if (!frames.empty())
			{
				f = std::move(frames.front());
				frames.pop_front();
				return true;
			}
			cv.wait_for(g, std::chrono::milliseconds(500));
			// no disconnect event in SteamNetworkingMessages: silence means gone (pings come every 3 s)
			int limit = heard ? 20 : 45;
			if (std::chrono::steady_clock::now() - last_heard > std::chrono::seconds(limit))
			{
				reason = heard ? "напарник не отвечает 20 с" : "нет ответа от хоста за 45 с";
				return false;
			}
		}
	}

	void close() override;
	std::string why() const override { return reason; }
};

// never destroyed: the pump thread may still run while the game quits
std::mutex& links_lock = *new std::mutex;
std::map<u64_, std::shared_ptr<SteamLink>>& links = *new std::map<u64_, std::shared_ptr<SteamLink>>;
std::function<void(std::shared_ptr<coop_link::Link>)>& new_peer = *new std::function<void(std::shared_ptr<coop_link::Link>)>;

void SteamLink::close()
{
	if (closed.exchange(true))
		return;
	cv.notify_all();
	{
		std::lock_guard<std::mutex> g(links_lock);
		auto it = links.find(peer);
		// an old link closing must not drop a newer one to the same peer
		if (it == links.end() || it->second.get() != this)
			return;
		links.erase(it);
	}
	if (is_ready)
	{
		Identity id(peer);
		api.CloseSessionWithUser(msgs, id.raw);
	}
}

void deliver(u64_ from, const u8* data, int len)
{
	std::shared_ptr<SteamLink> l;
	bool fresh = false;
	{
		std::lock_guard<std::mutex> g(links_lock);
		auto it = links.find(from);
		if (it != links.end())
			l = it->second;
		else
		{
			if (accept_only != 0 && from != accept_only)
				return;
			l = std::make_shared<SteamLink>(from);
			links[from] = l;
			fresh = true;
		}
	}
	l->feed(data, len);
	if (fresh && new_peer)
	{
		auto handler = new_peer;
		std::thread([handler, l]() { handler(l); }).detach();
	}
}

std::string friend_name(u64_ id)
{
	const char* n = api.GetFriendPersonaName(friends, id);
	return n && *n ? n : "друга";
}

void handle(int id, const u8* p)
{
	switch (id)
	{
	case CbLobbyCreated:
		{
			int result = *(const int*)p;
			u64_ lobby = *(const u64_*)(p + 8);
			// arrives twice on real Steam (call result + broadcast): handle once
			if (result != 1 || lobby == last_created)
				return;
			last_created = lobby;
			current_lobby = lobby;
			api.SetLobbyData(mm, lobby, "anomaly_coop", lobby_protocol.c_str());
			string64 connect;
			xr_sprintf(connect, "+connect_lobby %llu", lobby);
			api.SetRichPresence(friends, "connect", connect);
			api.SetRichPresence(friends, "status", "Anomaly Co-op: ждёт напарника");
			Msg("~ coop steam: lobby %llu", lobby);
			break;
		}
	case CbLobbyEnter:
		{
			u64_ lobby = *(const u64_*)p;
			int response = *(const int*)(p + 16);
			if (lobby == last_created)
				return; // the host enters its own lobby too
			if (response == 1)
				current_lobby = lobby;
			std::lock_guard<std::mutex> g(entered.m);
			entered.lobby = lobby;
			entered.response = response;
			entered.cv.notify_all();
			break;
		}
	case CbLobbyChatUpdate:
		{
			u64_ changed = *(const u64_*)(p + 8);
			int state = *(const int*)(p + 24);
			if (state & (0x2 | 0x4 | 0x8 | 0x10))
			{
				std::shared_ptr<SteamLink> l;
				{
					std::lock_guard<std::mutex> g(links_lock);
					auto it = links.find(changed);
					if (it != links.end())
						l = it->second;
				}
				if (l)
				{
					l->reason = "напарник вышел из лобби";
					l->close();
				}
			}
			break;
		}
	case CbLobbyJoinRequested:
		join_request = *(const u64_*)p;
		break;
	case CbRichPresenceJoinRequested:
		{
			const char* connect = (const char*)(p + 8);
			if (strncmp(connect, "+connect_lobby ", 15) == 0)
				join_request = _strtoui64(connect + 15, nullptr, 10);
			break;
		}
	case CbSessionRequest:
		{
			u64_ from = api.Identity_GetSteamID64(p);
			// host accepts anyone (the protocol hello decides); a guest accepts only its host
			if (accept_only == 0 || from == accept_only)
			{
				bool ok = api.AcceptSessionWithUser(msgs, p);
				Msg("~ coop steam: incoming connection from %s%s", friend_name(from).c_str(), ok ? "" : " - not accepted");
			}
			break;
		}
	case CbSessionFailed:
		{
			// SteamNetConnectionInfo_t: identity (136), ..., end reason @180, debug text @184
			u64_ from = api.Identity_GetSteamID64(p);
			int reason = *(const int*)(p + 180);
			Msg("~ coop steam: connection with %s failed (%d: %.128s)", friend_name(from).c_str(), reason, (const char*)(p + 184));
			std::shared_ptr<SteamLink> l;
			{
				std::lock_guard<std::mutex> g(links_lock);
				auto it = links.find(from);
				if (it != links.end())
					l = it->second;
			}
			if (l)
			{
				l->reason = "Steam не смог связаться с напарником (код " + std::to_string(reason) + ")";
				l->close();
			}
			break;
		}
	}
}

void pump()
{
	CallbackMsg cb;
	void* inbox[32];
	std::vector<u8> result;
	while (true)
	{
		std::deque<std::function<void()>> todo;
		{
			std::lock_guard<std::mutex> g(calls_lock);
			todo.swap(calls);
		}
		for (auto& f : todo)
			f();
		api.ManualDispatch_RunFrame(pipe);
		while (api.ManualDispatch_GetNextCallback(pipe, &cb))
		{
			if (cb.id == CbCallCompleted)
			{
				u64_ call = *(const u64_*)cb.param;
				int expected = *(const int*)(cb.param + 8);
				int psize = *(const int*)(cb.param + 12);
				result.assign(std::max(psize, 8), 0);
				bool failed = true;
				if (api.ManualDispatch_GetAPICallResult(pipe, call, result.data(), psize, expected, &failed) && !failed)
					handle(expected, result.data());
			}
			else
				handle(cb.id, cb.param);
			api.ManualDispatch_FreeLastCallback(pipe);
		}
		int n = api.ReceiveMessagesOnChannel(msgs, Channel, inbox, 32);
		for (int i = 0; i < n; i++)
		{
			u8* m = (u8*)inbox[i];
			const u8* data = *(u8**)m;
			int len = *(int*)(m + 8);
			u64_ from = api.Identity_GetSteamID64(m + 16);
			deliver(from, data, len);
			api.Message_Release(m);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}
} // namespace

bool init(std::string& error)
{
	static std::mutex once;
	std::lock_guard<std::mutex> g(once);
	// 0 not started, 1 ready, 2 failed, 3 still starting (the thread outlives a call that gave up waiting)
	static std::atomic<int> state{0};
	static std::string fail;
	if (is_ready)
		return true;
	if (state != 3)
	{
		static bool loaded = false;
		if (!loaded && !(loaded = load_api(error)))
			return false;
		SetEnvironmentVariableA("SteamAppId", "480");
		SetEnvironmentVariableA("SteamGameId", "480");
		if (!api.IsSteamRunning())
		{
			error = "Steam не запущен. Запустите Steam и войдите в аккаунт.";
			return false;
		}
		state = 3;
		pump_thread = std::thread([]()
		{
			pump_id = std::this_thread::get_id();
			char err[1024] = {};
			int r = api.InitFlat(err);
			if (r != 0)
			{
				fail = r == 2 ? "Steam не запущен" : r == 3 ? "версия Steam не подходит, обновите Steam" : std::string("Steam не инициализировался: ") + err;
				state = 2;
				return;
			}
			api.ManualDispatch_Init();
			pipe = api.GetHSteamPipe();
			user = api.SteamUser();
			mm = api.SteamMatchmaking();
			friends = api.SteamFriends();
			msgs = api.SteamNetworkingMessages();
			utils = api.SteamNetworkingUtils();
			api.InitRelayNetworkAccess(utils);
			const char* name = api.GetPersonaName(friends);
			persona = name ? name : "";
			is_ready = true;
			state = 1;
			pump();
		});
		pump_thread.detach(); // lives as long as the game
	}
	for (int i = 0; i < 2000 && state == 3; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	if (state == 1)
		return true;
	error = state == 3 ? "Steam не ответил за 20 с" : fail;
	if (state == 2)
		state = 0; // may try again (Steam started later)
	return false;
}

bool ready() { return is_ready; }
std::string my_name() { return persona; }

int relay_status()
{
	int s = -1;
	on_pump([&]() { s = api.GetRelayNetworkStatus(utils, nullptr); });
	return s;
}

void host_lobby(const std::string& protocol)
{
	lobby_protocol = protocol;
	accept_only = 0;
	// room for stale members: a guest that dropped stays listed for a while (the bridge takes one partner)
	on_pump([]() { api.CreateLobby(mm, 1 /* friends only */, 4); });
}

unsigned long long join_lobby(unsigned long long lobby)
{
	{
		std::lock_guard<std::mutex> g(entered.m);
		entered.lobby = 0;
	}
	on_pump([lobby]() { api.JoinLobby(mm, lobby); });
	std::unique_lock<std::mutex> g(entered.m);
	if (!entered.cv.wait_for(g, std::chrono::seconds(15), [lobby]() { return entered.lobby == lobby; }) || entered.response != 1)
		return 0;
	g.unlock();
	u64_ owner = 0;
	on_pump([&]() { owner = api.GetLobbyOwner(mm, lobby); });
	return owner;
}

void leave()
{
	u64_ lobby = current_lobby.exchange(0);
	on_pump([lobby]()
	{
		if (lobby)
			api.LeaveLobby(mm, lobby);
		api.ClearRichPresence(friends);
	});
	std::map<u64_, std::shared_ptr<SteamLink>> old;
	{
		std::lock_guard<std::mutex> g(links_lock);
		old.swap(links);
	}
	for (auto& kv : old)
		kv.second->close();
	accept_only = 0;
	last_created = 0;
}

void set_presence(const std::string& status)
{
	on_pump([status]()
	{
		api.SetRichPresence(friends, "status", status.c_str());
		if (u64_ lobby = current_lobby)
		{
			string64 connect;
			xr_sprintf(connect, "+connect_lobby %llu", lobby);
			api.SetRichPresence(friends, "connect", connect);
		}
	});
}

std::vector<FriendLobby> friend_lobbies()
{
	std::vector<FriendLobby> list;
	on_pump([&]()
	{
		int n = api.GetFriendCount(friends, 4 /* immediate */);
		for (int i = 0; i < n; i++)
		{
			u64_ id = api.GetFriendByIndex(friends, i, 4);
			const char* connect = api.GetFriendRichPresence(friends, id, "connect");
			if (!connect || strncmp(connect, "+connect_lobby ", 15) != 0)
				continue;
			u64_ lobby = _strtoui64(connect + 15, nullptr, 10);
			if (lobby)
				list.push_back({friend_name(id), lobby});
		}
	});
	return list;
}

unsigned long long take_join_request() { return join_request.exchange(0); }

void on_new_peer(std::function<void(std::shared_ptr<coop_link::Link>)> handler) { new_peer = handler; }

std::shared_ptr<coop_link::Link> connect(unsigned long long peer)
{
	accept_only = peer;
	auto l = std::make_shared<SteamLink>(peer);
	std::lock_guard<std::mutex> g(links_lock);
	links[peer] = l;
	return l;
}
} // namespace coop_steam
