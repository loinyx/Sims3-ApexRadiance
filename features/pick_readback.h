#pragma once
// The game reads its mouse pick target back from the GPU every other frame (GetRenderTargetData), and the copy waited
// for the GPU to finish the frame. The read-back now waits a frame or two until an event query says the GPU is past
// the pick's draws, so the copy no longer stalls the render thread. Steam 1.67.2 only; always on.
#include <string>
namespace PickReadback {
void Install();          // after the features start (the device hooks come later; the first Present gives the device)
std::string StatsText(); // developer log
}
