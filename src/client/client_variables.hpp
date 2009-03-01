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
 */


// Client defaults

config.set("default_host",	"localhost");		// default host to connect to
config.set("default_port",	DEFAULT_SERVER_PORT);	// default port to connect to
config.set("player_name",	"Unnamed");		// the player name
config.set("uuid",		"");			// unique ID for re-connect
config.set("locale",		"");			// language/locale to use
config.set("log",		true);			// log to file
config.set("ui_show_handstrength", 	true);		// display hand strength on table
config.set("sound",		true);			// play sounds
config.set("sound_focus",	true);			// only play sound if window has focus

#ifdef DEBUG
config.set("dbg_register",	-1);			// automatically connect and register to a game (value is gid)
config.set("dbg_bbox",		false);			// show bounding boxes around scene items
config.set("dbg_srv_cmd",	false);			// log every message from server
#endif
