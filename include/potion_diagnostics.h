#pragma once

namespace PotionDiagnostics {
    enum class Stage { Public, Inner };
    enum class Route { PublicForward, PausedForward, InvalidForward, ExcludedForward, BlacklistedForward,
                       NonPotionForward, AnimationAccepted, AnimationRejectedForward, BusyForward, BusyBlocked, SpamForward, Unknown };

#ifdef UAPNG_POTION_DIAGNOSTICS
    class EquipTrace {
    public:
        EquipTrace(Stage stage, RE::ActorEquipManager* manager, RE::Actor* actor, RE::TESBoundObject* object,
                   std::uintptr_t request = 0);
        ~EquipTrace();
        void SetRoute(Route route);
        void SetArguments(RE::ExtraDataList* extra, std::uint32_t count, const RE::BGSEquipSlot* slot,
                          bool queue, bool force, bool sounds, bool immediate);
    private:
        std::uint64_t id{}, parent{};
        RE::FormID actorID{}, objectID{};
        Route route{Route::Unknown};
        Stage stage{};
        int uncaughtOnEntry{};
    };
    void Note(const char* label, std::uint64_t first = 0, std::uint64_t second = 0, std::uintptr_t address = 0);
    void Initialize();
    void DataLoaded();
    void OnMessage(SKSE::MessagingInterface::Message* message);
    void SettingsLoaded();
    void InstallEntryProbe();
    bool VerifiedRuntime1170();
#else
    class EquipTrace {
    public:
        EquipTrace(Stage, RE::ActorEquipManager*, RE::Actor*, RE::TESBoundObject*, std::uintptr_t = 0) {}
        void SetRoute(Route) {}
        void SetArguments(RE::ExtraDataList*, std::uint32_t, const RE::BGSEquipSlot*, bool, bool, bool, bool) {}
    };
    inline void Note(const char*, std::uint64_t = 0, std::uint64_t = 0, std::uintptr_t = 0) {}
    inline void Initialize() {}
    inline void DataLoaded() {}
    inline void OnMessage(SKSE::MessagingInterface::Message*) {}
    inline void SettingsLoaded() {}
    inline void InstallEntryProbe() {}
    inline bool VerifiedRuntime1170() { return false; }
#endif
}
