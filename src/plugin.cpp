#include "config.h"
#include "equip_object_override.h"
#include "logger.h"
#include "animation_event_sink.h"
#include "actor_data.h"


void PluginReady(SKSE::MessagingInterface::Message* a_msg) {
    switch (a_msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            config::ReadPotionFormLists();
            EquipObjectOverRide::Install();
            logger::info("Ultimate Potion Animation NG - Plugin Ready");            
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            {
                std::map<ActorCustom, std::shared_ptr<AnimationEventSink>> eventSinks;
                {
                    const std::lock_guard lock(actorAnimationState.mutex);
                    pause.store(true);
                    ++actorAnimationState.loadGeneration;
                    eventSinks.swap(actorAnimationState.actorEventSink);
                }

                auto settings = config::ReadSettings();
                config::iMovementType = settings["iMovementType"];
                config::bEnableNotifications = settings["bEnableNotifications"] != 0;
                config::bEnableSpamPotions = settings["bEnableSpamPotions"] != 0;
                config::bExcludePlayer = settings["bExcludePlayer"] != 0;

                for (auto& eventSink : eventSinks | std::views::values) {
                    CleanupAnimationEvent(eventSink.get(), true);
                }

                
                logger::info("Ultimate Potion Animation NG - Post Load Game");
                logger::info("Settings loaded: iMovementType: {}, bEnableNotifications: {}, bEnableSpamPotions: {}", config::iMovementType, config::bEnableNotifications, config::bEnableSpamPotions);

                pause.store(false);
            }
            break;
        default:
            break;
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    SetupLog();
    SKSE::GetMessagingInterface()->RegisterListener(PluginReady);
    return true;
}
