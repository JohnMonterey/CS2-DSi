// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// movement.cfg: CS:GO-unit tuning values, reloadable without a rebuild.

#include "movement_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Values are held in the file in CS:GO's own units -- friction = 5.2, not the
 * Q16 per-tick multiplier it becomes. That way the file reads like a CS config
 * and can be compared against a real one, and every conversion lives in exactly
 * one place (PM_TunablesFromCS).
 */

typedef struct
{
    const char *key;
    size_t offset;
} MovementCfgKey;

#define CFG_FIELD(name) offsetof(PM_CSTunables, name)

static const MovementCfgKey kKeys[] = {
    {"gravity", CFG_FIELD(gravity)},
    {"jump_impulse", CFG_FIELD(jumpImpulse)},
    {"stopspeed", CFG_FIELD(stopSpeed)},
    {"air_speed_cap", CFG_FIELD(airSpeedCap)},
    {"friction", CFG_FIELD(friction)},
    {"accelerate", CFG_FIELD(accelerate)},
    {"airaccelerate", CFG_FIELD(airAccelerate)},
    {"stamina_max", CFG_FIELD(staminaMax)},
    {"stamina_range", CFG_FIELD(staminaRange)},
    {"stamina_recovery", CFG_FIELD(staminaRecovery)},
    {"stamina_jumpcost", CFG_FIELD(staminaJumpCost)},
    {"stamina_landcost", CFG_FIELD(staminaLandCost)},
    {"duck_modifier", CFG_FIELD(duckModifier)},
    {"bhop_factor", CFG_FIELD(bhopFactor)},
    {"duck_speed", CFG_FIELD(duckSpeedIdeal)},
    {"duck_spam_penalty", CFG_FIELD(duckSpamPenalty)},
};

#define CFG_KEY_COUNT ((int)(sizeof(kKeys) / sizeof(kKeys[0])))

static char s_lastPath[64] = "";

static bool cfg_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' ||
           c == '\f';
}

/** @brief Case-insensitive compare of a bounded span against a C string. */
static bool cfg_key_matches(const char *span, size_t len, const char *key)
{
    size_t i;

    for (i = 0; i < len; i++)
    {
        char a = span[i];
        char b = key[i];

        if (b == '\0')
            return false;
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (a != b)
            return false;
    }
    return key[len] == '\0';
}

void MovementCfg_ParseBuffer(const char *text, size_t len, PM_CSTunables *cs,
                             MovementCfgResult *res)
{
    res->applied = 0;
    res->unknown = 0;
    res->malformed = 0;
    res->truncated = false;

    size_t i = 0;
    while (i < len)
    {
        /* Take one line. */
        size_t lineStart = i;
        while (i < len && text[i] != '\n')
            i++;
        size_t lineEnd = i;
        if (i < len)
            i++; /* step over the newline */

        /* Strip a trailing comment, then trim. */
        for (size_t c = lineStart; c < lineEnd; c++)
        {
            if (text[c] == '#' || text[c] == ';')
            {
                lineEnd = c;
                break;
            }
        }
        while (lineStart < lineEnd && cfg_is_space(text[lineStart]))
            lineStart++;
        while (lineEnd > lineStart && cfg_is_space(text[lineEnd - 1]))
            lineEnd--;

        if (lineStart == lineEnd)
            continue; /* blank or comment-only */

        /* Split on the first '='. */
        size_t eq = lineStart;
        while (eq < lineEnd && text[eq] != '=')
            eq++;
        if (eq == lineEnd)
        {
            res->malformed++;
            continue;
        }

        size_t keyEnd = eq;
        while (keyEnd > lineStart && cfg_is_space(text[keyEnd - 1]))
            keyEnd--;
        size_t valStart = eq + 1;
        while (valStart < lineEnd && cfg_is_space(text[valStart]))
            valStart++;

        if (keyEnd == lineStart || valStart == lineEnd)
        {
            res->malformed++;
            continue;
        }

        /* strtod needs a terminator, and the buffer is not ours to modify. */
        char valueText[32];
        size_t valLen = lineEnd - valStart;
        if (valLen >= sizeof(valueText))
            valLen = sizeof(valueText) - 1;
        memcpy(valueText, text + valStart, valLen);
        valueText[valLen] = '\0';

        char *endPtr = NULL;
        double value = strtod(valueText, &endPtr);
        if (endPtr == valueText)
        {
            res->malformed++;
            continue;
        }

        size_t keyLen = keyEnd - lineStart;
        bool matched = false;
        for (int k = 0; k < CFG_KEY_COUNT; k++)
        {
            if (!cfg_key_matches(text + lineStart, keyLen, kKeys[k].key))
                continue;

            *(float *)((char *)cs + kKeys[k].offset) = (float)value;
            res->applied++;
            matched = true;
            break;
        }
        if (!matched)
            res->unknown++;
    }
}

bool MovementCfg_LoadPath(const char *path, PM_CSTunables *cs,
                          MovementCfgResult *res)
{
    /* The ARM9 stack lives in DTCM and is only about 16 KB, so this buffer is
     * static rather than automatic. */
    static char buffer[8192];

    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return false;

    size_t read = fread(buffer, 1, sizeof(buffer) - 1, file);

    /* A config that overruns the buffer would otherwise lose its last keys in
     * silence, and those keys would read as "already at the default". Say so
     * instead: on a console nobody can attach a debugger to, a silent partial
     * load is the worst possible failure. */
    bool truncated = (read == sizeof(buffer) - 1) && (fgetc(file) != EOF);

    fclose(file);
    buffer[read] = '\0';

    MovementCfg_ParseBuffer(buffer, read, cs, res);
    res->truncated = truncated;

    strncpy(s_lastPath, path, sizeof(s_lastPath) - 1);
    s_lastPath[sizeof(s_lastPath) - 1] = '\0';
    return true;
}

bool MovementCfg_Load(PM_CSTunables *cs, MovementCfgResult *res)
{
    /* Relative first: a launched development build's working directory is
     * sd:/dsidev/, which is exactly where `make asset-dsi` drops the file. */
    static const char *const kPaths[] = {
        "movement.cfg",
        "fat:/movement.cfg",
        "sd:/dsidev/movement.cfg",
        "sd:/movement.cfg",
    };

    for (int i = 0; i < (int)(sizeof(kPaths) / sizeof(kPaths[0])); i++)
    {
        if (MovementCfg_LoadPath(kPaths[i], cs, res))
            return true;
    }

    res->applied = 0;
    res->unknown = 0;
    res->malformed = 0;
    res->truncated = false;
    return false;
}

const char *MovementCfg_LastPath(void)
{
    return s_lastPath;
}
