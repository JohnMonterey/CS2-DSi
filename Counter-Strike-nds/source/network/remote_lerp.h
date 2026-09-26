// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// Smooth motion for other online players between their position snapshots.

#ifndef REMOTE_LERP_H_ /* Include guard */
#define REMOTE_LERP_H_

#include <stdbool.h>

/*
 * No Nitro Engine, no libnds: this compiles for the host test runner too (make test-anim).
 *
 * The server relays each player's position about ten times a second. Each new snapshot
 * starts a straight segment from wherever the player is drawn now to the snapshot, timed
 * to last as long as the longer of the last two gaps between snapshots, plus a frame. So a
 * player moving steadily is drawn moving steadily, one snapshot behind. The server's 30 Hz
 * loop spaces snapshots 6 and 8 frames apart in turn, and taking the longer gap means the
 * next one arrives before the segment runs out rather than after the player has stopped;
 * a burst of two snapshots does not make a sprint either.
 *
 * Times are in frames (network.h's frameCount); positions are world units.
 */

#define REMOTE_LERP_FIRST_FRAMES 6 // no usable gap measured: the relay's usual 100 ms
#define REMOTE_LERP_MIN_FRAMES 2
#define REMOTE_LERP_MAX_FRAMES 20

typedef struct
{
    float from[3];
    float to[3];
    int start;    // frame the segment began
    int duration; // frames it lasts
    int lastPush; // frame the previous snapshot arrived
    int lastGap;  // frames between the two before that, 0 when unknown
    bool pushed;  // a snapshot has arrived since the last snap
} RemoteLerp;

// A snapshot arrived at frame `now` while the player was drawn at `current`.
void RemoteLerp_Push(RemoteLerp *lerp, const float current[3], const float to[3], int now);

// Jump straight to `to`: first sight, respawns, anything that teleports.
void RemoteLerp_Snap(RemoteLerp *lerp, const float to[3], int now);

// Where to draw the player at frame `now`. A `now` before the segment began (the frame
// counter was reset) counts as finished.
void RemoteLerp_Sample(const RemoteLerp *lerp, int now, float out[3]);

// False when the game has since put the player's destination somewhere else itself.
bool RemoteLerp_Targets(const RemoteLerp *lerp, const float to[3]);

#endif // REMOTE_LERP_H_
