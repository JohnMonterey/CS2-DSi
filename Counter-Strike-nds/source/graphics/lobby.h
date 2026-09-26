// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// The singleplayer lobby: your character on the top screen, the match setup below.

#ifndef LOBBY_H_ /* Include guard */
#define LOBBY_H_

#include <stdbool.h>

// The side picked in the lobby for the match it has just started, SPECTATOR when there is
// none. checkStartGameLoop() applies it once the players exist, instead of asking.
extern int lobbyPendingTeam;

void initLobbyMenu();

// Input and animation. Once per frame, while the lobby is the current menu.
void Lobby_Update(void);

// Draws the top screen and returns true while the lobby is open.
bool drawLobbyTopScreen(void);

#endif // LOBBY_H_
