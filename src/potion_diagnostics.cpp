#include "potion_diagnostics.h"
#include "config.h"
#include "equip_object_override.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <bcrypt.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <thread>

namespace PotionDiagnostics {
    namespace {
        constexpr std::size_t ringSize = 8192;
        constexpr auto snapshotInterval = std::chrono::milliseconds(500);
        constexpr std::size_t maximumEffects = 256;
        constexpr std::size_t maximumPotions = 256, maximumPotionEffects = 32;
        constexpr RE::FormID playerRefID = 0x14;
        struct SwiftProfile {
            std::string_view version, hash;
            std::uintptr_t settings, guard, firstCategory, extraCategory;
            std::size_t categoryCount;
            std::uintptr_t delta, hook, elapsed, original, utility, update;
        };
        constexpr std::array swiftProfiles{
            SwiftProfile{"3.4.1", "ffedd70f7246a15cf6f930ed5367db0a4f3e8701fc9bd5973be0ff07a2d069da",
                0xB0530, 0xB0B80, 0x1F0, 0x448, 15, 0xB0C10, 0xB0C18, 0xB0C20, 0xB0C28, 0xB0C50, 0x370A0},
            SwiftProfile{"3.4", "1a34f9f34f67e54fd1152c5b4c6a52fb378a49b3e9c3e9934b35c3feed3a011d",
                0xAF530, 0xAFB80, 0x1F0, 0x448, 15, 0xAFC10, 0xAFC18, 0xAFC20, 0xAFC28, 0xAFC50, 0x36B10},
            SwiftProfile{"3.3", "df480082193172b17fae6c9e7decdb83e52ef60113af736b777aeab9cd890c55",
                0xAC540, 0xACB90, 0x1F0, 0x448, 15, 0xACC40, 0xACC48, 0xACC50, 0xACC58, 0xACC60, 0x461D0},
            SwiftProfile{"3.2", "3bf0e611df8a141fa89a02ad5cbfd9aaba5f8b4a160dd3e4038e021d47009686",
                0x961B0, 0x96800, 0x1F0, 0x448, 15, 0x96890, 0x96898, 0x968A0, 0x968A8, 0x968B0, 0x333F0},
            SwiftProfile{"3.1", "5df88e94d9266d78864185e074edb421d58430e9ebf2a1a49e0441162194795d",
                0x94130, 0x94600, 0x170, 0x3C8, 11, 0x94690, 0x94698, 0x946A0, 0x946A8, 0x946B0, 0x31CB0},
            SwiftProfile{"3.0.1", "593e434acc0a59d9bb841ed4420ecdf6617a5026ece187a12b734eb9887e3b68",
                0x9FC10, 0x9FC00, 0x170, 0x3C8, 11, 0x9FB40, 0x9FB48, 0x9FBD8, 0x9FAA8, 0xA0100, 0x2BFE0},
            SwiftProfile{"3.0", "62a96c526bbd7118e34e9f2b5ab81f76fd3b745dc6bdb028acbb240b029eb88a",
                0x9DC10, 0x9DC00, 0x170, 0x3C8, 11, 0x9DB40, 0x9DB48, 0x9DBD8, 0x9DAA8, 0x9E100, 0x29840}
        };
        constexpr std::string_view runtimeHash = "c434208894f07f604b852f29b8edc3a58c4de63de783373733e72b2b73f33be9";
        constexpr std::string_view unpackedRuntimeHash = "4874acda2603b6da9063dd43cdcd5ca514dc2b586ea1e15c9f20fc8ad501712a";
        thread_local std::uint64_t currentTrace{};

        struct Record {
            std::uint64_t tick{}, id{}, parent{};
            std::uint32_t thread{};
            RE::FormID actor{}, object{};
            std::array<std::uint64_t, 12> values{};
            std::array<void*, 12> frames{};
            std::uint16_t frameCount{};
            std::array<char, 72> label{};
            std::array<char, 128> text{};
        };

        Record MakeRecord(const char* label) {
            Record record;
            record.tick = GetTickCount64();
            record.thread = GetCurrentThreadId();
            record.id = currentTrace;
            strncpy_s(record.label.data(), record.label.size(), label, _TRUNCATE);
            return record;
        }

        std::string FileHash(const std::filesystem::path& path) {
            std::ifstream input(path, std::ios::binary);
            if (!input) return "unreadable";
            BCRYPT_ALG_HANDLE algorithm{};
            if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return "hash-provider-error";
            BCRYPT_HASH_HANDLE hash{};
            if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
                BCryptCloseAlgorithmProvider(algorithm, 0);
                return "hash-create-error";
            }
            std::array<char, 65536> buffer;
            bool success = true;
            while (input) {
                input.read(buffer.data(), buffer.size());
                if (const auto count = input.gcount(); count > 0 && BCryptHashData(hash,
                    reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) < 0) success = false;
            }
            std::array<unsigned char, 32> digest{};
            success = !input.bad() && success && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            if (!success) return "hash-read-error";
            std::string result;
            for (const auto byte : digest) result += std::format("{:02x}", byte);
            return result;
        }

        std::string FileVersion(const std::filesystem::path& path) {
            DWORD ignored{};
            const auto size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
            if (!size) return "unavailable";
            std::vector<std::byte> buffer(size);
            if (!GetFileVersionInfoW(path.c_str(), 0, size, buffer.data())) return "unavailable";
            VS_FIXEDFILEINFO* info{};
            UINT length{};
            if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &length) || length < sizeof(*info)) return "unavailable";
            return std::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                               HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
        }

        void LogConfiguration(spdlog::logger& log, const std::filesystem::path& path) {
            constexpr std::size_t maximumConfigurationBytes = 64 * 1024;
            std::ifstream file(path, std::ios::binary);
            if (!file) { log.info("Config path={} unreadable-or-missing", path.string()); return; }
            std::array<char, maximumConfigurationBytes> bytes;
            file.read(bytes.data(), bytes.size());
            const auto size = static_cast<std::size_t>(file.gcount());
            std::string text(bytes.data(), size);
            log.info("Config path={} capturedBytes={} limit={} possiblyTruncated={}", path.string(), size, maximumConfigurationBytes,
                     size == maximumConfigurationBytes);
            std::size_t lineNumber{};
            std::istringstream lines(text);
            for (std::string line; std::getline(lines, line);) {
                ++lineNumber;
                std::string escaped;
                for (const auto ch : line) {
                    const auto value = static_cast<unsigned char>(ch);
                    if (value < 32 && value != '\t') escaped += std::format("\\x{:02X}", value);
                    else escaped += ch;
                }
                log.info("Config line={} text={}", lineNumber, escaped);
            }
        }

        bool ReadMemory(std::uintptr_t address, void* output, std::size_t size) {
            SIZE_T copied{};
            return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), output,
                                               size, &copied) && copied == size;
        }

        template <class T> bool ReadMemory(std::uintptr_t address, T& output) {
            return ReadMemory(address, &output, sizeof(output));
        }

        Record CaptureBranchTarget(const char* label, std::uintptr_t site, std::uintptr_t expected) {
            constexpr std::uint8_t relativeCall = 0xE8, relativeJump = 0xE9, indirectBranch = 0xFF;
            constexpr std::uint8_t indirectCall = 0x15, indirectJump = 0x25;
            constexpr std::size_t relativeSize = 5, indirectSize = 6;
            enum : std::uint64_t { SiteRead = 1, RelativeDecoded = 2, IndirectRead = 4,
                                  TargetRead = 8, RelayFound = 16, RelayRead = 32 };
            auto record = MakeRecord(label);
            std::array<std::uint8_t, indirectSize> bytes{};
            std::uint64_t flags{}, raw{}, relayRaw{};
            std::uintptr_t target{}, resolved{}, slot{}, relaySlot{};
            if (ReadMemory(site, bytes)) {
                flags |= SiteRead;
                std::memcpy(&raw, bytes.data(), bytes.size());
                std::int32_t displacement{};
                if (bytes[0] == relativeCall || bytes[0] == relativeJump) {
                    std::memcpy(&displacement, bytes.data() + 1, sizeof(displacement));
                    target = site + relativeSize + displacement;
                    flags |= RelativeDecoded;
                } else if (bytes[0] == indirectBranch && (bytes[1] == indirectCall || bytes[1] == indirectJump)) {
                    std::memcpy(&displacement, bytes.data() + 2, sizeof(displacement));
                    slot = site + indirectSize + displacement;
                    if (ReadMemory(slot, target)) flags |= IndirectRead;
                    else target = 0;
                }
                resolved = target;
                if (target && ReadMemory(target, bytes)) {
                    flags |= TargetRead;
                    std::memcpy(&relayRaw, bytes.data(), bytes.size());
                    if (bytes[0] == indirectBranch && bytes[1] == indirectJump) {
                        flags |= RelayFound;
                        std::memcpy(&displacement, bytes.data() + 2, sizeof(displacement));
                        relaySlot = target + indirectSize + displacement;
                        if (ReadMemory(relaySlot, resolved)) flags |= RelayRead;
                        else resolved = 0;
                    }
                }
            }
            record.values = {site, expected, target, resolved, raw, flags, slot, relaySlot, relayRaw};
            return record;
        }

        struct Recorder : RE::BSTEventSink<RE::TESDeathEvent>, RE::BSTEventSink<RE::TESContainerChangedEvent>,
                          RE::BSTEventSink<RE::MenuOpenCloseEvent> {
            std::mutex mutex;
            std::condition_variable wake;
            std::array<Record, ringSize> ring;
            std::size_t begin{}, count{};
            std::atomic<std::uint64_t> nextID{1}, drops{}, totalDrops{};
            std::atomic<bool> ready{}, pending{}, modulesRequested{}, initialized{};
            std::atomic<std::uintptr_t> swiftBase{};
            std::atomic<const SwiftProfile*> swiftProfile{};
            bool verifiedRuntime{};
            std::filesystem::path executable;
            std::string executableHash;
            std::uint64_t inventoryHash{};
            bool inventoryRecorded{};
            std::shared_ptr<spdlog::logger> log;
            std::jthread worker;

            ~Recorder() {
                initialized.store(false);
                ready.store(false);
                worker.request_stop();
                wake.notify_all();
                if (worker.joinable()) worker.join();
            }

            void Push(const Record& record) {
                if (!initialized.load()) return;
                try {
                    const std::lock_guard lock(mutex);
                    if (count == ringSize) { drops.fetch_add(1); return; }
                    ring[(begin + count) % ringSize] = record;
                    ++count;
                } catch (...) { drops.fetch_add(1); }
            }

            void Write(const Record& record) {
                log->trace("tick={} tid={} id={} parent={} actor={:08X} object={:08X} {} text='{}' values=[{:X},{:X},{:X},{:X},{:X},{:X},{:X},{:X},{:X},{:X},{:X},{:X}]",
                          record.tick, record.thread, record.id, record.parent, record.actor, record.object, record.label.data(),
                          record.text.data(),
                          record.values[0], record.values[1], record.values[2], record.values[3], record.values[4], record.values[5],
                          record.values[6], record.values[7], record.values[8], record.values[9], record.values[10], record.values[11]);
                if (std::string_view(record.label.data()) == "PlayerSnapshot") {
                    log->trace("  player dead={} combat={} paused={} health={}/{} magicka={}/{} stamina={}/{}",
                              record.values[0], record.values[1], record.values[2], std::bit_cast<float>(static_cast<std::uint32_t>(record.values[3])),
                              std::bit_cast<float>(static_cast<std::uint32_t>(record.values[4])), std::bit_cast<float>(static_cast<std::uint32_t>(record.values[5])),
                              std::bit_cast<float>(static_cast<std::uint32_t>(record.values[6])), std::bit_cast<float>(static_cast<std::uint32_t>(record.values[7])),
                              std::bit_cast<float>(static_cast<std::uint32_t>(record.values[8])));
                }
                for (std::uint16_t index = 0; index < record.frameCount; ++index) {
                    HMODULE module{};
                    std::array<wchar_t, MAX_PATH> path{};
                    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCWSTR>(record.frames[index]), &module)) GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
                    log->trace("  caller[{}]={}+0x{:X} address=0x{:X}", index, std::filesystem::path(path.data()).filename().string(),
                              reinterpret_cast<std::uintptr_t>(record.frames[index]) - reinterpret_cast<std::uintptr_t>(module),
                              reinterpret_cast<std::uintptr_t>(record.frames[index]));
                }
                if (std::string_view(record.label.data()) == "SKSEVersion") {
                    log->info("SKSE version={} runtime={}", REL::Version::unpack(static_cast<std::uint32_t>(record.values[0])).string("."),
                        REL::Version::unpack(static_cast<std::uint32_t>(record.values[1])).string("."));
                }
            }

            void Modules() {
                log->info("Executable path={} version={} sha256={}", executable.string(), FileVersion(executable), executableHash);
                const std::array<std::wstring_view, 7> relevant{L"uapng.dll", L"swiftpotion-ng.dll", L"npcsusepotions.dll", L"po3_tweaks.dll",
                    L"privateprofileredirector.dll", L"animationqueuefix.dll", L"enginefixes.dll"};
                std::array<bool, relevant.size()> found{};
                const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
                if (snapshot == INVALID_HANDLE_VALUE) { log->error("Module inventory failed error={}", GetLastError()); return; }
                MODULEENTRY32W module{sizeof(MODULEENTRY32W)};
                for (auto more = Module32FirstW(snapshot, &module); more; more = Module32NextW(snapshot, &module)) {
                    const std::filesystem::path path(module.szExePath);
                    log->info("Module name={} base=0x{:X} size=0x{:X} version={} path={}", path.filename().string(),
                              reinterpret_cast<std::uintptr_t>(module.modBaseAddr), module.modBaseSize, FileVersion(path), path.string());
                    for (std::size_t index = 0; index < relevant.size(); ++index) {
                        if (_wcsicmp(module.szModule, relevant[index].data()) != 0) continue;
                        found[index] = true;
                        const auto hash = FileHash(path);
                        log->info("Relevant module={} sha256={}", path.filename().string(), hash);
                        if (relevant[index] == L"uapng.dll") {
                            LogConfiguration(*log, L"Data/SKSE/Plugins/SwiftPotionNG.ini");
                            LogConfiguration(*log, L"Data/SKSE/Plugins/SwiftPotionNG_Blacklist.ini");
                        }
                        if (relevant[index] == L"swiftpotion-ng.dll") {
                            const auto profile = std::ranges::find(swiftProfiles, hash, &SwiftProfile::hash);
                            if (profile != swiftProfiles.end()) {
                                swiftProfile.store(&*profile);
                                swiftBase.store(reinterpret_cast<std::uintptr_t>(module.modBaseAddr));
                                log->info("Swift {} passive read profile enabled: exact hash match", profile->version);
                            } else log->info("Swift passive read profile unavailable: no audited exact hash match");
                        }
                    }
                }
                CloseHandle(snapshot);
                for (std::size_t index = 0; index < relevant.size(); ++index) {
                    if (found[index]) continue;
                    const auto path = executable.parent_path() / L"Data" / L"SKSE" / L"Plugins" / relevant[index];
                    log->info("Relevant module={} loaded=false installed={} path={}", path.filename().string(),
                              std::filesystem::exists(path), path.string());
                }
            }

            void SwiftSnapshot() {
                const auto module = swiftBase.load();
                if (!module) return;
                const auto& profile = *swiftProfile.load();
                std::int32_t guard{};
                if (!ReadMemory(module + profile.guard, guard) || guard >= 0 || guard == -1) {
                    Note("SwiftSettingsUnavailable", static_cast<std::uint32_t>(guard)); return;
                }
                auto scheduler = MakeRecord("SwiftScheduler");
                std::uint8_t enabled{};
                float elapsed{}, delta{};
                std::uintptr_t deltaAddress{}, hook{}, original{};
                std::uint64_t reads{};
                reads |= ReadMemory(module + profile.settings, enabled) ? 1 : 0;
                reads |= ReadMemory(module + profile.elapsed, elapsed) ? 2 : 0;
                reads |= ReadMemory(module + profile.delta, deltaAddress) ? 4 : 0;
                reads |= ReadMemory(deltaAddress, delta) ? 8 : 0;
                reads |= ReadMemory(module + profile.hook, hook) ? 16 : 0;
                reads |= ReadMemory(module + profile.original, original) ? 32 : 0;
                scheduler.values = {enabled, std::bit_cast<std::uint32_t>(elapsed), std::bit_cast<std::uint32_t>(delta), deltaAddress, hook, original, reads};
                Push(scheduler);
                Push(CaptureBranchTarget("SwiftUpdateHook", hook, module + profile.update));
                constexpr std::uintptr_t brawlStageOffset = 0x228;
                std::array<std::uintptr_t, 9> utility{};
                if (ReadMemory(module + profile.utility, utility)) {
                    auto gates = MakeRecord("SwiftEligibilityPointers");
                    std::uintptr_t player{}, ui{}, controls{};
                    std::uint16_t stage{};
                    std::uint64_t gateReads{};
                    gateReads |= ReadMemory(utility[6], player) ? 1 : 0;
                    gateReads |= ReadMemory(utility[7], ui) ? 2 : 0;
                    gateReads |= ReadMemory(utility[8], controls) ? 4 : 0;
                    gateReads |= ReadMemory(utility[3] + brawlStageOffset, stage) ? 8 : 0;
                    gates.values = {utility[0], utility[1], utility[3], stage, utility[6], player, utility[7], ui, utility[8], controls, gateReads};
                    Push(gates);
                }
                constexpr std::array names{"HealthRestore", "HealthRegen", "HealthFortify", "MagickaRestore", "MagickaRegen", "MagickaFortify",
                    "StaminaRestore", "StaminaRegen", "StaminaFortify", "CureDisease", "CurePoison", "ResistFire", "ResistShock", "ResistFrost", "ResistMagic"};
                constexpr std::size_t primaryCategoryCount = 9, categorySize = 0x40;
                constexpr std::size_t stringInlineCapacity = 15, maximumEffectNameLength = 4096;
                for (std::size_t index = 0; index < profile.categoryCount; ++index) {
                    const auto address = module + profile.settings + (index < primaryCategoryCount ?
                        profile.firstCategory + index * categorySize : profile.extraCategory + (index - primaryCategoryCount) * categorySize);
                    std::array<std::byte, categorySize> bytes;
                    if (!ReadMemory(address, bytes)) { Note("SwiftCategoryReadFailed", index); continue; }
                    const auto read = [&]<typename T>(std::size_t offset) { T value; std::memcpy(&value, bytes.data() + offset, sizeof(value)); return value; };
                    auto record = MakeRecord(names[index]);
                    record.values = {read.operator()<std::uint8_t>(0x28), read.operator()<std::uint8_t>(0x34), read.operator()<std::uint8_t>(0x29),
                        read.operator()<std::uint32_t>(0x2C), read.operator()<std::uint32_t>(0x30), read.operator()<std::uint8_t>(0x20),
                        read.operator()<std::uint8_t>(0x21), read.operator()<std::uint32_t>(0x24), read.operator()<std::uint32_t>(0x38)};
                    Push(record);
                    const auto length = read.operator()<std::uint64_t>(0x10), capacity = read.operator()<std::uint64_t>(0x18);
                    auto name = MakeRecord("SwiftEffectName");
                    name.values = {index, length, capacity};
                    const auto pointer = capacity <= stringInlineCapacity ? address : read.operator()<std::uintptr_t>(0);
                    std::array<char, 64> text{};
                    if (length <= capacity && length < maximumEffectNameLength && ReadMemory(pointer, text.data(), std::min<std::uint64_t>(length, text.size() - 1))) {
                        strncpy_s(name.text.data(), name.text.size(), text.data(), _TRUNCATE);
                    }
                    Push(name);
                }
            }

            void InventorySnapshot(RE::PlayerCharacter* player) {
                const auto initializedInventory = player->GetInventoryChanges(true) != nullptr;
                const auto inventory = player->GetInventory([](RE::TESBoundObject& object) {
                    return object.Is(RE::FormType::AlchemyItem);
                }, true);
                std::uint64_t hash = initializedInventory;
                const auto combine = [&](std::uint64_t value) { hash = std::rotl(hash, 7) ^ std::hash<std::uint64_t>{}(value); };
                for (const auto& [object, entry] : inventory) {
                    combine(object->formID);
                    combine(static_cast<std::uint64_t>(entry.first));
                    const auto potion = static_cast<RE::AlchemyItem*>(object);
                    combine(potion->data.flags.underlying());
                    for (const auto effect : potion->effects) {
                        if (!effect || !effect->baseEffect) continue;
                        combine(effect->baseEffect->formID);
                        combine(effect->baseEffect->data.flags.underlying());
                        combine(std::hash<std::string_view>{}(effect->baseEffect->GetFullName()));
                        combine(std::bit_cast<std::uint32_t>(effect->effectItem.magnitude));
                        combine(effect->effectItem.duration);
                    }
                }
                if (inventoryRecorded && inventoryHash == hash) return;
                inventoryRecorded = true;
                inventoryHash = hash;
                Note("PotionInventoryBegin", inventory.size(), initializedInventory);
                std::size_t recorded{};
                for (const auto& [object, entry] : inventory) {
                    if (recorded++ >= maximumPotions) break;
                    const auto potion = static_cast<RE::AlchemyItem*>(object);
                    auto record = MakeRecord("InventoryPotion");
                    record.object = object->formID;
                    record.values = {static_cast<std::uint64_t>(entry.first), potion->data.flags.underlying(), potion->effects.size()};
                    strncpy_s(record.text.data(), record.text.size(), object->GetName(), _TRUNCATE);
                    Push(record);
                    std::size_t effects{};
                    for (const auto effect : potion->effects) {
                        if (!effect || !effect->baseEffect) continue;
                        if (effects++ >= maximumPotionEffects) break;
                        const auto base = effect->baseEffect;
                        auto detail = MakeRecord("InventoryPotionEffect");
                        detail.object = object->formID;
                        detail.values = {base->formID, base->data.flags.underlying(), static_cast<std::uint64_t>(base->data.primaryAV),
                            static_cast<std::uint64_t>(base->data.archetype), std::bit_cast<std::uint32_t>(effect->effectItem.magnitude),
                            effect->effectItem.duration, effect->effectItem.area};
                        strncpy_s(detail.text.data(), detail.text.size(), base->GetFullName(), _TRUNCATE);
                        Push(detail);
                    }
                    if (potion->effects.size() > maximumPotionEffects) Note("InventoryPotionEffectsTruncated", object->formID, potion->effects.size());
                }
                Note("PotionInventoryEnd", std::min(inventory.size(), maximumPotions), inventory.size() > maximumPotions);
            }

            void Snapshot() {
                if (!ready.load()) return;
                const auto player = RE::PlayerCharacter::GetSingleton();
                if (!player) { Note("PlayerUnavailable"); return; }
                auto record = MakeRecord("PlayerSnapshot");
                record.actor = player->formID;
                const auto ui = RE::UI::GetSingleton();
                record.values[0] = player->IsDead();
                record.values[1] = player->IsInCombat();
                record.values[2] = ui && ui->GameIsPaused();
                constexpr std::array attributes{RE::ActorValue::kHealth, RE::ActorValue::kMagicka, RE::ActorValue::kStamina};
                for (std::size_t index = 0; index < attributes.size(); ++index) {
                    record.values[3 + index * 2] = std::bit_cast<std::uint32_t>(player->GetActorValue(attributes[index]));
                    record.values[4 + index * 2] = std::bit_cast<std::uint32_t>(player->GetPermanentActorValue(attributes[index]) +
                        player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, attributes[index]));
                }
                record.values[9] = ui ? ui->numPausesGame : 0;
                record.values[10] = player->GetRace() ? player->GetRace()->formID : 0;
                record.values[11] = reinterpret_cast<std::uintptr_t>(player->GetRace());
                Push(record);
                if (verifiedRuntime) {
                    const REL::Relocation<std::uintptr_t> entry{REL::ID(38894)};
                    constexpr std::size_t innerCallOffset = 0x170;
                    Push(CaptureBranchTarget("PublicEquipHook", entry.address(), 0));
                    Push(CaptureBranchTarget("UAPNGEquipHook", entry.address() + innerCallOffset,
                        reinterpret_cast<std::uintptr_t>(EquipObjectOverRide::thunk)));
                }
                std::size_t effectCount{}, total{};
                RE::BGSKeyword* positive{}, *negative{};
                if (const auto swift = swiftBase.load()) {
                    const auto& profile = *swiftProfile.load();
                    constexpr std::uintptr_t positiveKeywordOffset = 0x20, negativeKeywordOffset = 0x28;
                    ReadMemory(swift + profile.utility + positiveKeywordOffset, positive);
                    ReadMemory(swift + profile.utility + negativeKeywordOffset, negative);
                }
                if (const auto effects = player->GetActiveEffectList()) {
                    for (const auto effect : *effects) {
                        ++total;
                        if (!effect || !effect->effect || !effect->effect->baseEffect) continue;
                        const auto base = effect->effect->baseEffect;
                        if (++effectCount > maximumEffects) continue;
                        auto active = MakeRecord("ActiveEffect");
                        active.actor = player->formID;
                        active.object = base->formID;
                        active.values = {effect->spell ? effect->spell->formID : 0, effect->source ? effect->source->formID : 0,
                            effect->flags.underlying(), static_cast<std::uint64_t>(base->data.primaryAV),
                            static_cast<std::uint64_t>(base->data.archetype), std::bit_cast<std::uint32_t>(effect->elapsedSeconds),
                            std::bit_cast<std::uint32_t>(effect->duration), std::bit_cast<std::uint32_t>(effect->magnitude),
                            positive && std::ranges::contains(base->GetKeywords(), positive),
                            negative && std::ranges::contains(base->GetKeywords(), negative), base->data.flags.underlying()};
                        strncpy_s(active.text.data(), active.text.size(), base->GetFullName(), _TRUNCATE);
                        Push(active);
                    }
                }
                Note("EffectCount", total, effectCount);
                if (effectCount > maximumEffects) Note("ActiveEffectsTruncated", effectCount, maximumEffects);
                InventorySnapshot(player);
                SwiftSnapshot();
            }

            void Run(std::stop_token stop) {
                std::array<Record, 256> batch;
                auto nextSnapshot = std::chrono::steady_clock::now();
                while (true) {
                    if (!stop.stop_requested() && modulesRequested.exchange(false)) Modules();
                    std::size_t available{};
                    {
                        std::unique_lock lock(mutex);
                        wake.wait_for(lock, std::chrono::milliseconds(200), [&] { return count || stop.stop_requested(); });
                        available = std::min(count, batch.size());
                        for (std::size_t index = 0; index < available; ++index) batch[index] = ring[(begin + index) % ringSize];
                        begin = (begin + available) % ringSize;
                        count -= available;
                    }
                    if (stop.stop_requested() && available == 0) break;
                    for (std::size_t index = 0; index < available; ++index) Write(batch[index]);
                    if (const auto lost = drops.exchange(0)) {
                        totalDrops.fetch_add(lost);
                        log->error("Diagnostic ring overflow dropped={} records", lost);
                    }
                    log->flush();
                    const auto now = std::chrono::steady_clock::now();
                    if (!stop.stop_requested() && now >= nextSnapshot && ready.load() && !pending.exchange(true)) {
                        try {
                            SKSE::GetTaskInterface()->AddTask([this] {
                                try { Snapshot(); }
                                catch (...) { Note("SnapshotException"); }
                                pending.store(false);
                            });
                        } catch (...) { pending.store(false); Note("SnapshotEnqueueException"); }
                        nextSnapshot = now + snapshotInterval;
                    }
                }
                totalDrops.fetch_add(drops.exchange(0));
                log->info("SessionEnd droppedRecords={}", totalDrops.load());
                log->flush();
            }

            RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* event, RE::BSTEventSource<RE::TESDeathEvent>*) override {
                if (event && event->actorDying && event->actorDying->formID == playerRefID) {
                    Note("PlayerDeath", event->dead, event->actorKiller ? event->actorKiller->formID : 0);
                    wake.notify_all();
                }
                return RE::BSEventNotifyControl::kContinue;
            }

            RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* event,
                                                  RE::BSTEventSource<RE::TESContainerChangedEvent>*) override {
                if (event && (event->oldContainer == playerRefID || event->newContainer == playerRefID)) {
                    auto record = MakeRecord("PlayerInventoryMove");
                    record.object = event->baseObj;
                    record.values = {event->oldContainer, event->newContainer, static_cast<std::uint64_t>(event->itemCount), event->reference.native_handle()};
                    Push(record);
                }
                return RE::BSEventNotifyControl::kContinue;
            }

            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event,
                                                  RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                if (event) {
                    constexpr std::array relevant{"Loading Menu", "InventoryMenu", "TweenMenu", "FavoritesMenu", "HUD Menu", "Main Menu", "Journal Menu", "Console"};
                    for (const auto name : relevant) if (event->menuName == name) { Note(name, event->opening); break; }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        Recorder& GetRecorder() { static Recorder recorder; return recorder; }

        const char* RouteName(Route route) {
            constexpr std::array names{"PublicForward", "PausedForward", "InvalidForward", "ExcludedForward", "BlacklistedForward", "NonPotionForward",
                "AnimationAccepted", "AnimationRejectedForward", "BusyForward", "BusyBlocked", "SpamForward", "Unknown"};
            return names[static_cast<std::size_t>(route)];
        }
    }

    EquipTrace::EquipTrace(Stage a_stage, RE::ActorEquipManager* manager, RE::Actor* actor, RE::TESBoundObject* object, std::uintptr_t request)
        : stage(a_stage), uncaughtOnEntry(std::uncaught_exceptions()) {
        if (actor && actor->formID != playerRefID && object && object->formType != RE::FormType::AlchemyItem) return;
        auto& recorder = GetRecorder();
        if (!recorder.initialized.load()) return;
        id = recorder.nextID.fetch_add(1);
        parent = currentTrace;
        currentTrace = id;
        actorID = actor ? actor->formID : 0;
        objectID = object ? object->formID : 0;
        auto record = MakeRecord(stage == Stage::Public ? "PublicEntry" : "InnerEntry");
        record.actor = actorID;
        record.object = objectID;
        record.parent = parent;
        record.values = {reinterpret_cast<std::uintptr_t>(manager), reinterpret_cast<std::uintptr_t>(actor),
            reinterpret_cast<std::uintptr_t>(object), request, config::bExcludePlayer, config::bEnableSpamPotions, pause.load(),
            object ? static_cast<std::uint64_t>(object->formType.underlying()) : 0};
        if (actorID == playerRefID) record.frameCount = CaptureStackBackTrace(1, static_cast<DWORD>(record.frames.size()), record.frames.data(), nullptr);
        recorder.Push(record);
    }

    EquipTrace::~EquipTrace() {
        if (!id) return;
        auto record = MakeRecord(stage == Stage::Public ? "PublicExit" : "InnerExit");
        record.parent = parent;
        record.actor = actorID;
        record.object = objectID;
        record.values = {static_cast<std::uint64_t>(route), std::uncaught_exceptions() > uncaughtOnEntry};
        strncpy_s(record.text.data(), record.text.size(), RouteName(route), _TRUNCATE);
        GetRecorder().Push(record);
        currentTrace = parent;
    }

    void EquipTrace::SetRoute(Route value) {
        route = value;
        if (!id) return;
        auto record = MakeRecord(stage == Stage::Public ? "PublicRouteBegin" : "InnerRouteBegin");
        record.actor = actorID;
        record.object = objectID;
        record.parent = parent;
        record.values = {static_cast<std::uint64_t>(route)};
        strncpy_s(record.text.data(), record.text.size(), RouteName(route), _TRUNCATE);
        GetRecorder().Push(record);
    }

    void EquipTrace::SetArguments(RE::ExtraDataList* extra, std::uint32_t count, const RE::BGSEquipSlot* slot,
                                  bool queue, bool force, bool sounds, bool immediate) {
        if (!id) return;
        auto record = MakeRecord("PublicArguments");
        record.actor = actorID;
        record.object = objectID;
        record.values = {reinterpret_cast<std::uintptr_t>(extra), count, reinterpret_cast<std::uintptr_t>(slot), queue, force, sounds, immediate};
        GetRecorder().Push(record);
    }

    void Note(const char* label, std::uint64_t first, std::uint64_t second, std::uintptr_t address) {
        auto record = MakeRecord(label);
        record.values = {first, second, address};
        GetRecorder().Push(record);
    }

    void Initialize() try {
        auto& recorder = GetRecorder();
        std::array<wchar_t, MAX_PATH> path{};
        GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        recorder.executable = path.data();
        recorder.executableHash = FileHash(recorder.executable);
        recorder.verifiedRuntime = REL::Module::get().version() == REL::Version(1, 6, 1170, 0) &&
            (recorder.executableHash == runtimeHash || recorder.executableHash == unpackedRuntimeHash);
        const auto directory = SKSE::log::log_directory();
        if (!directory) return;
        recorder.log = std::make_shared<spdlog::logger>("PotionDiagnostics", std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            (*directory / "UAPNG-PotionDiagnostics.log").string(), 20 * 1024 * 1024, 3, true));
        recorder.log->set_level(spdlog::level::trace);
        recorder.log->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        recorder.log->info("UAPNG one-shot potion diagnostics build={} runtime={} verified1170={}", UAPNG_DIAGNOSTIC_BUILD_ID,
                           REL::Module::get().version().string(), recorder.verifiedRuntime);
        recorder.log->info("Numeric values are hexadecimal unless separately formatted. IDs are form IDs; ticks are monotonic milliseconds. Trace parent links public and inner calls. RouteBegin precedes forwarding; Exit values are route and unwinding (0 means normal return).");
        recorder.log->info("Hook values: site,expectedTarget,firstTarget,resolvedTarget,siteBytes,readFlags,indirectSlot,relaySlot,relayBytes. Read flags: 1=site,2=relative,4=indirect,8=target,16=relay,32=relayTarget. A different head target can still chain to the expected hook.");
        recorder.log->info("Equip entry values: manager,actor,object,request,excludePlayer,spam,pause,objectType. PublicArguments: extra,count,slot,queue,force,sounds,immediate. EffectiveSettings: excludePlayer,spam,movement,notifications,blacklistCount,allowedCount.");
        recorder.log->info("PlayerSnapshot values: dead,combat,paused,health,maxHealth,magicka,maxMagicka,stamina,maxStamina,uiPauseCount,raceForm,racePointer. Actor values are float bits.");
        recorder.log->info("Swift category values: enabled,stopper,combatOnly,threshold(float bits),attribute,useFood,poison,bestValue,hotkey. Scheduler: enabled,elapsed(float bits),delta(float bits),deltaAddress,hooksite,originalCallee,readMask (63=all reads succeeded).");
        recorder.log->info("SwiftEligibilityPointers: vampireRace,werewolfRace,brawlQuest,brawlStage,cachedPlayerAddress,player,cachedUIAddress,UI,cachedControlsAddress,controls,readMask (15=all reads succeeded). SwiftEffectName: categoryIndex,length,capacity; text is bounded effect name.");
        recorder.log->info("ActiveEffect values: spell,source,flags,attribute,archetype,elapsed,duration,magnitude(float bits),positiveKeyword,negativeKeyword,effectFlags. Text is effect name. EffectCount: total,matching; ActiveEffectsTruncated records overflow.");
        recorder.log->info("InventoryPotion: count,alchemyFlags,effectCount; InventoryPotionEffect: effectForm,effectFlags,attribute,archetype,magnitude(float bits),duration,area. InventoryBegin: potionTypes,inventoryChangesPresent; InventoryEnd: recorded,truncated. PlayerInventoryMove: oldContainer,newContainer,count,referenceHandle.");
        recorder.initialized.store(true);
        recorder.worker = std::jthread([&recorder](std::stop_token stop) {
            try { recorder.Run(stop); }
            catch (...) {
                recorder.initialized.store(false);
                recorder.ready.store(false);
                OutputDebugStringA("UAPNG potion diagnostic recorder stopped after a logging exception.\n");
            }
        });
    } catch (...) {
        GetRecorder().initialized.store(false);
        OutputDebugStringA("UAPNG potion diagnostics could not initialize.\n");
    }

    bool VerifiedRuntime1170() { return GetRecorder().initialized.load() && GetRecorder().verifiedRuntime; }

    void DataLoaded() {
        auto& recorder = GetRecorder();
        if (!recorder.initialized.load()) return;
        if (const auto events = RE::ScriptEventSourceHolder::GetSingleton()) {
            events->AddEventSink<RE::TESDeathEvent>(&recorder);
            events->AddEventSink<RE::TESContainerChangedEvent>(&recorder);
        }
        if (const auto ui = RE::UI::GetSingleton()) ui->AddEventSink<RE::MenuOpenCloseEvent>(&recorder);
        recorder.modulesRequested.store(true);
        recorder.wake.notify_all();
    }

    void OnMessage(SKSE::MessagingInterface::Message* message) {
        if (!message) return;
        auto& recorder = GetRecorder();
        Note("SKSEMessage", message->type, message->dataLen, reinterpret_cast<std::uintptr_t>(message->data));
        if (message->type == SKSE::MessagingInterface::kPreLoadGame) recorder.ready.store(false);
        if (message->type == SKSE::MessagingInterface::kPostLoadGame || message->type == SKSE::MessagingInterface::kNewGame) recorder.ready.store(true);
        recorder.wake.notify_all();
    }

    void SettingsLoaded() {
        auto settings = MakeRecord("EffectiveSettings");
        settings.values = {config::bExcludePlayer, config::bEnableSpamPotions, static_cast<std::uint64_t>(config::iMovementType),
            config::bEnableNotifications, config::potionFormLists.blacklisted.size(), config::potionFormLists.allowed.size()};
        GetRecorder().Push(settings);
        auto record = MakeRecord("INIPath");
        const auto path = config::GetIniFilePath();
        for (std::size_t offset = 0; offset < path.size(); offset += record.text.size() - 1) {
            strncpy_s(record.text.data(), record.text.size(), path.c_str() + offset, record.text.size() - 1);
            record.values[0] = offset;
            GetRecorder().Push(record);
        }
    }
}
