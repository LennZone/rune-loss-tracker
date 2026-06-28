#include "tracker.h"
#include "config.h"
#include "memory.h"
#include "version.h"
#include <windows.h>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

// eldenring.exe+0x3D5DF38 → deref → GameDataMan
// GameDataMan+0x8         → deref → PlayerGameData
// PlayerGameData+0x6C     = SoulsCount  (used only for readiness check)
// GameDataMan+0x94        = DeathNum (direct)
// GameDataMan+0x34        → deref → BloodstainData
// BloodstainData+0x48     = BloodstainSouls  (>0 = active, -1 = just picked up, 0 = none)
// eldenring.exe+0x3D69918 → deref → GameMan
// GameMan+0xAC0           = SaveSlotIndex (uint32, 0-based)
static constexpr uintptr_t OFFSET_BASE    = 0x3D5DF38;
static constexpr uintptr_t OFFSET_PLAYER  = 0x8;
static constexpr uintptr_t OFFSET_SOULS   = 0x6C;
static constexpr uintptr_t OFFSET_DEATHS  = 0x94;
static constexpr uintptr_t OFFSET_BLOOD   = 0x48;  // GameDataMan+0x48 →deref→ BloodstainData
static constexpr uintptr_t OFFSET_BLOOD2  = 0x34;  // BloodstainData+0x34 = BloodstainSouls
static constexpr uintptr_t OFFSET_GAMEMAN = 0x3D69918;
static constexpr uintptr_t OFFSET_SLOT    = 0xAC0;

static constexpr DWORD POLL_MS = 100;

static std::string GetModDir() {
    HMODULE hm = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)GetModDir, &hm);
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(hm, buf, MAX_PATH);
    char* slash = strrchr(buf, '\\');
    if (slash) *(slash + 1) = '\0';
    return std::string(buf);
}

static std::string LOG_FILE;
static std::string SAVE_DIR;
static std::string SAVE_FILE;
static std::string g_currentCharKey;

volatile bool g_trackerRunning = true;
int64_t       g_totalLost      = 0;
int32_t       g_pendingLoss    = 0;
int32_t       g_deathCount     = 0;
bool          g_inGame         = false;

static void Log(const std::string& msg) {
    if (!g_config.log) return;
    std::ofstream f(LOG_FILE, std::ios::app);
    f << msg << "\n";
}

static void LoadSaveData(int64_t& outTotal, int32_t& outDeaths) {
    outTotal  = 0;
    outDeaths = 0;
    if (SAVE_FILE.empty()) return;
    std::ifstream f(SAVE_FILE);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        auto sep = line.find(": ");
        if (sep == std::string::npos) continue;
        std::string key = line.substr(0, sep);
        std::string val = line.substr(sep + 2);
        if      (key == "total_runes_lost") outTotal  = std::stoll(val);
        else if (key == "deaths")           outDeaths = std::stoi(val);
    }
}

static void SaveData(int64_t total, int32_t deaths) {
    if (SAVE_FILE.empty()) return;
    std::ofstream f(SAVE_FILE);
    f << "total_runes_lost: " << total  << "\n"
      << "deaths: "           << deaths;
}

// Only used for the startup readiness check — waits until game memory is accessible.
static int32_t ReadSouls() {
    uintptr_t base = GetModuleBase("eldenring.exe");
    if (!base) return -1;
    uintptr_t addr = ResolvePointerChain(base, {OFFSET_BASE, OFFSET_PLAYER, OFFSET_SOULS});
    if (!addr) return -1;
    return ReadInt32(addr);
}

static int32_t ReadDeathCount() {
    uintptr_t base = GetModuleBase("eldenring.exe");
    if (!base) return -1;
    uintptr_t addr = ResolvePointerChain(base, {OFFSET_BASE, OFFSET_DEATHS});
    if (!addr) return -1;
    return ReadInt32(addr);
}

// Returns raw BloodstainSouls value:
//   > 0  — bloodstain exists in world, value = rune count
//   -1   — bloodstain was just picked up this tick
//    0   — no bloodstain (fresh start, or died again without picking up)
static int32_t ReadBloodstainSouls() {
    uintptr_t base = GetModuleBase("eldenring.exe");
    if (!base) return 0;
    uintptr_t addr = ResolvePointerChain(base, {OFFSET_BASE, OFFSET_BLOOD, OFFSET_BLOOD2});
    if (!addr) return 0;
    return ReadInt32(addr);
}

static int32_t ReadSlotIndex() {
    uintptr_t base = GetModuleBase("eldenring.exe");
    if (!base) return -1;
    uintptr_t addr = ResolvePointerChain(base, {OFFSET_GAMEMAN, OFFSET_SLOT});
    if (!addr) return -1;
    int32_t slot = ReadInt32(addr);
    return (slot >= 0 && slot < 10) ? slot : -1;
}

static void LoadCharacterData(int32_t deaths, int32_t bloodstain) {
    std::string key;
    int32_t slot = ReadSlotIndex();
    if (slot >= 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "slot%d", slot);
        key = std::string(buf);
    }
    if (key.empty()) {
        char buf[32];
        snprintf(buf, sizeof(buf), "d%d", deaths);
        key = std::string(buf);
    }
    g_currentCharKey = key;
    SAVE_FILE        = SAVE_DIR + "RuneLossTracker_" + key + "_save.txt";

    int64_t storedTotal  = 0;
    int32_t storedDeaths = 0;
    LoadSaveData(storedTotal, storedDeaths);

    if (storedDeaths > 0 && deaths < storedDeaths) {
        g_totalLost   = 0;
        g_pendingLoss = 0;
        SaveData(0, deaths);
        Log("New character detected in slot. Save file cleared.");
    } else {
        g_totalLost   = storedTotal;
        g_pendingLoss = (bloodstain > 0) ? bloodstain : 0;
    }

    std::ostringstream oss;
    oss << "Character loaded: " << key
        << " | totalLost=" << g_totalLost
        << " | storedDeaths=" << storedDeaths
        << " | currentDeaths=" << deaths
        << " | pendingLoss=" << g_pendingLoss
        << " | bloodstainSouls=" << bloodstain;
    Log(oss.str());
}

void RunTracker() {
    std::string dir = GetModDir();
    LOG_FILE = dir + "RuneLossTracker.log";
    SAVE_DIR = dir;
    LoadConfig(dir);

    Log("RuneLossTracker v" VERSION_STR " started.");

    // Wait until game memory is accessible.
    int32_t souls = -1;
    int waitSecs = 0;
    while (g_trackerRunning && souls < 0) {
        uintptr_t base = GetModuleBase("eldenring.exe");
        uintptr_t pgd  = base ? ResolvePointerChain(base, {OFFSET_BASE, OFFSET_PLAYER, OFFSET_SOULS}) : 0;
        souls = ReadSouls();
        if (waitSecs % 5 == 0) {
            std::ostringstream oss;
            oss << "Waiting... base=0x" << std::hex << base
                << " soulsAddr=0x" << pgd
                << " souls=" << std::dec << souls;
            Log(oss.str());
        }
        waitSecs++;
        Sleep(1000);
    }

    int32_t prevDeaths    = ReadDeathCount();
    int32_t prevBloodstain = ReadBloodstainSouls();

    if (prevDeaths > 0) {
        // Character already in world at tracker startup — init directly.
        // prevDeaths == 0 means main menu; let the death-counter jump handle init there.
        LoadCharacterData(prevDeaths, prevBloodstain);
        g_inGame = true;
        Log("Tracker initialized mid-session.");
    }

    while (g_trackerRunning) {
        int32_t deaths    = ReadDeathCount();
        int32_t bloodstain = ReadBloodstainSouls();

        if (deaths < 0) {
            g_inGame = false;
            Sleep(1000);
            continue;
        }

        if (deaths < prevDeaths) {
            // Death counter dropped — back in main menu or character switch.
            SaveData(g_totalLost, g_deathCount);
            std::ostringstream oss;
            oss << "Back in menu (deaths " << prevDeaths << " -> " << deaths << ").";
            Log(oss.str());
            g_inGame         = false;
            g_currentCharKey = "";
            SAVE_FILE        = "";
            g_pendingLoss    = 0;

        } else if (deaths > prevDeaths) {

            if (deaths - prevDeaths == 1) {
                // Single death.
                if (g_currentCharKey.empty()) LoadCharacterData(deaths, bloodstain);
                g_inGame = true;

                if (g_pendingLoss > 0) {
                    // Had a bloodstain that was never picked up — runes are permanently lost.
                    g_totalLost += g_pendingLoss;
                    std::ostringstream oss;
                    oss << "LOST " << g_pendingLoss << " runes (bloodstain missed). Total: " << g_totalLost;
                    Log(oss.str());
                }

                // New pending: read from memory. Bloodstain may update same tick or next tick;
                // the else-branch below will sync it on the following poll if still 0 here.
                g_pendingLoss = (bloodstain > 0) ? bloodstain : 0;
                SaveData(g_totalLost, deaths);
                std::ostringstream oss;
                oss << "Died. pendingLoss=" << g_pendingLoss
                    << " bloodstainSouls=" << bloodstain;
                Log(oss.str());

            } else {
                // Large jump — character entered world (load/fast travel reinit).
                LoadCharacterData(deaths, bloodstain);
                g_inGame = true;
                std::ostringstream oss;
                oss << "Death counter reinitialized: " << prevDeaths << " -> " << deaths;
                Log(oss.str());
            }

        } else {
            // No death event this tick.

            if (bloodstain == -1 && prevBloodstain > 0) {
                // Game set BloodstainSouls to -1 — bloodstain was just picked up.
                std::ostringstream oss;
                oss << "Bloodstain recovered (" << g_pendingLoss << " runes). Pending cleared.";
                Log(oss.str());
                g_pendingLoss = 0;
                SaveData(g_totalLost, g_deathCount);

            } else if (bloodstain > 0) {
                // Sync pendingLoss from live memory. Handles the case where the bloodstain
                // value was not yet updated in the same tick as the death event.
                g_pendingLoss = bloodstain;
            }
        }

        prevDeaths     = deaths;
        prevBloodstain = bloodstain;
        g_deathCount   = deaths;
        Sleep(POLL_MS);
    }

    SaveData(g_totalLost, g_deathCount);
    Log("RuneLossTracker stopped.");
}
