#include "config.h"
#include <CLibUtilsQTR/FormReader.hpp>
#include <CLibUtilsQTR/StringHelpers.hpp>
#include <ClibUtil/string.hpp>
#include <fstream>
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
        std::string ReadIniFile(const std::string& iniFilePath) {
            std::ifstream file(iniFilePath, std::ios::binary);
            if (!file) {
                logger::error("Failed to read INI file '{}'", iniFilePath);
                return {};
            }
            std::string text(std::istreambuf_iterator<char>{file}, {});
            constexpr std::string_view utf16LEBOM = "\xFF\xFE";
            constexpr std::string_view utf8BOM = "\xEF\xBB\xBF";
            if (text.starts_with(utf16LEBOM)) {
                const auto byteCount = text.size() - utf16LEBOM.size();
                const auto characterCount = byteCount / sizeof(wchar_t);
                if (byteCount % sizeof(wchar_t) != 0 || characterCount > std::numeric_limits<int>::max()) {
                    logger::error("Invalid UTF-16 INI file '{}'", iniFilePath);
                    return {};
                }
                std::wstring wide(characterCount, L'\0');
                std::memcpy(wide.data(), text.data() + utf16LEBOM.size(), byteCount);
                const auto length = static_cast<int>(wide.size());
                const auto size = WideCharToMultiByte(CP_ACP, 0, wide.data(), length, nullptr, 0, nullptr, nullptr);
                if (size == 0) return {};
                text.assign(size, '\0');
                WideCharToMultiByte(CP_ACP, 0, wide.data(), length, text.data(), size, nullptr, nullptr);
            } else if (text.starts_with(utf8BOM)) {
                text.erase(0, utf8BOM.size());
            }
            std::ranges::replace(text, '\r', '\n');
            return text;
        }

        template <typename T>
        std::vector<T*> ReadFormsFromINISection(std::istream& file, const char* section) {
            file.clear();
            file.seekg(0);
            std::vector<T*> forms;
            bool inSection = false;
            for (std::string line; std::getline(file, line);) {
                const auto entry = StringHelpers::trim(line.substr(0, line.find(';')));
                if (entry.starts_with('[')) {
                    if (const auto end = entry.find(']'); end != std::string::npos) {
                        if (inSection) break;
                        inSection = clib_util::string::iequals(StringHelpers::trim(entry.substr(1, end - 1)), section);
                        continue;
                    }
                }
                if (inSection) {
                    if (const auto form = FormReader::GetFormFromString(entry)) {
                        if (const auto object = form->As<T>()) {
                            forms.push_back(object);
                        }
                    }
                }
            }
            return forms;
        }
    }

    void ReadPotionFormLists() {
        std::istringstream file(ReadIniFile(GetIniFilePath()));
        potionFormLists.blacklisted = ReadFormsFromINISection<RE::TESBoundObject>(file, "BlacklistedPotions");
        potionFormLists.allowed = ReadFormsFromINISection<RE::AlchemyItem>(file, "AllowedPotions");
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
