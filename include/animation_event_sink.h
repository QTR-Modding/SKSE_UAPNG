#pragma once

struct AnimationEventSink;

void ProcessPotionAnimationEvent(const AnimationEventSink* eventSink, RE::Actor* actor);
std::vector<AnimationEventSink*> GetAllEventSinks();
std::shared_ptr<AnimationEventSink> CreateEventSink(RE::Actor* a_actor, RE::ActorEquipManager* a_self,
                                                  RE::TESBoundObject* a_object, std::uint64_t a_unk);
void CleanupAnimationEvent(AnimationEventSink* eventSink, bool unHideWeapon);
void RemoveEventSink(const AnimationEventSink* sink);

struct ActorCustom
{
    RE::FormID formid;
    RE::FormID refid;

    ActorCustom(RE::FormID formid, RE::FormID refid);
    ActorCustom(RE::Actor* a_actor);

    bool operator==(const ActorCustom& other) const;

    bool operator==(const RE::Actor* actor) const;

    bool operator<(const ActorCustom& other) const;

    explicit operator bool() const {return get()!=nullptr;}

    [[nodiscard]] RE::Actor* get() const;
};

struct AnimationEventSink : public RE::BSTEventSink<RE::BSAnimationGraphEvent>,
                            public std::enable_shared_from_this<AnimationEventSink> {
    enum class State { Starting, Active, StopRequested, Retiring };

    AnimationEventSink(RE::ActorEquipManager* equipManager, RE::Actor* actor, RE::TESBoundObject* object,
                       std::uint64_t unk, RE::BSTSmartPointer<RE::BShkbAnimationGraph> graph);
    RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event,
                                          RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override;
    void CompleteSetup();
    void Cleanup(bool unHideWeapon);

    RE::ActorEquipManager* a_equipManager{nullptr};
    const ActorCustom actorKey;
    RE::ActorHandle a_actor;
    RE::TESBoundObject* a_object{nullptr};
    std::uint64_t a_unk{0};
    const RE::BSTSmartPointer<RE::BShkbAnimationGraph> animationGraph;
    std::vector<RE::NiPointer<RE::NiAVObject>> hiddenWeaponNodes;
    std::atomic<State> state{State::Starting};
};
