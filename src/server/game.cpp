/*
 * Copyright 2008, 2009, Dominik Geyer
 *
 * This file is part of HoldingNuts.
 *
 * HoldingNuts is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HoldingNuts is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HoldingNuts.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *     Dominik Geyer <dominik.geyer@holdingnuts.net>
 */


#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Config.h"
#include "Platform.h"
#include "Network.h"
#include "Debug.h"
#include "Logger.h"
#include "Tokenizer.hpp"
#include "ConfigParser.hpp"

#include "game.hpp"


using namespace std;

extern ConfigParser config;

// temporary buffer for sending messages
#define MSG_BUFFER_SIZE  (1024*16)
static char msg[MSG_BUFFER_SIZE];

static games_type games;
static unsigned int gid_counter = 0;

static clients_type clients;
static unsigned int cid_counter = 0;


static clientconar_type con_archive;
static time_t last_conarchive_cleanup = 0;   // last time scan


GameController* get_game_by_id(int gid)
{
	games_type::const_iterator it = games.find(gid);
	if (it != games.end())
		return it->second;
	else
		return NULL;
}

// for pserver.cpp filling FD_SET
clients_type& get_client_vector()
{
	return clients;
}

clientcon* get_client_by_sock(socktype sock)
{
	for (unsigned int i=0; i < clients.size(); i++)
		if (clients[i].sock == sock)
			return &(clients[i]);
	
	return NULL;
}

clientcon* get_client_by_id(int cid)
{
	for (unsigned int i=0; i < clients.size(); i++)
		if (clients[i].id == cid)
			return &(clients[i]);
	
	return NULL;
}

int send_msg(socktype sock, const char *message)
{
	char buf[MSG_BUFFER_SIZE];
	const int len = snprintf(buf, sizeof(buf), "%s\r\n", message);
	const int bytes = socket_write(sock, buf, len);
	
	// FIXME: send remaining bytes if not all have been sent
	if (len != bytes)
		log_msg("clientsock", "(%d) warning: not all bytes written (%d != %d)", sock, len, bytes);
	
	return bytes;
}

bool send_response(socktype sock, bool is_success, int last_msgid, int code=0, const char *str="")
{
	char buf[512];
	if (last_msgid == -1)
		snprintf(buf, sizeof(buf), "%s %d %s",
			is_success ? "OK" : "ERR", code, str);
	else
		snprintf(buf, sizeof(buf), "%d %s %d %s",
			  last_msgid, is_success ? "OK" : "ERR", code, str);
	
	return send_msg(sock, buf);
}

bool send_ok(clientcon *client, int code=0, const char *str="")
{
	return send_response(client->sock, true, client->last_msgid, code, str);
}

bool send_err(clientcon *client, int code=0, const char *str="")
{
	return send_response(client->sock, false, client->last_msgid, code, str);
}

// from client/foyer to client/foyer
bool client_chat(int from, int to, const char *message)
{
	char msg[128];
	
	if (from == -1)
	{
		snprintf(msg, sizeof(msg), "MSG %d %s %s",
			from, "foyer", message);
	}
	else
	{
		clientcon* fromclient = get_client_by_id(from);
		
		snprintf(msg, sizeof(msg), "MSG %d \"%s\" %s",
			from,
			(fromclient) ? fromclient->name : "???",
			message);
	}
	
	if (to == -1)
	{
		for (clients_type::iterator e = clients.begin(); e != clients.end(); e++)
		{
			if (!(e->state & Introduced))  // do not send broadcast to non-introduced clients
				continue;
			
			send_msg(e->sock, msg);
		}
	}
	else
	{
		clientcon* toclient = get_client_by_id(to);
		if (toclient)
			send_msg(toclient->sock, msg);
		else
			return false;
	}
	
	return true;
}

// from game/table to client
bool client_chat(int from_gid, int from_tid, int to, const char *message)
{
	char msg[128];
	
	snprintf(msg, sizeof(msg), "MSG %d:%d %s %s",
		from_gid, from_tid, (from_tid == -1) ? "game" : "table", message);
	
	clientcon* toclient = get_client_by_id(to);
	if (toclient)
		send_msg(toclient->sock, msg);
	
	return true;
}

// from client to game/table
bool table_chat(int from_cid, int to_gid, int to_tid, const char *message)
{
	char msg[128];
	
	clientcon* fromclient = get_client_by_id(from_cid);
	
	GameController *g = get_game_by_id(to_gid);
	if (!g)
		return false;
	
	vector<int> client_list;
	g->getPlayerList(client_list);
	
	for (unsigned int i=0; i < client_list.size(); i++)
	{
		snprintf(msg, sizeof(msg), "MSG %d:%d:%d \"%s\" %s",
			to_gid, to_tid, from_cid,
			(fromclient) ? fromclient->name : "???",
			message);
		
		clientcon* toclient = get_client_by_id(client_list[i]);
		if (toclient)
			send_msg(toclient->sock, msg);
	}
	
	return true;
}

bool client_snapshot(int from_gid, int from_tid, int to, int sid, const char *message)
{
	snprintf(msg, sizeof(msg), "SNAP %d:%d %d %s",
		from_gid, from_tid, sid, message);
	
	clientcon* toclient = get_client_by_id(to);
	if (toclient)
		send_msg(toclient->sock, msg);
	
	return true;
}

bool client_add(socktype sock, sockaddr_in *saddr)
{
	// drop client if maximum connection count is reached
	if (clients.size() == SERVER_CLIENT_HARDLIMIT || clients.size() == (unsigned int) config.getInt("max_clients"))
	{
		send_response(sock, false, -1, ErrServerFull, "server full");
		socket_close(sock);
		
		return false;
	}
	
	
	// drop client if maximum connections per IP is reached
	const unsigned int connection_max = (unsigned int) config.getInt("max_connections_per_ip");
	if (connection_max)
	{
		unsigned int connection_count = 0;
		for (clients_type::const_iterator client = clients.begin(); client != clients.end(); client++)
		{
			// does the IP match?
			if (!memcmp(&client->saddr.sin_addr, &saddr->sin_addr, sizeof(saddr->sin_addr)))
			{
				if (++connection_count == connection_max)
				{
					send_response(sock, false, -1, ErrMaxConnectionsPerIP, "connection limit per IP is reached");
					socket_close(sock);
					
					return false;
				}
			}
		}
	}
	
	// add the client
	clientcon client;
	memset(&client, 0, sizeof(client));
	client.sock = sock;
	client.saddr = *saddr;
	client.id = -1;
	
	// set initial state
	client.state |= Connected;
	
	clients.push_back(client);
	
	return true;
}

bool client_remove(socktype sock)
{
	for (clients_type::iterator client = clients.begin(); client != clients.end(); client++)
	{
		if (client->sock == sock)
		{
			socket_close(client->sock);
			
			bool send_msg = false;
			if (client->state & SentInfo)
			{
				// remove player from unstarted games
				for (games_type::iterator e = games.begin(); e != games.end(); e++)
				{
					GameController *g = e->second;
					if (!g->isStarted() && g->isPlayer(client->id))
						g->removePlayer(client->id);
				}
				
				
				snprintf(msg, sizeof(msg),
					"%s (%d) left foyer",
					client->name, client->id);
				
				send_msg = true;
				
				// save client-con in archive
				string uuid = client->uuid;
				
				if (uuid.length())
				{
					// FIXME: only add max. 3 entries for each IP
					con_archive[uuid].logout_time = time(NULL);
				}
			}
			
			log_msg("clientsock", "(%d) connection closed", client->sock);
			
			clients.erase(client);
			
			// send client-left-msg to all remaining clients
			if (send_msg)
				client_chat(-1, -1, msg);
			
			break;
		}
	}
	
	return true;
}

int client_cmd_pclient(clientcon *client, Tokenizer &t)
{
	unsigned int version = t.getNextInt();
	string uuid = t.getNext();
	
	if (VERSION_GETMAJOR(version) != VERSION_MAJOR ||
		VERSION_GETMINOR(version) != VERSION_MINOR)
	{
		log_msg("client", "client %d version (%d) doesn't match", client->sock, version);
		send_err(client, ErrWrongVersion, "wrong version");
		client_remove(client->sock);
	}
	else
	{
		// ack the command
		send_ok(client);
		
		
		client->version = version;
		client->state |= Introduced;
		
		snprintf(client->uuid, sizeof(client->uuid), "%s", uuid.c_str());
		
		// re-assign cid if this client was previously connected (and cid isn't already connected)
		bool use_prev_cid = false;
		
		if (uuid.length())
		{
			clientconar_type::iterator it = con_archive.find(uuid);
			
			if (it != con_archive.end())
			{
				clientcon *conc = get_client_by_id(it->second.id);
				if (!conc)
				{
					client->id = it->second.id;
					use_prev_cid = true;
					
					dbg_msg("uuid", "(%d) using previous cid (%d) for uuid '%s'", client->sock, client->id, client->uuid);
				}
				else
				{
					dbg_msg("uuid", "(%d) uuid '%s' already connected; used by cid %d", client->sock, client->uuid, conc->id);
					client->uuid[0] = '\0';    // client is not allowed to use this uuid
				}
			}
			else
				dbg_msg("uuid", "(%d) reserving uuid '%s'", client->sock, client->uuid);
		}
		
		if (!use_prev_cid)
			client->id = cid_counter++;
		
		
		// set temporary client name
		snprintf(client->name, sizeof(client->name), "client_%d", client->id);
		
		
		// send 'introduced response'
		snprintf(msg, sizeof(msg), "PSERVER %d %d",
			VERSION_CREATE(VERSION_MAJOR, VERSION_MINOR, VERSION_REVISION),
			client->id);
			
		send_msg(client->sock, msg);
	}
	
	return 0;
}

int client_cmd_info(clientcon *client, Tokenizer &t)
{
	string infostr;
	Tokenizer it(":");
	
	while (t.getNext(infostr))
	{
		it.parse(infostr);
		
		string infotype, infoarg;
		it.getNext(infotype);
		
		bool havearg = it.getNext(infoarg);
		
		if (infotype == "name" && havearg)
		{
			snprintf(client->name, sizeof(client->name), "%s", infoarg.c_str());
		}
	}
	
	send_ok(client);
	
	if (!(client->state & SentInfo))
	{
		// store UUID in connection-archive
		if (*client->uuid)
		{
			clientcon_archive ar;
			memset(&ar, 0, sizeof(ar));
			ar.id = client->id;
			con_archive[client->uuid] = ar;
		}
		
		// send broadcast message to foyer
		snprintf(msg, sizeof(msg),
			"%s (%d) joined foyer",
			client->name, client->id);
		
		client_chat(-1, -1, msg);
	}
	
	client->state |= SentInfo;
	
	return 0;
}

int client_cmd_chat(clientcon *client, Tokenizer &t)
{
	bool cmderr = false;
	
	if (t.count() < 2)
		cmderr = true;
	else
	{
		// flooding-protection
		int time_since_last_chat = (int) difftime(time(NULL), client->last_chat);
		
		// is the client still muted?
		if (time_since_last_chat < 0)
		{
			send_err(client, 0, "you are still muted");
			return 0;
		}
		
		if ((unsigned int)time_since_last_chat > (unsigned int) config.getInt("flood_chat_interval"))
		{
			// reset flood-measure for new interval
			client->last_chat = time(NULL);
			client->chat_count = 0;
		}
		
		// is client flooding?
		if (++client->chat_count >= (unsigned int) config.getInt("flood_chat_per_interval"))
		{
			log_msg("flooding", "client (%d) caught flooding the chat", client->id);
			
			// mute client for n-seconds
			client->last_chat = time(NULL) + config.getInt("flood_chat_mute");
			client->chat_count = 0;
			
			send_err(client, 0, "you have been muted for some time");
			return 0;
		}
		
		
		Tokenizer ct(":");
		ct.parse(t.getNext());
		string chatmsg = t.getTillEnd();
		
		if (ct.count() == 1) // cid
		{
			int dest = ct.getNextInt();
			
			if (!client_chat(client->id, dest, chatmsg.c_str()))
				cmderr = true;
		}
		else if (ct.count() == 2)  // gid:tid
		{
			int gid = ct.getNextInt();
			int tid = ct.getNextInt();
			
			if (!table_chat(client->id, gid, tid, chatmsg.c_str()))
				cmderr = true;
		}
	}
	
	if (!cmderr)
		send_ok(client);
	else
		send_err(client);
	
	return 0;
}

bool client_cmd_request_gameinfo(clientcon *client, Tokenizer &t)
{
	string sgid;
	while (t.getNext(sgid))   // FIXME: have maximum for count of requests
	{
		const int gid = Tokenizer::string2int(sgid);
		const GameController *g;
		if ((g = get_game_by_id(gid)))
		{
			int game_mode = 0;
			switch ((int)g->getGameType())
			{
			case GameController::SNG:
				game_mode = GameModeSNG;
				break;
			case GameController::FreezeOut:
				game_mode = GameModeFreezeOut;
				break;
			case GameController::RingGame:
				game_mode = GameModeRingGame;
				break;
			}
			
			int state = 0;
			if (g->isStarted())
				state = GameStateStarted;
			else if (g->isEnded())
				state = GameStateEnded;
			else
				state = GameStateWaiting;
			
			snprintf(msg, sizeof(msg),
				"GAMEINFO %d %d:%d:%d:%d:%d:%d \"%s\"",
				gid,
				(int) GameTypeHoldem,
				game_mode,
				state,
				g->getPlayerMax(),
				g->getPlayerCount(),
				g->getPlayerTimeout(),
				g->getName().c_str());
			
			send_msg(client->sock, msg);
		}
	}
	
	return true;
}

bool client_cmd_request_clientinfo(clientcon *client, Tokenizer &t)
{
	string scid;
	while (t.getNext(scid))   // FIXME: have maximum for count of requests
	{
		const socktype cid = Tokenizer::string2int(scid);
		const clientcon *c;
		if ((c = get_client_by_id(cid)))
		{
			snprintf(msg, sizeof(msg),
				"CLIENTINFO %d \"name:%s\"",
				cid,
				c->name);
			
			send_msg(client->sock, msg);
		}
	}
	
	return true;
}

bool client_cmd_request_gamelist(clientcon *client, Tokenizer &t)
{
	string gamelist;
	for (games_type::iterator e = games.begin(); e != games.end(); e++)
	{
		const int gid = e->first;
		
		snprintf(msg, sizeof(msg), "%d ", gid);
		gamelist += msg;
	}
	
	snprintf(msg, sizeof(msg),
		"GAMELIST %s", gamelist.c_str());
	
	send_msg(client->sock, msg);
	
	return true;
}

bool client_cmd_request_playerlist(clientcon *client, Tokenizer &t)
{
	int gid;
	t >> gid;
	
	GameController *g = get_game_by_id(gid);
	if (!g)
		return false;
	
	vector<int> client_list;
	g->getPlayerList(client_list);
	
	string slist;
	for (unsigned int i=0; i < client_list.size(); i++)
	{
		snprintf(msg, sizeof(msg), "%d ", client_list[i]);
		slist += msg;
	}
	
	snprintf(msg, sizeof(msg), "PLAYERLIST %d %s", gid, slist.c_str());
	send_msg(client->sock, msg);
	
	return true;
}

int client_cmd_request(clientcon *client, Tokenizer &t)
{
	if (!t.count())
	{
		send_err(client, ErrParameters);
		return 1;
	}
	
	bool cmderr = false;
	
	string request;
	t >> request;
	
	if (request == "clientinfo")
		cmderr = !client_cmd_request_clientinfo(client, t);
	else if (request == "gameinfo")
		cmderr = !client_cmd_request_gameinfo(client, t);
	else if (request == "gamelist")
		cmderr = !client_cmd_request_gamelist(client, t);
	else if (request == "playerlist")
		cmderr = !client_cmd_request_playerlist(client, t);
	else if (request == "serverinfo")
	{
		snprintf(msg, sizeof(msg), "games=%d clients=%d con_archive=%d",
			(int) games.size(),
			(int) clients.size(),
			(int) con_archive.size());
		
		client_chat(-1, client->id, msg);
	}
	else
		cmderr = true;

	
	if (!cmderr)
		send_ok(client);
	else
		send_err(client);
	
	return 0;
}

int client_cmd_register(clientcon *client, Tokenizer &t)
{
	if (!t.count())
	{
		send_err(client, ErrParameters);
		return 1;
	}
	
	int gid;
	t >> gid;
	
	GameController *g = get_game_by_id(gid);
	if (!g)
	{
		send_err(client, 0 /*FIXME*/, "game does not exist");
		return 1;
	}
	
	if (g->isStarted())
	{
		send_err(client, 0 /*FIXME*/, "game has already been started");
		return 1;
	}
	
	if (g->isPlayer(client->id))
	{
		send_err(client, 0 /*FIXME*/, "you are already registered");
		return 1;
	}
	
	// check for max-games-register limit
	unsigned int register_limit = config.getInt("max_register_per_player");
	unsigned int count = 0;
	for (games_type::const_iterator e = games.begin(); e != games.end(); e++)
	{
		GameController *g = e->second;
		
		if (g->isPlayer(client->id))
		{
			if (++count == register_limit)
			{
				send_err(client, 0 /*FIXME*/, "register limit per player is reached");
				return 1;
			}
		}
	}
	
	if (!g->addPlayer(client->id))
	{
		send_err(client, 0 /*FIXME*/, "unable to register");
		return 1;
	}
	
	
	snprintf(msg, sizeof(msg),
		"%s (%d) joined game %d (%d/%d)",
		client->name, client->id, gid,
		g->getPlayerCount(), g->getPlayerMax());
	
	log_msg("game", "%s", msg);
	client_chat(-1, -1, msg);
	
	send_ok(client);
	
	return 0;
}

int client_cmd_unregister(clientcon *client, Tokenizer &t)
{
	if (!t.count())
	{
		send_err(client, ErrParameters);
		return 1;
	}
	
	int gid;
	t >> gid;
	
	GameController *g = get_game_by_id(gid);
	if (!g)
	{
		send_err(client, 0 /*FIXME*/, "game does not exist");
		return 1;
	}
	
	if (g->isStarted())
	{
		send_err(client, 0 /*FIXME*/, "game has already been started");
		return 1;
	}
	
	if (!g->isPlayer(client->id))
	{
		send_err(client, 0 /*FIXME*/, "you are not registered");
		return 1;
	}
	
	if (!g->removePlayer(client->id))
	{
		send_err(client, 0 /*FIXME*/, "unable to unregister");
		return 1;
	}
	
	
	snprintf(msg, sizeof(msg),
		"%s (%d) parted game %d (%d/%d)",
		client->name, client->id, gid,
		g->getPlayerCount(), g->getPlayerMax());
	
	log_msg("game", "%s", msg);
	client_chat(-1, -1, msg);
	
	send_ok(client);
	
	return 0;
}

int client_cmd_action(clientcon *client, Tokenizer &t)
{
	if (t.count() < 2)
	{
		send_err(client, ErrParameters);
		return 1;
	}
	
	int gid;
	string action;
	float amount;
	
	t >> gid >> action >> amount;
	
	
	GameController *g = get_game_by_id(gid);
	if (!g)
	{
		send_err(client, 0 /* FIXME */, "game does not exist");
		return 1;
	}
	
	Player::PlayerAction a = Player::None;
	
	if (action == "check")
		a = Player::Check;
	else if (action == "fold")
		a = Player::Fold;
	else if (action == "call")
		a = Player::Call;
	else if (action == "bet")
		a = Player::Bet;
	else if (action == "raise")
		a = Player::Raise;
	else if (action == "allin")
		a = Player::Allin;
	else if (action == "show")
		a = Player::Show;
	else if (action == "muck")
		a = Player::Muck;
	else if (action == "sitout")
		a = Player::Sitout;
	else if (action == "back")
		a = Player::Back;
	else if (action == "reset")
		a = Player::ResetAction;
	else
	{
		send_err(client, ErrParameters);
		return 1;
	}
	
	
	g->setPlayerAction(client->id, a, amount);
	
	send_ok(client);
	
	return 0;
}

int client_cmd_create(clientcon *client, Tokenizer &t)
{
	if (!config.getBool("perm_create_user") && !(client->state & Authed))
	{
		send_err(client, ErrNoPermission, "no permission");
		return -1;
	}
	
	// FIXME: check user-create limit
	// FIXME: check game-count limit
	
	bool cmderr = false;
	
	struct {
		string name;
		int type;
		unsigned int max_players;
		unsigned int stake;
		unsigned int timeout;
	} ginfo = {
		"user_game",
		10,
		GameController::SNG,
		1500,
		30
	};
	
	
	string infostr;
	Tokenizer it(":");
	
	while (t.getNext(infostr))
	{
		it.parse(infostr);
		
		string infotype, infoarg;
		it.getNext(infotype);
		
		bool havearg = it.getNext(infoarg);
		
		if (infotype == "type" && havearg)
		{
			ginfo.type = Tokenizer::string2int(infoarg);
			
			// TODO: no other gamesmodes supported yet
			if (ginfo.type != GameController::SNG)
				cmderr = true;
		}
		else if (infotype == "players" && havearg)
		{
			ginfo.max_players = Tokenizer::string2int(infoarg);
			
			if (ginfo.max_players < 2 || ginfo.max_players > 10)
				cmderr = true;
		}
		else if (infotype == "stake" && havearg)
		{
			ginfo.stake = Tokenizer::string2int(infoarg);
			
			if (ginfo.stake < 10)
				cmderr = true;
		}
		else if (infotype == "timeout" && havearg)
		{
			ginfo.timeout = Tokenizer::string2int(infoarg);
			
			if (ginfo.timeout < 10 || ginfo.timeout > 5*60)
				cmderr = true;
		}
		else if (infotype == "name" && havearg)
		{
			if (infoarg.length() > 50)
				infoarg = string(infoarg, 0, 50);
			
			ginfo.name = infoarg;
		}
	}
	
	if (!cmderr)
	{
		GameController *g = new GameController();
		const int gid = ++gid_counter;
		g->setGameId(gid);
		g->setPlayerMax(ginfo.max_players);
		g->setPlayerTimeout(ginfo.timeout);
		g->setPlayerStakes(ginfo.stake);
		g->addPlayer(client->id);
		g->setOwner(client->id);
		g->setName(ginfo.name);
		games[gid] = g;
		
		send_ok(client);
		
		snprintf(msg, sizeof(msg), "Your game '%d' has been created.", gid);
		client_chat(-1, client->id, msg);
	}
	else
		send_err(client);
	
	return 0;
}

int client_cmd_auth(clientcon *client, Tokenizer &t)
{
	bool cmderr = true;
	
	if (t.count() >= 2 && config.get("auth_password").length())
	{
		int type = t.getNextInt();
		string passwd = t.getNext();
		
		// -1 is server-auth, anything other is game-auth
		if (type == -1)
		{
			if (passwd == config.get("auth_password"))
			{
				client->state |= Authed;
				
				cmderr = false;
			}
		}
		else
		{
			// TODO: game-auth
		}
	}
	
	if (!cmderr)
		send_ok(client);
	else
		send_err(client, 0, "auth failed");
	
	return 0;
}

int client_execute(clientcon *client, const char *cmd)
{
	Tokenizer t(" ");
	t.parse(cmd);  // parse the command line
	
	// ignore blank command
	if (!t.count())
		return 0;
	
	//dbg_msg("clientsock", "(%d) executing '%s'", client->sock, cmd);
	
	// FIXME: could be done better...
	// extract message-id if present
	const char firstchar = t[0][0];
	if (firstchar >= '0' && firstchar <= '9')
		client->last_msgid = t.getNextInt();
	else
		client->last_msgid = -1;
	
	
	// get command argument
	const string command = t.getNext();
	
	
	if (!(client->state & Introduced))  // state: not introduced
	{
		if (command == "PCLIENT")
			return client_cmd_pclient(client, t);
		else
		{
			// seems not to be a pclient
			send_err(client, ErrProtocol, "protocol error");
			return -1;
		}
	}
	else if (command == "INFO")
		return client_cmd_info(client, t);
	else if (command == "CHAT")
		return client_cmd_chat(client, t);
	else if (command == "REQUEST")
		return client_cmd_request(client, t);
	else if (command == "REGISTER")
		return client_cmd_register(client, t);
	else if (command == "UNREGISTER")
		return client_cmd_unregister(client, t);
	else if (command == "ACTION")
		return client_cmd_action(client, t);
	else if (command == "CREATE")
		return client_cmd_create(client, t);
	else if (command == "AUTH")
		return client_cmd_auth(client, t);
	else if (command == "QUIT")
	{
		send_ok(client);
		return -1;
	}
	else
		send_err(client, ErrNotImplemented, "not implemented");
	
	return 0;
}

// returns zero if no cmd was found or no bytes remaining after exec
int client_parsebuffer(clientcon *client)
{
	//log_msg("clientsock", "(%d) parse (bufferlen=%d)", client->sock, client->buflen);
	
	int found_nl = -1;
	for (int i=0; i < client->buflen; i++)
	{
		if (client->msgbuf[i] == '\r')
			client->msgbuf[i] = ' ';  // space won't hurt
		else if (client->msgbuf[i] == '\n')
		{
			found_nl = i;
			break;
		}
	}
	
	int retval = 0;
	
	// is there a command in queue?
	if (found_nl != -1)
	{
		// extract command
		char cmd[sizeof(client->msgbuf)];
		memcpy(cmd, client->msgbuf, found_nl);
		cmd[found_nl] = '\0';
		
		//log_msg("clientsock", "(%d) command: '%s' (len=%d)", client->sock, cmd, found_nl);
		if (client_execute(client, cmd) != -1)  // client quitted ?
		{
			// move the rest to front
			memmove(client->msgbuf, client->msgbuf + found_nl + 1, client->buflen - (found_nl + 1));
			client->buflen -= found_nl + 1;
			//log_msg("clientsock", "(%d) new buffer after cmd (bufferlen=%d)", client->sock, client->buflen);
			
			retval = client->buflen;
		}
		else
		{
			client_remove(client->sock);
			retval = 0;
		}
	}
	else
		retval = 0;
	
	return retval;
}

int client_handle(socktype sock)
{
	char buf[1024];
	int bytes;
	
	// return early on client close/error
	if ((bytes = socket_read(sock, buf, sizeof(buf))) <= 0)
		return bytes;
	
	
	//log_msg("clientsock", "(%d) DATA len=%d", sock, bytes);
	
	clientcon *client = get_client_by_sock(sock);
	if (!client)
	{
		log_msg("clientsock", "(%d) error: no client associated", sock);
		return -1;
	}
	
	if (client->buflen + bytes > (int)sizeof(client->msgbuf))
	{
		log_msg("clientsock", "(%d) error: buffer size exceeded", sock);
		client->buflen = 0;
	}
	else
	{
		memcpy(client->msgbuf + client->buflen, buf, bytes);
		client->buflen += bytes;
		
		// parse and execute all commands in queue
		while (client_parsebuffer(client));
	}
	
	return bytes;
}

void remove_expired_conar_entries()
{
	time_t curtime = time(NULL);
	unsigned int expire = config.getInt("conarchive_expire");
	
	for (clientconar_type::iterator e = con_archive.begin(); e != con_archive.end();)
	{
		clientcon_archive *conar = &(e->second);
		
		if (conar->logout_time && (unsigned int)difftime(curtime, conar->logout_time) > expire)
		{
			dbg_msg("clientar", "removing expired entry %s", e->first.c_str());
			con_archive.erase(e++);
		}
		else
			++e;
	}
}

int gameloop()
{
#ifdef DEBUG
	// initially add games for debugging purpose
	if (!games.size())
	{
		for (int i=0; i < config.getInt("dbg_testgame_games"); i++)
		{
			GameController *g = new GameController();
			const int gid = i;
			g->setGameId(gid);
			g->setName("HoldingNuts test game");
			g->setRestart(true);
			g->setPlayerMax(config.getInt("dbg_testgame_players"));
			g->setPlayerTimeout(config.getInt("dbg_testgame_timeout"));
			g->setPlayerStakes(config.getInt("dbg_testgame_stakes"));
			
			if (config.getBool("dbg_stresstest") && i > 10)
			{
				for (int j=0; j < config.getInt("dbg_testgame_players"); j++)
					g->addPlayer(j*1000 + i);
			}
			
			games[gid] = g;
			
			gid_counter++;
		}
	}
#endif
	
	
	// handle all games
	for (games_type::iterator e = games.begin(); e != games.end();)
	{
		GameController *g = e->second;
		if (g->tick() < 0)
		{
			// replicate game if "restart" is set
			if (g->getRestart())
			{
				const int gid = ++gid_counter;
				GameController *newgame = new GameController();
				
				newgame->setGameId(gid);
				newgame->setName(g->getName());
				newgame->setRestart(true);
				newgame->setPlayerMax(g->getPlayerMax());
				newgame->setPlayerTimeout(g->getPlayerTimeout());
				newgame->setPlayerStakes(g->getPlayerStakes());
				games[gid] = newgame;
				
				log_msg("game", "restarted game (old: %d, new: %d)",
					g->getGameId(), newgame->getGameId());
			}
			
			log_msg("game", "game %d has been deleted", g->getGameId());
			
			delete g;
			games.erase(e++);
		}
		else
			++e;
	}
	
	
	// delete all expired archived connection-data (con_archive)
	if ((unsigned int)difftime(time(NULL), last_conarchive_cleanup) > 5 * 60)
	{
		dbg_msg("clientar", "scanning for expired entries");
		remove_expired_conar_entries();
		
		last_conarchive_cleanup = time(NULL);
	}
	
	return 0;
}
