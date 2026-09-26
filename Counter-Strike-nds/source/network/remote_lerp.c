// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// Smooth motion for other online players between their position snapshots. See
// remote_lerp.h.

#include "remote_lerp.h"

static int clampInt(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void RemoteLerp_Push(RemoteLerp *lerp, const float current[3], const float to[3], int now)
{
    // A player standing still sends nothing, so a long gap says nothing about the pace of
    // the motion that follows it: use the relay's usual interval then.
    int gap = now - lerp->lastPush;
    bool known = lerp->pushed && gap > 0 && gap < REMOTE_LERP_MAX_FRAMES;
    int duration = REMOTE_LERP_FIRST_FRAMES;
    if (known)
    {
        int longer = gap > lerp->lastGap ? gap : lerp->lastGap;
        duration = clampInt(longer + 1, REMOTE_LERP_MIN_FRAMES, REMOTE_LERP_MAX_FRAMES);
    }
    lerp->lastGap = known ? gap : 0;

    for (int k = 0; k < 3; k++)
    {
        lerp->from[k] = current[k];
        lerp->to[k] = to[k];
    }
    lerp->start = now;
    lerp->duration = duration;
    lerp->lastPush = now;
    lerp->pushed = true;
}

void RemoteLerp_Snap(RemoteLerp *lerp, const float to[3], int now)
{
    for (int k = 0; k < 3; k++)
        lerp->from[k] = lerp->to[k] = to[k];
    lerp->start = now;
    lerp->duration = 1;
    lerp->lastPush = now;
    lerp->lastGap = 0;
    lerp->pushed = false;
}

void RemoteLerp_Sample(const RemoteLerp *lerp, int now, float out[3])
{
    // The frame a snapshot arrives already takes the first step: the snapshot is handled
    // before the players move, so starting from zero would hold the player still for a frame
    // at every packet.
    int step = now - lerp->start + 1;
    float t = 1.0f;
    if (step > 0 && step < lerp->duration)
        t = (float)step / (float)lerp->duration;
    for (int k = 0; k < 3; k++)
        out[k] = lerp->from[k] + (lerp->to[k] - lerp->from[k]) * t;
}

bool RemoteLerp_Targets(const RemoteLerp *lerp, const float to[3])
{
    return lerp->to[0] == to[0] && lerp->to[1] == to[1] && lerp->to[2] == to[2];
}
