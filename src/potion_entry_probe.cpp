#include "pch.h"
#include "potion_diagnostics.h"

#ifdef UAPNG_POTION_DIAGNOSTICS
#include <Windows.h>

namespace {
    void TraceEquipObject(RE::ActorEquipManager* manager, RE::Actor* actor, RE::TESBoundObject* object,
                         RE::ExtraDataList* extra, std::uint32_t count, const RE::BGSEquipSlot* slot,
                         bool queue, bool force, bool sounds, bool immediate);

    REL::Relocation<decltype(TraceEquipObject)> originalEquipObject;
    SKSE::Trampoline entryTrampoline{"UAPNG potion entry diagnostics"};

    void TraceEquipObject(RE::ActorEquipManager* manager, RE::Actor* actor, RE::TESBoundObject* object,
                         RE::ExtraDataList* extra, std::uint32_t count, const RE::BGSEquipSlot* slot,
                         bool queue, bool force, bool sounds, bool immediate) {
        PotionDiagnostics::EquipTrace trace(PotionDiagnostics::Stage::Public, manager, actor, object);
        trace.SetArguments(extra, count, slot, queue, force, sounds, immediate);
        trace.SetRoute(PotionDiagnostics::Route::PublicForward);
        originalEquipObject(manager, actor, object, extra, count, slot, queue, force, sounds, immediate);
    }

    bool ReadIndirectTarget(std::uintptr_t address, std::uintptr_t& target) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) ||
            region.State != MEM_COMMIT) {
            return false;
        }
        switch (region.Protect) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            break;
        default:
            return false;
        }
        const auto offset = address - reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        if (region.RegionSize < sizeof(target) || offset > region.RegionSize - sizeof(target)) {
            return false;
        }
        std::memcpy(&target, reinterpret_cast<const void*>(address), sizeof(target));
        return target != 0;
    }

    bool IsExecutableTarget(std::uintptr_t target) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<const void*>(target), &region, sizeof(region)) ||
            region.State != MEM_COMMIT) {
            return false;
        }
        switch (region.Protect) {
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
        }
    }
}

void PotionDiagnostics::InstallEntryProbe() {
    const auto runtime = REL::Module::get().version();
    if (runtime != SKSE::RUNTIME_SSE_1_6_1170 || !VerifiedRuntime1170()) {
        Note("public-probe-unavailable", runtime.pack());
        return;
    }

    const REL::Relocation<std::uintptr_t> entry{REL::ID(38894)};
    const auto address = entry.address();
    constexpr std::array<std::uint8_t, 9> expected{
        0x48, 0x85, 0xD2, 0x0F, 0x84, 0xC3, 0x01, 0x00, 0x00
    };
    std::array<std::uint8_t, expected.size()> bytes{};
    std::memcpy(bytes.data(), reinterpret_cast<const void*>(address), bytes.size());
    std::uint64_t firstBytes{};
    std::memcpy(&firstBytes, bytes.data(), sizeof(firstBytes));

    constexpr std::size_t nearJumpSize = 5;
    constexpr std::size_t indirectJumpSize = 6;
    constexpr std::uint8_t nearJumpOpcode = 0xE9;
    constexpr std::uint8_t indirectJumpOpcode = 0xFF;
    constexpr std::uint8_t ripRelativeJump = 0x25;
    std::size_t patchSize{};
    std::uintptr_t original{};
    if (bytes == expected) {
        patchSize = expected.size();
    } else if (bytes[0] == nearJumpOpcode) {
        std::int32_t displacement{};
        std::memcpy(&displacement, bytes.data() + sizeof(nearJumpOpcode), sizeof(displacement));
        original = address + nearJumpSize + displacement;
        patchSize = nearJumpSize;
    } else if (bytes[0] == indirectJumpOpcode && bytes[1] == ripRelativeJump) {
        std::int32_t displacement{};
        std::memcpy(&displacement, bytes.data() + sizeof(indirectJumpOpcode) + sizeof(ripRelativeJump),
                    sizeof(displacement));
        if (ReadIndirectTarget(address + indirectJumpSize + displacement, original)) {
            patchSize = indirectJumpSize;
        }
    }
    if (!patchSize || (patchSize != expected.size() && !original)) {
        Note("public-probe-skipped-unknown-entry", firstBytes, bytes.back(), address);
        return;
    }
    if (patchSize != expected.size() && !IsExecutableTarget(original)) {
        Note("public-probe-skipped-nonexecutable-target", firstBytes, original, address);
        return;
    }

    // Preserve the original null-actor return without relocating its relative branch.
    // The relay is a leaf: test rdx,rdx; jnz +1; ret; jmp [rip+0]; original+9.
    std::array<std::uint8_t, 20> relay{
        0x48, 0x85, 0xD2, 0x75, 0x01, 0xC3, 0xFF, 0x25, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    constexpr auto branchStubSize = indirectJumpSize + sizeof(std::uintptr_t);
    entryTrampoline.create(relay.size() + branchStubSize, reinterpret_cast<void*>(address));
    if (patchSize == expected.size()) {
        const auto continuation = address + expected.size();
        std::memcpy(relay.data() + relay.size() - sizeof(continuation), &continuation, sizeof(continuation));
        auto* code = entryTrampoline.allocate(relay.size());
        std::memcpy(code, relay.data(), relay.size());
        FlushInstructionCache(GetCurrentProcess(), code, relay.size());
        original = reinterpret_cast<std::uintptr_t>(code);
    }
    originalEquipObject = original;
    entryTrampoline.write_branch<nearJumpSize>(address, TraceEquipObject);
    if (patchSize > nearJumpSize) {
        REL::safe_fill(address + nearJumpSize, REL::NOP, patchSize - nearJumpSize);
    }
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(address), patchSize);
    Note("public-probe-installed", firstBytes, original, address);
}
#endif
