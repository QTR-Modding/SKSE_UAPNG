#include "actor_data.h"
#include "animation_event_sink.h"
#include "function_library.h"
#include "config.h"
#include "potion_diagnostics.h"

AnimationEventSink::AnimationEventSink(RE::ActorEquipManager* equipManager, RE::Actor* actor,
                                       RE::TESBoundObject* object, const std::uint64_t unk,
                                       RE::BSTSmartPointer<RE::BShkbAnimationGraph> graph)
    : a_equipManager(equipManager), actorKey(actor), a_actor(actor->GetHandle()), a_object(object), a_unk(unk),
      animationGraph(std::move(graph)) {}

RE::BSEventNotifyControl AnimationEventSink::ProcessEvent(
    const RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>*) {
    const auto keepAlive = shared_from_this();
    const auto currentState = state.load();
    if (currentState == State::StopRequested || currentState == State::Retiring) return RE::BSEventNotifyControl::kContinue;
    if (a_event) {
        const auto actor = a_actor.get();
        if (const RE::BSFixedString& eventTag = a_event->tag; eventTag == "AnimObjLoad" && a_event->payload == "AnimObjectDrinkPotion") {
            logger::trace("Received potion animation event '{}' for Actor {:08X}", eventTag.c_str(), actorKey.refid);
            if (actor) {
                GetActorData(actorKey).waitForUIHeal.store(true);
            }
        }
        else if (eventTag == "IUHeal") {
            logger::trace("Received potion animation event '{}' for Actor {:08X}", eventTag.c_str(), actorKey.refid);
            if (ActorData& data = GetActorData(actorKey); actor && data.waitForUIHeal.load()) {
                data.waitForUIHeal.store(false);
                data.bDrinking.store(false);
                ProcessPotionAnimationEvent(this, actor.get());
            }
        } else if (eventTag == "DrinkPotionStop" || eventTag == "EnableBumper" || eventTag == "MTState") {
            logger::trace("Received potion animation stop event '{}' for Actor {:08X}", eventTag.c_str(), actorKey.refid);
            if (actor) {
                logger::trace("Removing event sink for Actor: '{}'", actor->GetName());
            }
            CleanupAnimationEvent(this, true);
        }
    }
    return RE::BSEventNotifyControl::kContinue;
}

void ProcessPotionAnimationEvent(const AnimationEventSink* eventSink, RE::Actor* actor) {
    PotionDiagnostics::Note("AnimationConsumptionBegin", eventSink->actorKey.refid, eventSink->actorKey.formid,
                            reinterpret_cast<std::uintptr_t>(eventSink->a_object));
    logger::info("Processing potion animation event for Actor: '{}'", actor->GetName());
    eventSink->a_equipManager->EquipObject(actor, eventSink->a_object);
    PotionDiagnostics::Note("AnimationConsumptionEnd", eventSink->actorKey.refid, eventSink->actorKey.formid);
    if (config::bEnableSpamPotions && actor->IsPlayerRef()) {
        RE::SendUIMessage::SendInventoryUpdateMessage(actor,nullptr);
    }
}

std::shared_ptr<AnimationEventSink> CreateEventSink(RE::Actor* a_actor, RE::ActorEquipManager* a_self,
                                                  RE::TESBoundObject* a_object, const std::uint64_t a_unk) {
    const auto actor_custom = ActorCustom(a_actor);
    std::uint64_t loadGeneration;
    {
        const std::lock_guard lock(actorAnimationState.mutex);
        if (pause.load()) return {};
        loadGeneration = actorAnimationState.loadGeneration;
    }

    auto graph = GetActorAnimationGraph(a_actor);
    if (!graph) return {};
    auto eventSink = std::make_shared<AnimationEventSink>(a_self, a_actor, a_object, a_unk, std::move(graph));
    const std::lock_guard lock(actorAnimationState.mutex);
    if (pause.load() || loadGeneration != actorAnimationState.loadGeneration) return {};
    actorAnimationState.actorEventSink.emplace(actor_custom, eventSink);
    return eventSink;
}

void CleanupAnimationEvent(AnimationEventSink* eventSink, const bool unHideWeapon) {
    auto expected = AnimationEventSink::State::Starting;
    if (eventSink->state.compare_exchange_strong(expected, AnimationEventSink::State::StopRequested)) return;
    expected = AnimationEventSink::State::Active;
    if (eventSink->state.compare_exchange_strong(expected, AnimationEventSink::State::Retiring)) {
        eventSink->Cleanup(unHideWeapon);
    }
}

void AnimationEventSink::CompleteSetup() {
    auto expected = State::Starting;
    if (state.compare_exchange_strong(expected, State::Active)) return;
    expected = State::StopRequested;
    if (state.compare_exchange_strong(expected, State::Retiring)) {
        Cleanup(true);
    }
}

void AnimationEventSink::Cleanup(const bool unHideWeapon) {
    PotionDiagnostics::Note("AnimationCleanupBegin", actorKey.refid, actorKey.formid);
    if (unHideWeapon) {
        RestoreWeaponVisibility(hiddenWeaponNodes);
    }
    const auto actor = a_actor.get();
    if (actor) {
        logger::trace("Cleaning up AnimationEventSink for Actor: '{}'", actor->GetName());
        SendAnimationEvent(actor.get(), "HeadtrackingOn");
        SetAnimationVariableBool(actor.get(), "tdmHeadtrackingBehavior", true);
        SetAnimationVariableBool(actor.get(), "bPlayEquipSound", false);
    }

    animationGraph->GetEventSource<RE::BSAnimationGraphEvent>()->RemoveEventSink(this);
    RemoveEventSink(this);
    ActorData& data = GetActorData(actorKey);
    data.bDrinking.store(false);
    data.waitForUIHeal.store(false);
    data.bAnimationInProgress.store(false);
    PotionDiagnostics::Note("AnimationCleanupEnd", actorKey.refid, actorKey.formid);
    logger::trace("Potion animation cleanup completed for Actor {:08X}; listener removed and busy flags cleared", actorKey.refid);
}

void RemoveEventSink(const AnimationEventSink* sink) {
    {
        const std::lock_guard lock(actorAnimationState.mutex);
        if (const auto it = actorAnimationState.actorEventSink.find(sink->actorKey);
            it != actorAnimationState.actorEventSink.end() && it->second.get() == sink) {
            actorAnimationState.actorEventSink.erase(it);
        }
    }
    logger::info("AnimationEventSink removed.");
}

ActorData& GetActorData(const ActorCustom& actor) {
    const std::lock_guard lock(actorAnimationState.mutex);
    return actorAnimationState.actorDataMap[actor];
}

ActorData* FindActorData(const ActorCustom& actor) {
    const std::lock_guard lock(actorAnimationState.mutex);
    const auto it = actorAnimationState.actorDataMap.find(actor);
    return it != actorAnimationState.actorDataMap.end() ? &it->second : nullptr;
}

ActorCustom::ActorCustom(const RE::FormID formid, const RE::FormID refid): formid(formid), refid(refid) {}

ActorCustom::ActorCustom(RE::Actor* a_actor)
{
    refid = a_actor ? a_actor->GetFormID() : 0;
    formid = a_actor ? a_actor->GetBaseObject()->GetFormID() : 0;
}

bool ActorCustom::operator==(const ActorCustom& other) const {
    return formid == other.formid && refid == other.refid;
}

bool ActorCustom::operator==(const RE::Actor* actor) const {
    if (!actor) return false;
    return refid == actor->GetFormID() && refid == actor->GetBaseObject()->GetFormID();
}

bool ActorCustom::operator<(const ActorCustom& other) const {
    return refid < other.refid;
}

RE::Actor* ActorCustom::get() const {
    auto* actor_temp = RE::TESForm::LookupByID<RE::Actor>(refid);
    return actor_temp && actor_temp->GetBaseObject()->GetFormID() == formid ? actor_temp : nullptr;
}
