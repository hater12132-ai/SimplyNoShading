#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace bactro::targethud {

void registerModule();
void onSignaturesReady();
void shutdown();

// Called from NormalTick / attack detours
void onFrame();
void onAttack(void* targetActor);

} // namespace bactro::targethud
