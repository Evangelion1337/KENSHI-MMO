#pragma once

namespace kmmo {
namespace speedlock {

// Start the speed lock (idempotent). Calibrates the game-speed variable
// automatically (injects the speed hotkeys, finds which memory slot holds the
// x1/x3/x5 value) and then forces it to x1 permanently.
void Begin();

} // namespace speedlock
} // namespace kmmo