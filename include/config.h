#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace config {
    extern int iMovementType;
    extern bool bEnableNotifications;
    extern bool bEnableSpamPotions;
    extern bool bExcludePlayer;
    struct PotionFormLists {
        std::vector<RE::TESBoundObject*> blacklisted;
        std::vector<RE::AlchemyItem*> allowed;
    };
    extern PotionFormLists potionFormLists;

    std::unordered_map<std::string, int> ReadSettings();
    void ReadPotionFormLists();
    std::string GetIniFilePath();
    int GetIniSetting(const char* settingName, int defaultValue);
    void SetIniSetting(const char* settingName, int value); 
}

inline std::atomic<bool> pause = false;
