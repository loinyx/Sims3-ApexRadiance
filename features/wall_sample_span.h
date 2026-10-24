#pragma once
#include <algorithm>
#include <cmath>

namespace WallSampleSpan {
// The sample rows and the vertex UVs must use the same base. Keep the native
// three-metre span while moving its base to the measured foot. Raised full
// walls need the same shift as short gables; otherwise their shared edge reads
// light evaluated at different heights. Tall raised spans stay on their old path.
inline bool RaisedSpan(float foot, float base, float left, float middle, float right) {
    if (!std::isfinite(foot) || !std::isfinite(base) || !std::isfinite(left) || !std::isfinite(middle) || !std::isfinite(right)) return false;
    const float height = std::max({left, middle, right});
    return foot > base + 0.05f && foot < base + 3.2f &&
           std::min({left, middle, right}) > 0.2f && height <= 3.02f;
}
// Only short cells need their native UV crop reconsidered. A full-height
// wall's existing endpoint UVs already span its drawn three metres.
inline bool RaisedCell(float foot, float base, float left, float middle, float right) {
    return RaisedSpan(foot, base, left, middle, right) && std::max({left, middle, right}) < 2.98f;
}
// Accept the clipped tip only on the existing low-foot outdoor UV path.
inline bool MeasuredCell(float foot, float base, float left, float middle, float right, bool outdoor) {
    if (!std::isfinite(foot) || !std::isfinite(base) || !std::isfinite(left) || !std::isfinite(middle) || !std::isfinite(right)) return false;
    const float lo = std::min({left, middle, right}), hi = std::max({left, middle, right});
    if (hi >= 20.0f) return false;
    if (lo > 0.2f) return true;
    return outdoor && foot < base - 0.05f && foot > base - 3.2f && lo > -1.0f && hi > 0.2f && hi < 2.98f;
}
inline bool ShiftFoot(float foot, float base, float left, float middle, float right) {
    return (foot < base - 0.05f && foot > base - 3.2f) || RaisedSpan(foot, base, left, middle, right);
}
}
