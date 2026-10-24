#pragma once

#include "Logger.h"

#include <spdlog/sinks/basic_file_sink.h>

#ifdef UAPNG_SAVE_LOAD_DIAGNOSTICS
#include "actor_data.h"
#include <Windows.h>
#endif

namespace logger = SKSE::log;

void SetupLog() {
    auto logsFolder = SKSE::log::log_directory();
    if (!logsFolder) SKSE::stl::report_and_fail("SKSE log_directory not provided, logs disabled.");
    auto pluginName = SKSE::PluginDeclaration::GetSingleton()->GetName();
    auto logFilePath = *logsFolder / std::format("{}.log", pluginName);
    auto fileLoggerPtr = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), true);
    auto loggerPtr = std::make_shared<spdlog::logger>("log", std::move(fileLoggerPtr));
    spdlog::set_default_logger(std::move(loggerPtr));
#ifdef UAPNG_SAVE_LOAD_DIAGNOSTICS
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [thread %t] [%l] [%s:%#] %v");
#endif
#if !defined(NDEBUG) || defined(UAPNG_SAVE_LOAD_DIAGNOSTICS)
    spdlog::set_level(spdlog::level::trace);
    spdlog::flush_on(spdlog::level::trace);
#else
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
#endif
    logger::info("Name of the plugin is {}.", pluginName);
    logger::info("Version of the plugin is {}.", SKSE::PluginDeclaration::GetSingleton()->GetVersion());
#ifdef UAPNG_SAVE_LOAD_DIAGNOSTICS
    logger::info("UAPNG save/load diagnostic 1; runtime {}; module={:016X}; NPC vtable={:016X}; leveled-extra vtable={:016X}",
                 REL::Module::get().version().string(), REL::Module::get().base(),
                 RE::TESNPC::VTABLE[0].address(), RE::ExtraLeveledCreature::VTABLE[0].address());
#endif
}

#ifdef UAPNG_SAVE_LOAD_DIAGNOSTICS
namespace {
    struct FormSnapshot {
        bool readable{};
        std::uintptr_t vtable{};
        RE::FormID formID{};
        RE::FormType formType{};
    };

    template <typename T>
    bool ReadDiagnosticMemory(std::uintptr_t address, T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        SIZE_T bytesRead{};
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                                 &value, sizeof(value), &bytesRead) && bytesRead == sizeof(value);
    }

    FormSnapshot TraceForm(const char* stage, RE::FormID refID, const char* role, std::uintptr_t address) {
        FormSnapshot snapshot;
        std::array<std::byte, sizeof(RE::TESForm)> header{};
        snapshot.readable = ReadDiagnosticMemory(address, header);
        if (snapshot.readable) {
            std::memcpy(&snapshot.vtable, header.data(), sizeof(snapshot.vtable));
            std::memcpy(&snapshot.formID, header.data() + offsetof(RE::TESForm, formID), sizeof(snapshot.formID));
            snapshot.formType = static_cast<RE::FormType>(std::to_integer<std::uint8_t>(header[offsetof(RE::TESForm, formType)]));
        }
        logger::trace("[save-load] stage={} ref={:08X} role={} address={:016X} readable={} vtable={:016X} form={:08X} type={:02X}",
                      stage, refID, role, address, snapshot.readable, snapshot.vtable, snapshot.formID,
                      static_cast<std::uint8_t>(snapshot.formType));
        return snapshot;
    }

    void TraceLeveledBases(const char* stage, RE::FormID refID, std::uintptr_t actorAddress) {
        // Observe the extra list without calling through potentially damaged objects.
        const auto headAddress = actorAddress + offsetof(RE::TESObjectREFR, extraList) +
                                 REL::VersionShift(0, sizeof(void*), SKSE::RUNTIME_SSE_1_6_629);
        std::uintptr_t node{};
        if (!ReadDiagnosticMemory(headAddress, node)) {
            logger::trace("[save-load] stage={} ref={:08X} extra-list head unreadable at {:016X}", stage, refID, headAddress);
            return;
        }

        struct ExtraNode {
            std::uintptr_t vtable;
            std::uintptr_t next;
        };
        static_assert(sizeof(ExtraNode) == sizeof(RE::BSExtraData));
        static_assert(offsetof(ExtraNode, next) == offsetof(RE::BSExtraData, next));
        constexpr auto maximumExtraNodes = sizeof(RE::BaseExtraList::PresenceBitfield) * std::numeric_limits<std::uint8_t>::digits;
        std::size_t visited{};
        for (; node && visited < maximumExtraNodes; ++visited) {
            ExtraNode extra{};
            if (!ReadDiagnosticMemory(node, extra)) {
                logger::trace("[save-load] stage={} ref={:08X} extra node unreadable at {:016X}; visited={}", stage, refID, node, visited);
                return;
            }
            if (extra.vtable == RE::ExtraLeveledCreature::VTABLE[0].address()) {
                std::array<std::uintptr_t, 2> bases{};
                static_assert(offsetof(RE::ExtraLeveledCreature, templateBase) ==
                              offsetof(RE::ExtraLeveledCreature, originalBase) + sizeof(std::uintptr_t));
                const bool readable = ReadDiagnosticMemory(node + offsetof(RE::ExtraLeveledCreature, originalBase), bases);
                logger::trace("[save-load] stage={} ref={:08X} leveled-extra={:016X} readable={}; visited={}", stage, refID, node, readable, visited);
                if (readable) {
                    TraceForm(stage, refID, "original-base", bases[0]);
                    TraceForm(stage, refID, "template-base", bases[1]);
                }
                return;
            }
            node = extra.next;
        }
        logger::trace("[save-load] stage={} ref={:08X} leveled-extra scan={}; visited={}",
                      stage, refID, node ? "limit reached" : "no matching vtable", visited);
    }
}

void TraceActorBases(const char* stage, RE::Actor* actor, const RE::TESForm* item) {
    const auto actorAddress = reinterpret_cast<std::uintptr_t>(actor);
    const auto snapshot = TraceForm(stage, 0, "actor", actorAddress);
    if (!snapshot.readable || snapshot.formType != RE::FormType::ActorCharacter) return;
    if (item) {
        TraceForm(stage, snapshot.formID, "item", reinterpret_cast<std::uintptr_t>(item));
    }
    std::uintptr_t base{};
    const auto baseAddress = actorAddress + offsetof(RE::TESObjectREFR, data) + offsetof(RE::OBJ_REFR, objectReference);
    if (ReadDiagnosticMemory(baseAddress, base)) {
        TraceForm(stage, snapshot.formID, "current-base", base);
    } else {
        logger::trace("[save-load] stage={} ref={:08X} current-base pointer unreadable at {:016X}", stage, snapshot.formID, baseAddress);
    }
    TraceLeveledBases(stage, snapshot.formID, actorAddress);
}

void TraceTrackedActorBases(const char* stage) {
    std::vector<ActorCustom> actors;
    {
        const std::lock_guard lock(actorAnimationState.mutex);
        actors.reserve(actorAnimationState.actorDataMap.size());
        for (const auto& [actor, data] : actorAnimationState.actorDataMap) {
            actors.push_back(actor);
        }
    }
    logger::trace("[save-load] stage={} tracked actors={}", stage, actors.size());
    for (const auto& actor : actors) {
        const auto form = RE::TESForm::LookupByID(actor.refid);
        logger::trace("[save-load] stage={} tracked ref={:08X} initial-base={:08X} resolved={:016X}",
                      stage, actor.refid, actor.formid, reinterpret_cast<std::uintptr_t>(form));
        if (form) {
            TraceActorBases(stage, reinterpret_cast<RE::Actor*>(form), nullptr);
        }
    }
}
#endif
