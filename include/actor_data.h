#pragma once
#include "animation_event_sink.h"

struct ActorData {
    ActorData() : lastActor(nullptr), lastObject(nullptr), bAnimationInProgress(false), bDrinking(false), waitForUIHeal(false) {}

    RE::Actor* lastActor;
    RE::TESBoundObject* lastObject;
    std::atomic<bool> bAnimationInProgress;
    std::atomic<bool> bDrinking;
    std::atomic<bool> waitForUIHeal;
};

struct ActorAnimationState {
    std::mutex mutex;
    std::uint64_t loadGeneration{};
    std::map<ActorCustom, ActorData> actorDataMap;
    std::map<ActorCustom, std::shared_ptr<AnimationEventSink>> actorEventSink;
};

inline ActorAnimationState actorAnimationState;

ActorData& GetActorData(const ActorCustom& actor);
ActorData* FindActorData(const ActorCustom& actor);

enum class ActorState { PLAYER_FIRST_PERSON, PLAYER_THIRD_PERSON, OTHER };
