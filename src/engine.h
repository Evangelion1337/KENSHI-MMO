#pragma once

#include <cstdint>

namespace kmmo {
namespace engine {

bool Init();
bool Ready();

// The real GameWorld object (revealed by KenshiLib's `ou` global), or 0.
uintptr_t GetGameWorld();

// Raw field access against the KenshiLib-anchored GameWorld. KenshiLib's
// offsets (frameSpeedMult @ +0x700, paused @ +0x8B9) are validated against
// this exact object by the speed lock before any write.
bool ReadSpeed(uintptr_t gw, float& mult, bool& paused);
bool WriteSpeed(uintptr_t gw, float mult, bool paused);

} // namespace engine
} // namespace kmmo