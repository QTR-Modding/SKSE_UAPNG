#include "config.h"
#include <CLibUtilsQTR/FormReader.hpp>
#include <CLibUtilsQTR/StringHelpers.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include <Windows.h>

namespace config {
    const char* INI_FILE_NAME = "UAPNG.ini";
    int iMovementType;
    bool bEnableNotifications;
    bool bEnableSpamPotions;
    bool bExcludePlayer;
    PotionFormLists potionFormLists;

    std::string GetIniFilePath() {
        char dllPath[MAX_PATH];
        GetModuleFileNameA(GetModuleHandleA("UAPNG.dll"), dllPath, sizeof(dllPath));
        std::string iniFilePath(dllPath);
        size_t lastSlashIndex = iniFilePath.find_last_of("\\/");
        iniFilePath = iniFilePath.substr(0, lastSlashIndex + 1) + INI_FILE_NAME;
        return iniFilePath;
    }

    int GetIniSetting(const char* settingName, int defaultValue) {
        std::string iniFilePath = GetIniFilePath();
        return GetPrivateProfileIntA("Settings", settingName, defaultValue, iniFilePath.c_str());
    }

    void SetIniSetting(const char* settingName, int value) {
        std::string iniFilePath = GetIniFilePath();
        WritePrivateProfileStringA("Settings", settingName, std::to_string(value).c_str(), iniFilePath.c_str());
    }

    namespace {
        template <typename T>
        std::vector<T*> ReadFormsFromINISection(const char* section) {
            const std::string iniFilePath = GetIniFilePath();
            constexpr std::size_t initialBufferSize = 4096;
            constexpr std::size_t sectionTerminatorCount = 2;
            constexpr auto maximumBufferSize = static_cast<std::size_t>(std::numeric_limits<DWORD>::max());
            std::vector<char> buffer(initialBufferSize);
            while (GetPrivateProfileSectionA(section, buffer.data(), static_cast<DWORD>(buffer.size()), iniFilePath.c_str()) == buffer.size() - sectionTerminatorCount) {
                if (buffer.size() == maximumBufferSize) {
                    logger::error("INI section '{}' exceeds the Windows section size limit", section);
                    return {};
                }
                buffer.resize(std::min(buffer.size() * 2, maximumBufferSize));
            }

            std::vector<T*> forms;
            const char* ptr = buffer.data();
            while (*ptr != '\0') {
                const std::string line(ptr);
                const auto entry = StringHelpers::trim(line.substr(0, line.find(';')));
                if (const auto form = FormReader::GetFormFromString(entry)) {
                    if (const auto object = form->As<T>()) {
                        forms.push_back(object);
                    }
                }
                ptr += line.size() + 1;
            }
            return forms;
        }
    }

    void ReadPotionFormLists() {
        potionFormLists.blacklisted = ReadFormsFromINISection<RE::TESBoundObject>("BlacklistedPotions");
        potionFormLists.allowed = ReadFormsFromINISection<RE::AlchemyItem>("AllowedPotions");
    }

    std::unordered_map<std::string, int> ReadSettings() {
        std::unordered_map<std::string, int> settings;
        settings["iMovementType"] = GetIniSetting("iMovementType", 0);
        settings["bEnableNotifications"] = GetIniSetting("bEnableNotifications", 1);
        settings["bEnableSpamPotions"] = GetIniSetting("bEnableSpamPotions", 1);
        settings["bExcludePlayer"] = GetIniSetting("bExcludePlayer", 0);
        return settings;
    }
}
