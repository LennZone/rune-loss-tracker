#pragma once
#include <string>

struct Config {
    bool showDeathOverlay = true;
    bool log              = false;
};

extern Config g_config;

void LoadConfig(const std::string& dir);
