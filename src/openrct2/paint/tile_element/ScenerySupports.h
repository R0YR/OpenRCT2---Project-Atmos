/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include "../Paint.h"

// Full-tile scenery blockers reserve every support segment. Wooden scenery
// supports otherwise consult only the general height and ignore that reservation.
inline bool ScenerySupportsAreBlocked(const PaintSession& session)
{
    for (const auto& segment : session.SupportSegments)
    {
        if (segment.height != 0xFFFF)
            return false;
    }
    return true;
}
