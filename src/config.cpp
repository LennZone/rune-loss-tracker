#include "config.h"
#include <fstream>
#include <string>

Config g_config;

static bool ParseBool(const std::string& val) {
    return val == "true" || val == "1" || val == "yes";
}

void LoadConfig(const std::string& dir) {
    std::string path = dir + "RuneLossTracker.ini";

    // Create file if it doesn't exist yet.
    {
        std::ifstream check(path);
        if (!check.is_open()) {
            std::ofstream f(path);
            f << "; RuneLossTracker configuration\n"
                 "; Changes take effect on next game launch.\n"
                 "\n"
                 "; Show the death counter panel above the lost-runes panel.\n"
                 "show_death_overlay=true\n"
                 "\n"
                 "; Write a log file for debugging (RuneLossTracker.log).\n"
                 "log=false\n";
        }
    }

    bool foundShowDeath = false;
    bool foundLog       = false;

    std::ifstream f(path);
    if (!f.is_open()) return;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        auto sep = line.find('=');
        if (sep == std::string::npos) continue;
        std::string key = line.substr(0, sep);
        std::string val = line.substr(sep + 1);
        if (key == "show_death_overlay") { g_config.showDeathOverlay = ParseBool(val); foundShowDeath = true; }
        else if (key == "log")           { g_config.log              = ParseBool(val); foundLog       = true; }
    }
    f.close();

    // Append any keys that were missing from the file.
    if (!foundShowDeath || !foundLog) {
        std::ofstream a(path, std::ios::app);
        if (!foundShowDeath) a << "\n; Show the death counter panel above the lost-runes panel.\nshow_death_overlay=true\n";
        if (!foundLog)       a << "\n; Write a log file for debugging (RuneLossTracker.log).\nlog=false\n";
    }
}
