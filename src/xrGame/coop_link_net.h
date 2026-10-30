////////////////////////////////////////////////////////////////////////////
//	Module 		: coop_link_net.h
//	Description : Anomaly Co-op: one connection to the partner, whatever carries it (TCP, Steam).
////////////////////////////////////////////////////////////////////////////

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace coop_link
{
struct Link
{
	virtual ~Link() {}
	virtual bool send(const std::string& frame) = 0;
	virtual bool receive(std::string& frame) = 0; // blocks; false = gone
	virtual void close() = 0;
	// why it went, for the player ("" when it just closed)
	virtual std::string why() const { return ""; }
};
} // namespace coop_link

// Steam (AppID 480, as CoopLauncher): friends-only lobbies, friends' rich presence, peer-to-peer messages
// through Steam's relays. steam_api64.dll is loaded from the exe's folder when first needed.
namespace coop_steam
{
bool init(std::string& error); // blocks up to 20 s on the first call
bool ready();
std::string my_name();
int relay_status(); // 100 = ready

void host_lobby(const std::string& protocol);
// blocks up to 15 s; the lobby owner, 0 on failure
unsigned long long join_lobby(unsigned long long lobby);
void leave();
void set_presence(const std::string& status);

struct FriendLobby
{
	std::string name; // UTF-8
	unsigned long long lobby;
};
std::vector<FriendLobby> friend_lobbies();
// a lobby a friend invited us to, or that "Join game" in Steam asked for; 0 none (cleared when read)
unsigned long long take_join_request();

// host: each new peer that talks to us; guest: only the host is accepted
void on_new_peer(std::function<void(std::shared_ptr<coop_link::Link>)> handler);
std::shared_ptr<coop_link::Link> connect(unsigned long long peer);
} // namespace coop_steam
