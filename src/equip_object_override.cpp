#include "equip_object_override.h"
#include "actor_data.h"
#include "function_library.h"
#include "pch.h"
#include "config.h"
#include "potion_diagnostics.h"

void EquipObjectOverRide::thunk(RE::ActorEquipManager* a_self, RE::Actor* a_actor, RE::TESBoundObject* a_object, std::uint64_t a_unk) {
    PotionDiagnostics::EquipTrace trace(PotionDiagnostics::Stage::Inner, a_self, a_actor, a_object, a_unk);

    if (pause.load()) {
        trace.SetRoute(PotionDiagnostics::Route::PausedForward);
        logger::warn("Pause is active, skipping EquipObjectOverRide::thunk");
        func(a_self, a_actor, a_object, a_unk);
        return;
    }

    if (!a_object || !a_actor || !a_self || config::bExcludePlayer && a_actor->IsPlayerRef()) {
        trace.SetRoute(a_object && a_actor && a_self ? PotionDiagnostics::Route::ExcludedForward : PotionDiagnostics::Route::InvalidForward);
        func(a_self, a_actor, a_object, a_unk);
        return;
    }

    if (IsObjectBlacklisted(a_object)) {
        trace.SetRoute(PotionDiagnostics::Route::BlacklistedForward);
        func(a_self, a_actor, a_object, a_unk);
        return;
    }

    if (CheckActorAndObjectKeywords(a_object, a_actor) || IsObjectAPotion(a_object) || IsObjectWhitelisted(a_object)) {
        const auto actor_custom = ActorCustom(a_actor);
        ActorData& data = GetActorData(actor_custom);
        bool animationInProgress = false;
        if (IsActorReadyForAnimation(a_actor ) && data.bAnimationInProgress.compare_exchange_strong(animationInProgress, true)) {
            data.bDrinking.store(true);
            AnimObjectReplaceModelByFormID(0x0D36C8, AlchObjectGetModelPath(a_object).c_str());
            if (!EquipPotion(a_actor, a_self, a_object, a_unk)) {
                trace.SetRoute(PotionDiagnostics::Route::AnimationRejectedForward);
                logger::trace("Using normal potion equip for Actor: '{}' ({:08X}), potion: '{}' ({:08X}) after animation setup failed",
                              a_actor->GetName(), a_actor->GetFormID(), a_object->GetName(), a_object->GetFormID());
                func(a_self, a_actor, a_object, a_unk);
                return;
            }
            trace.SetRoute(PotionDiagnostics::Route::AnimationAccepted);
            if (!config::bEnableSpamPotions) {
                CloseMenu();
            }
        } else {            
            if (!config::bEnableSpamPotions && data.bAnimationInProgress.load()){
                if (!data.bDrinking) {                
                    data.bDrinking = true;
                    NotifyActorDrinkingLastPotion(a_actor, a_object);
                    trace.SetRoute(PotionDiagnostics::Route::BusyForward);
                    func(a_self, a_actor, a_object, a_unk);
                } else {
                    trace.SetRoute(PotionDiagnostics::Route::BusyBlocked);
                    if (a_actor->GetActorBase()->IsPlayer()) {
                        SendDebugNotification("You can't use a potion right now");
                    }
                }
            } else {
                NotifyActorDrinkingLastPotion(a_actor, a_object);
                trace.SetRoute(PotionDiagnostics::Route::SpamForward);
                func(a_self, a_actor, a_object, a_unk);
            }                             
        }
    } else {
        trace.SetRoute(PotionDiagnostics::Route::NonPotionForward);
        func(a_self, a_actor, a_object, a_unk);
    }
}

void EquipObjectOverRide::Install() {
    REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(37938, 38894), REL::VariantOffset(0xE5, 0x170, 0xE5) };
    stl::write_thunk_call<EquipObjectOverRide>(target.address());
    PotionDiagnostics::Note("UAPNGInnerHook", target.address(), func.address(), reinterpret_cast<std::uintptr_t>(thunk));
    logger::info("Ultimate Potion Animation NG - Hook Installed");
}
