#pragma once
#include <cstdint>

extern volatile bool  g_trackerRunning;
extern int64_t        g_totalLost;
extern int32_t        g_pendingLoss;
extern int32_t        g_deathCount;
extern bool           g_inGame;

void RunTracker();
