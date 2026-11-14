// Menu icons (see icons.h): Lucide SVG elements -> cached polylines -> ImDrawList strokes.
#include "icons.h"
#include "lucide_data.h"
#include "apex_log.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace ApexUi {
namespace {

static_assert(sizeof(LucideData::kIcons) / sizeof(LucideData::kIcons[0]) == static_cast<size_t>(IconId::Count),
              "ui/lucide_data.h kIcons and ApexUi::IconId must list the same icons in the same order");

constexpr int kIconCount = static_cast<int>(IconId::Count);
constexpr float kViewBox = 24.0f;    // every Lucide icon
constexpr float kStrokeWidth = 2.0f; // Lucide's stroke-width, in viewBox units
constexpr float kFlattenPx = 4.0f;   // curves are flattened for up to 4 px per unit (96 px icons)
constexpr float kPi = 3.14159265358979f;

struct Pt {
    float x, y;
};
struct Stroke {
    std::vector<Pt> pts;
    bool closed = false;
};

// ---------------------------------------------------------------------------------------------------------------------
// SVG path data -> polylines (viewBox units)
// ---------------------------------------------------------------------------------------------------------------------
class PathParser {
  public:
    explicit PathParser(const char* d) : s_(d) {}

    // False on malformed data (what was read so far is kept)
    bool Parse(std::vector<Stroke>& out) {
        out_ = &out;
        char cmd = 0;
        while (true) {
            SkipSeparators();
            if (!*s_) return true;
            const char c = *s_;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                cmd = c;
                s_++;
            } else if (!cmd) {
                return false; // coordinates before the first command
            }
            // else: the previous command repeats with a new set of coordinates
            if (!Command(cmd)) return false;
            if (cmd == 'M') cmd = 'L'; // coordinates after a moveto are implicit linetos
            else if (cmd == 'm') cmd = 'l';
        }
    }

  private:
    const char* s_;
    std::vector<Stroke>* out_ = nullptr;
    Pt cur_{0.0f, 0.0f}, start_{0.0f, 0.0f};
    Pt lastCubicCtrl_{0.0f, 0.0f}, lastQuadCtrl_{0.0f, 0.0f};
    char prev_ = 0; // upper-case letter of the previous command (S and T reflect the previous control point)
    bool open_ = false;

    void SkipSeparators() {
        while (*s_ == ' ' || *s_ == ',' || *s_ == '\t' || *s_ == '\n' || *s_ == '\r') s_++;
    }

    // Locale-independent number: [sign] digits [. digits] [e [sign] digits]. ".5.5" reads as .5 then .5.
    bool Number(float& out) {
        SkipSeparators();
        const char* p = s_;
        double sign = 1.0;
        if (*p == '+' || *p == '-') {
            if (*p == '-') sign = -1.0;
            p++;
        }
        double value = 0.0;
        bool digits = false;
        while (*p >= '0' && *p <= '9') {
            value = value * 10.0 + (*p - '0');
            p++;
            digits = true;
        }
        if (*p == '.') {
            p++;
            double scale = 0.1;
            while (*p >= '0' && *p <= '9') {
                value += (*p - '0') * scale;
                scale *= 0.1;
                p++;
                digits = true;
            }
        }
        if (!digits) return false;
        if (*p == 'e' || *p == 'E') {
            const char* q = p + 1;
            int expSign = 1;
            if (*q == '+' || *q == '-') {
                if (*q == '-') expSign = -1;
                q++;
            }
            if (*q >= '0' && *q <= '9') {
                int e = 0;
                while (*q >= '0' && *q <= '9') {
                    e = e * 10 + (*q - '0');
                    q++;
                }
                value *= std::pow(10.0, expSign * e);
                p = q;
            }
        }
        out = static_cast<float>(sign * value);
        s_ = p;
        return true;
    }

    // Arc flags are single characters ("0" / "1") and may be written without separators
    bool Flag(bool& out) {
        SkipSeparators();
        if (*s_ != '0' && *s_ != '1') return false;
        out = *s_ == '1';
        s_++;
        return true;
    }

    void Begin() {
        if (open_) return;
        out_->emplace_back();
        out_->back().pts.push_back(cur_);
        open_ = true;
    }

    void LineTo(Pt p) {
        Begin();
        out_->back().pts.push_back(p);
        cur_ = p;
    }

    static float Dist(Pt a, Pt b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

    void CubicTo(Pt c1, Pt c2, Pt end) {
        const Pt p0 = cur_;
        const float len = (Dist(p0, c1) + Dist(c1, c2) + Dist(c2, end)) * kFlattenPx;
        const int n = std::clamp(static_cast<int>(std::ceil(len / 2.0f)), 2, 48);
        for (int i = 1; i <= n; i++) {
            const float t = static_cast<float>(i) / static_cast<float>(n), u = 1.0f - t;
            const float a = u * u * u, b = 3.0f * u * u * t, c = 3.0f * u * t * t, d = t * t * t;
            LineTo(i == n ? end : Pt{a * p0.x + b * c1.x + c * c2.x + d * end.x, a * p0.y + b * c1.y + c * c2.y + d * end.y});
        }
    }

    void QuadTo(Pt q, Pt end) {
        const Pt p0 = cur_;
        const Pt c1{p0.x + (q.x - p0.x) * (2.0f / 3.0f), p0.y + (q.y - p0.y) * (2.0f / 3.0f)};
        const Pt c2{end.x + (q.x - end.x) * (2.0f / 3.0f), end.y + (q.y - end.y) * (2.0f / 3.0f)};
        CubicTo(c1, c2, end);
    }

    // SVG elliptical arc (endpoint parameterisation, SVG 1.1 appendix F.6.5), flattened directly
    void ArcTo(float rx, float ry, float rotationDeg, bool largeArc, bool sweep, Pt end) {
        const Pt p1 = cur_;
        if (p1.x == end.x && p1.y == end.y) return;
        rx = std::fabs(rx);
        ry = std::fabs(ry);
        if (rx < 1e-6f || ry < 1e-6f) {
            LineTo(end);
            return;
        }
        const float phi = rotationDeg * kPi / 180.0f;
        const float cs = std::cos(phi), sn = std::sin(phi);
        const float dx = (p1.x - end.x) * 0.5f, dy = (p1.y - end.y) * 0.5f;
        const float x1p = cs * dx + sn * dy;
        const float y1p = -sn * dx + cs * dy;
        const float lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
        if (lambda > 1.0f) { // radii too small for the endpoints: scaled up (SVG rule)
            const float k = std::sqrt(lambda);
            rx *= k;
            ry *= k;
        }
        const float rx2 = rx * rx, ry2 = ry * ry;
        const float num = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
        const float den = rx2 * y1p * y1p + ry2 * x1p * x1p;
        float coef = den > 0.0f ? std::sqrt(std::max(0.0f, num / den)) : 0.0f;
        if (largeArc == sweep) coef = -coef;
        const float cxp = coef * rx * y1p / ry;
        const float cyp = -coef * ry * x1p / rx;
        const float cx = cs * cxp - sn * cyp + (p1.x + end.x) * 0.5f;
        const float cy = sn * cxp + cs * cyp + (p1.y + end.y) * 0.5f;
        auto angle = [](float ux, float uy, float vx, float vy) { return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy); };
        const float ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
        const float vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
        const float theta1 = angle(1.0f, 0.0f, ux, uy);
        float dtheta = angle(ux, uy, vx, vy);
        if (!sweep && dtheta > 0.0f) dtheta -= 2.0f * kPi;
        else if (sweep && dtheta < 0.0f) dtheta += 2.0f * kPi;
        const float arcPx = std::fabs(dtheta) * std::max(rx, ry) * kFlattenPx;
        const int n = std::clamp(static_cast<int>(std::ceil(arcPx / 2.0f)), 2, 64);
        for (int i = 1; i <= n; i++) {
            if (i == n) {
                LineTo(end);
                break;
            }
            const float th = theta1 + dtheta * static_cast<float>(i) / static_cast<float>(n);
            const float ex = rx * std::cos(th), ey = ry * std::sin(th);
            LineTo(Pt{cx + cs * ex - sn * ey, cy + sn * ex + cs * ey});
        }
    }

    bool Command(char cmd) {
        const bool rel = cmd >= 'a' && cmd <= 'z';
        const char up = rel ? static_cast<char>(cmd - 'a' + 'A') : cmd;
        const Pt base = rel ? cur_ : Pt{0.0f, 0.0f};
        float a[6];
        switch (up) {
        case 'M':
            if (!Number(a[0]) || !Number(a[1])) return false;
            cur_ = Pt{base.x + a[0], base.y + a[1]};
            start_ = cur_;
            open_ = false;
            Begin();
            break;
        case 'L':
            if (!Number(a[0]) || !Number(a[1])) return false;
            LineTo(Pt{base.x + a[0], base.y + a[1]});
            break;
        case 'H':
            if (!Number(a[0])) return false;
            LineTo(Pt{rel ? cur_.x + a[0] : a[0], cur_.y});
            break;
        case 'V':
            if (!Number(a[0])) return false;
            LineTo(Pt{cur_.x, rel ? cur_.y + a[0] : a[0]});
            break;
        case 'C': {
            for (int i = 0; i < 6; i++)
                if (!Number(a[i])) return false;
            const Pt c1{base.x + a[0], base.y + a[1]}, c2{base.x + a[2], base.y + a[3]}, e{base.x + a[4], base.y + a[5]};
            CubicTo(c1, c2, e);
            lastCubicCtrl_ = c2;
            break;
        }
        case 'S': {
            for (int i = 0; i < 4; i++)
                if (!Number(a[i])) return false;
            const Pt c1 = (prev_ == 'C' || prev_ == 'S') ? Pt{2.0f * cur_.x - lastCubicCtrl_.x, 2.0f * cur_.y - lastCubicCtrl_.y} : cur_;
            const Pt c2{base.x + a[0], base.y + a[1]}, e{base.x + a[2], base.y + a[3]};
            CubicTo(c1, c2, e);
            lastCubicCtrl_ = c2;
            break;
        }
        case 'Q': {
            for (int i = 0; i < 4; i++)
                if (!Number(a[i])) return false;
            const Pt q{base.x + a[0], base.y + a[1]}, e{base.x + a[2], base.y + a[3]};
            QuadTo(q, e);
            lastQuadCtrl_ = q;
            break;
        }
        case 'T': {
            if (!Number(a[0]) || !Number(a[1])) return false;
            const Pt q = (prev_ == 'Q' || prev_ == 'T') ? Pt{2.0f * cur_.x - lastQuadCtrl_.x, 2.0f * cur_.y - lastQuadCtrl_.y} : cur_;
            QuadTo(q, Pt{base.x + a[0], base.y + a[1]});
            lastQuadCtrl_ = q;
            break;
        }
        case 'A': {
            bool large = false, sweep = false;
            float rx = 0.0f, ry = 0.0f, rot = 0.0f, x = 0.0f, y = 0.0f;
            if (!Number(rx) || !Number(ry) || !Number(rot) || !Flag(large) || !Flag(sweep) || !Number(x) || !Number(y)) return false;
            ArcTo(rx, ry, rot, large, sweep, Pt{base.x + x, base.y + y});
            break;
        }
        case 'Z':
            if (open_) out_->back().closed = true;
            cur_ = start_; // the next command starts from the subpath's start
            open_ = false;
            break;
        default:
            return false;
        }
        prev_ = up;
        return true;
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Cache (viewBox units)
// ---------------------------------------------------------------------------------------------------------------------
struct Circle {
    float cx, cy, r;
    bool filled;
};
struct Rect {
    float x, y, w, h, rx;
};
struct CachedIcon {
    bool built = false;
    std::vector<Stroke> strokes; // paths and lines
    std::vector<Circle> circles;
    std::vector<Rect> rects;
};
CachedIcon g_cache[kIconCount];

// Drops repeated points (a zero-length segment gives ImGui's thick lines a bad normal) and, for a closed stroke, a last
// point that repeats the first.
void Clean(Stroke& s) {
    std::vector<Pt> out;
    out.reserve(s.pts.size());
    for (const Pt& p : s.pts)
        if (out.empty() || std::fabs(p.x - out.back().x) > 1e-4f || std::fabs(p.y - out.back().y) > 1e-4f) out.push_back(p);
    if (s.closed && out.size() > 2 && std::fabs(out.front().x - out.back().x) < 1e-4f && std::fabs(out.front().y - out.back().y) < 1e-4f) out.pop_back();
    s.pts.swap(out);
}

const CachedIcon& Get(int i) {
    CachedIcon& c = g_cache[i];
    if (c.built) return c;
    c.built = true;
    const LucideData::IconData& data = LucideData::kIcons[i];
    for (int k = 0; k < data.count; k++) {
        const LucideData::Element& e = data.elements[k];
        switch (e.kind) {
        case LucideData::Kind::Path: {
            std::vector<Stroke> strokes;
            PathParser parser(e.d);
            if (!parser.Parse(strokes)) LOG_WARNING(std::format("[Menu] Icon {}: element {} could not be read completely", data.name, k));
            for (Stroke& s : strokes) {
                Clean(s);
                if (!s.pts.empty()) c.strokes.push_back(std::move(s));
            }
            break;
        }
        case LucideData::Kind::Circle: c.circles.push_back(Circle{e.v[0], e.v[1], e.v[2], e.filled}); break;
        case LucideData::Kind::Rect: c.rects.push_back(Rect{e.v[0], e.v[1], e.v[2], e.v[3], e.v[4]}); break;
        case LucideData::Kind::Line: {
            Stroke s;
            s.pts = {Pt{e.v[0], e.v[1]}, Pt{e.v[2], e.v[3]}};
            Clean(s);
            c.strokes.push_back(std::move(s));
            break;
        }
        }
    }
    return c;
}

// Length of a stroke in viewBox units
float Length(const Stroke& s) {
    float len = 0.0f;
    for (size_t i = 1; i < s.pts.size(); i++) len += std::sqrt((s.pts[i].x - s.pts[i - 1].x) * (s.pts[i].x - s.pts[i - 1].x) + (s.pts[i].y - s.pts[i - 1].y) * (s.pts[i].y - s.pts[i - 1].y));
    return len;
}

// A round join is needed where the direction turns by more than ~25 degrees (flattened curves turn by less)
bool SharpCorner(Pt a, Pt b, Pt c) {
    const float ux = b.x - a.x, uy = b.y - a.y, vx = c.x - b.x, vy = c.y - b.y;
    const float lu = std::sqrt(ux * ux + uy * uy), lv = std::sqrt(vx * vx + vy * vy);
    if (lu < 1e-6f || lv < 1e-6f) return false;
    return (ux * vx + uy * vy) / (lu * lv) < 0.9f;
}

} // namespace

void DrawIcon(ImDrawList* dl, IconId id, ImVec2 pos, float sizePx, ImU32 color) {
    const int i = static_cast<int>(id);
    if (!dl || i < 0 || i >= kIconCount || sizePx <= 0.0f || (color & IM_COL32_A_MASK) == 0) return;
    const CachedIcon& icon = Get(i);
    const float s = sizePx / kViewBox;
    const float th = std::fmax(1.0f, kStrokeWidth * s); // never thinner than a pixel
    const float r = th * 0.5f;
    auto P = [&](Pt p) { return ImVec2(pos.x + p.x * s, pos.y + p.y * s); };

    static std::vector<ImVec2> pts; // render thread only
    for (const Stroke& st : icon.strokes) {
        const size_t n = st.pts.size();
        if (n == 1 || Length(st) < 0.1f) { // "h.01" style dots: a round cap alone
            dl->AddCircleFilled(P(st.pts.front()), r, color);
            continue;
        }
        pts.resize(n);
        for (size_t k = 0; k < n; k++) pts[k] = P(st.pts[k]);
        dl->AddPolyline(pts.data(), static_cast<int>(n), color, st.closed ? ImDrawFlags_Closed : ImDrawFlags_None, th);
        // Round caps and joins
        if (!st.closed) {
            dl->AddCircleFilled(pts.front(), r, color);
            dl->AddCircleFilled(pts.back(), r, color);
        }
        for (size_t k = 0; k < n; k++) {
            if (!st.closed && (k == 0 || k == n - 1)) continue;
            const Pt a = st.pts[(k + n - 1) % n], b = st.pts[k], c = st.pts[(k + 1) % n];
            if (SharpCorner(a, b, c)) dl->AddCircleFilled(pts[k], r, color);
        }
    }
    for (const Circle& c : icon.circles) {
        // A filled circle keeps its stroke too (Lucide's palette dots): radius + half the stroke
        if (c.filled) dl->AddCircleFilled(P(Pt{c.cx, c.cy}), c.r * s + r, color);
        else if (c.r * s <= th) dl->AddCircleFilled(P(Pt{c.cx, c.cy}), c.r * s + 0.5f * th, color); // a dot (the ellipsis): the stroke fills it
        else dl->AddCircle(P(Pt{c.cx, c.cy}), c.r * s, color, 0, th);
    }
    for (const Rect& rc : icon.rects) dl->AddRect(P(Pt{rc.x, rc.y}), P(Pt{rc.x + rc.w, rc.y + rc.h}), color, rc.rx * s, ImDrawFlags_None, th);
}

void Icon(IconId id, ImVec2 pos, float sizePx, ImU32 color) { DrawIcon(ImGui::GetWindowDrawList(), id, pos, sizePx, color); }

void InlineIcon(IconId id, float sizePx, ImU32 color) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = std::fmax(sizePx, ImGui::GetTextLineHeight());
    ImGui::Dummy(ImVec2(sizePx, h));
    DrawIcon(ImGui::GetWindowDrawList(), id, ImVec2(p.x, p.y + (h - sizePx) * 0.5f), sizePx, color);
}

void IconLabel(IconId id, const char* label, ImU32 iconColor) {
    const float size = ImGui::GetTextLineHeight();
    InlineIcon(id, size, iconColor);
    ImGui::SameLine(0.0f, size * 0.4f);
    ImGui::TextUnformatted(label);
}

} // namespace ApexUi

namespace ApexUi {
const char* IconName(IconId id) {
    const int index = static_cast<int>(id);
    return index >= 0 && index < static_cast<int>(IconId::Count) ? LucideData::kIcons[index].name : "bookmark";
}
IconId IconFromName(const std::string_view name) {
    for (int i = 0; i < static_cast<int>(IconId::Count); ++i)
        if (name == LucideData::kIcons[i].name) return static_cast<IconId>(i);
    return IconId::Bookmark;
}
} // namespace ApexUi
