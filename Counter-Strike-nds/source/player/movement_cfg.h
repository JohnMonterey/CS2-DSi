// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// movement.cfg: CS:GO-unit tuning values, reloadable without a rebuild.

#ifndef MOVEMENT_CFG_H_ /* Include guard */
#define MOVEMENT_CFG_H_

#include "playermove_core.h"

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Outcome of a parse, so a typo in the config says so rather than
 *        silently leaving a constant at its default.
 */
typedef struct
{
    int applied;    /* keys recognised and applied                        */
    int unknown;    /* keys that matched nothing                          */
    int malformed;  /* lines that were not key = value                    */
    bool truncated; /* file was larger than the read buffer; keys lost    */
} MovementCfgResult;

/**
 * @brief Apply key=value lines from @p text onto @p cs.
 *
 * Pure: no file access, no globals. Unrecognised keys and malformed lines are
 * counted and skipped, never fatal -- a bad config must not brick the build on
 * a console the developer cannot see.
 */
void MovementCfg_ParseBuffer(const char *text, size_t len, PM_CSTunables *cs,
                             MovementCfgResult *res);

/**
 * @brief Read one file and apply it. Returns false if the file is unreadable.
 */
bool MovementCfg_LoadPath(const char *path, PM_CSTunables *cs,
                          MovementCfgResult *res);

/**
 * @brief Probe the usual locations and apply the first one that exists.
 *
 * A launched development build's working directory is sd:/dsidev/, so the
 * relative path finds what `make asset-dsi` pushes; the absolute paths cover a
 * plain card boot. Returns false when no config was found, which is normal --
 * the compiled-in defaults are already CS:GO's shipping values.
 */
bool MovementCfg_Load(PM_CSTunables *cs, MovementCfgResult *res);

/** @brief Path the last successful load came from, or "" if none. */
const char *MovementCfg_LastPath(void);

#endif // MOVEMENT_CFG_H_
