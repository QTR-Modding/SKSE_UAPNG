#pragma once

namespace logger = SKSE::log;

void SetupLog();

#ifdef UAPNG_SAVE_LOAD_DIAGNOSTICS
void TraceActorBases(const char* stage, RE::Actor* actor, const RE::TESForm* item);
void TraceTrackedActorBases(const char* stage);
#endif
