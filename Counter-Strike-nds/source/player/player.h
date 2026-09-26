// SPDX-License-Identifier: MIT
//
// Copyright (c) 2021-2022, Fewnity - Grégory Machefer
//
// This file is part of Counter Strike Nintendo DS Multiplayer Edition (CS:DS)

#ifndef PLAYER_H_ /* Include guard */
#define PLAYER_H_

#include "main.h"


void killPlayer(Player *player);
void makeHit(int hitBy, int playerHit, float distance, int shootIndex);
void buyGun();
void CalculatePlayerPosition(int PlayerIndex);
void setPlayerPositionAtSpawns(int playerIndex);
void setNewRoundHandWeapon();
void SetGunInInventory(int Value, int inventoryIndex);
void SetGunInInventoryForNonLocalPlayer(int playerIndex, int Value, int inventoryIndex);
void ChangeGunInInventoryForLocalPlayer(int Left);
void ChangeGunInInventory(int playerIndex, int Left);
void setSelectedGunInInventory(int playerIndex, int gunIndex);
void playerUpdate();
void removeAllPlayers();
int AddNewPlayer(int NewId, bool IsLocalPlayer, bool isAI);
void UpdatePlayerTexture(int playerIndex);
void setPlayerHealth(int playerIndex, int health);
void resetPlayer(int index);
void reducePlayerMoney(int playerIndex, int Money);
void addPlayerMoney(int playerIndex, int Money);
void setPlayerMoney(int playerIndex, int Money);
void addMoneyToTeam(int Money, enum teamEnum Team);
void setBombForARandomPlayer();
void setShopZone(Player *player);

/**
 * @brief Eye height above the hull centre, for whichever player is acting.
 *
 * This is not a camera value: it sets the origin of every bullet raycast, the
 * AI's line of sight and grenade throws. It is actor-relative, so a crouching
 * shooter fires from a lower point -- otherwise you could hide behind cover and
 * still shoot over it. Only the local player can crouch in this version, so
 * every other player returns the standing height and behaves exactly as before.
 */
float PlayerEyeOffset(const Player *player);

/** @brief The same, premultiplied by 4096 for the integer raycast paths. */
int PlayerEyeOffsetF32(const Player *player);

/**
 * @brief How far below the hull centre a dropped bomb sits, in world units.
 *
 * `position.y` is the hull CENTRE, and crouching lowers the centre while the
 * feet stay put, so the old hard-coded 0.845 buried a crouch-planted bomb under
 * the floor. Actor-relative, like PlayerEyeOffset().
 */
float PlayerFootOffset(const Player *player);

#endif // PLAYER_H_