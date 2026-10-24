#pragma once
#include <algorithm>
#include <cmath>

namespace WallSampleSpan {
// Tangential offsets at a cell's native UV endpoints and midpoint. Conflicting
// fragments leave that anchor unavailable until the wall geometry is replaced.
struct Columns {
    float offset[3]{};
    unsigned char mask = 0, rejected = 0;
};
inline bool NoteColumn(Columns& c, int anchor, float offset, float tolerance) {
    if (anchor < 0 || anchor > 2 || !std::isfinite(offset) || !std::isfinite(tolerance) || tolerance < 0) return false;
    const unsigned char bit = static_cast<unsigned char>(1u << anchor);
    if (c.rejected & bit) return false;
    if (c.mask & bit) {
        if (std::fabs(c.offset[anchor] - offset) <= tolerance) return false;
        c.mask &= static_cast<unsigned char>(~bit);
        c.rejected |= bit;
        return true;
    }
    c.offset[anchor] = offset;
    c.mask |= bit;
    return true;
}
inline bool ColumnOffset(const Columns& c, float u, float& offset) {
    if (!std::isfinite(u) || u < 0 || u > 1) return false;
    const int closestAnchor = static_cast<int>(std::lround(u * 2));
    if (std::fabs(u * 2 - closestAnchor) < 1e-5f && (c.mask & (1u << closestAnchor))) {
        offset = c.offset[closestAnchor];
        return true;
    }
    const int a = u < 0.5f ? 0 : 1;
    if ((c.mask & (3u << a)) != (3u << a)) return false;
    const float t = u * 2 - a;
    offset = c.offset[a] + (c.offset[a + 1] - c.offset[a]) * t;
    return true;
}
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
