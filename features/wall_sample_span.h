#pragma once
#include <algorithm>
#include <cmath>

namespace WallSampleSpan {
// The sample rows and the vertex UVs must use the same base. Keep the native
// three-metre span unless an outdoor cell needs the existing lower-foot rule,
// or a short raised cell extends beyond that span and would clamp its top UVs.
inline bool RaisedOverflow(float foot, float base, float left, float middle, float right) {
    if (!std::isfinite(foot) || !std::isfinite(base) || !std::isfinite(left) || !std::isfinite(middle) || !std::isfinite(right)) return false;
    const float height = std::max({left, middle, right});
    return foot > base + 0.05f && foot < base + 3.2f &&
           std::min({left, middle, right}) > 0.2f && height < 2.98f && foot + height > base + 3.02f;
}
inline bool ShiftFoot(float foot, float base, float left, float middle, float right) {
    return (foot < base - 0.05f && foot > base - 3.2f) || RaisedOverflow(foot, base, left, middle, right);
}
}
