// Real-time GI update scheduler (see gi_sched.h). Part of Apex Radiance. Credits: @loinyx

#include "gi_sched.h"
#include <algorithm>
#include <cmath>
#include <tuple>

namespace GiSched {

Box Around(const float pos[3], int story, float r) {
    return {static_cast<int>(std::floor(pos[0] - r)), static_cast<int>(std::floor(pos[2] - r)), static_cast<int>(std::floor(pos[0] + r)),
            static_cast<int>(std::floor(pos[2] + r)), story, story};
}

Box Scheduler::Shown(const Lamp& l) { return l.fadeT >= 0 ? l.box.Union(l.fadeBox) : l.box; }
void Scheduler::EndFade(uintptr_t id, Lamp& l) {
    IndexBox(id, Shown(l), false);
    l.box = l.fadeBox;
    l.fadeT = -1;
    IndexBox(id, l.box, true);
}

void Scheduler::Clear() {
    for (auto& [id, l] : lamps_)
        if (l.phase == Phase::InFlight && backend_) backend_->Cancel(l.ticket);
    lamps_.clear(), tiles_.clear(), compose_.clear(), composeSet_.clear();
    published_ = 0;
    buildPending_ = buildAll_ = false;
    buildBox_ = {};
}

void Scheduler::ComposeBox(const Box& b) {
    if (b.Empty()) return;
    for (int s = b.s0; s <= b.s1; s++)
        for (int z = b.z0; z <= b.z1; z++)
            for (int x = b.x0; x <= b.x1; x++) {
                const uint64_t k = TileKey(s, x, z);
                if (composeSet_.insert(k).second) compose_.push_back(k);
            }
}

void Scheduler::IndexBox(uintptr_t id, const Box& b, bool add) {
    if (b.Empty()) return;
    for (int s = b.s0; s <= b.s1; s++)
        for (int z = b.z0; z <= b.z1; z++)
            for (int x = b.x0; x <= b.x1; x++) {
                const uint64_t k = TileKey(s, x, z);
                auto& v = tiles_[k];
                const auto it = std::find(v.begin(), v.end(), id);
                if (add && it == v.end()) v.push_back(id);
                else if (!add && it != v.end()) {
                    v.erase(it);
                    if (v.empty()) tiles_.erase(k);
                }
            }
}

void Scheduler::Enqueue(Lamp& l, Cause c, uint32_t frame) {
    if (l.phase == Phase::Restart) l.phase = Phase::Idle, l.cause = std::min(l.cause, c), c = l.cause;
    if (l.phase == Phase::Queued || l.phase == Phase::Parked || l.phase == Phase::WaitStill) { // (a moving lamp keeps waiting)
        l.cause = std::min(l.cause, c); // keep its place in age, take the higher cause
        return;
    }
    l.phase = Phase::Queued, l.cause = c, l.queuedFrame = frame;
}

void Scheduler::Dirty(Lamp& l, Cause c, uint32_t frame) {
    if (l.phase == Phase::Idle) l.dirtyFrame = frame;
    if (l.phase == Phase::InFlight) { // its bake is stale now: cancel it, it queues again once
        if (backend_) backend_->Cancel(l.ticket);
        l.phase = Phase::Restart;
        ++c_.restarts;
    }
    if (c == Cause::Moved || c == Cause::Added) { // waits for kStillFrames still frames, then queues
        l.cause = l.phase == Phase::Idle ? c : std::min(l.cause, c);
        l.phase = Phase::WaitStill;
        l.movedFrame = frame;
        return;
    }
    Enqueue(l, c, frame);
}

void Scheduler::Drop(uintptr_t id) {
    const auto it = lamps_.find(id);
    if (it == lamps_.end()) return;
    Lamp& l = it->second;
    if (l.phase == Phase::InFlight && backend_) backend_->Cancel(l.ticket);
    const Box shown = Shown(l);
    IndexBox(id, shown, false);
    ComposeBox(shown);
    lamps_.erase(it);
    ++c_.removed;
}

void Scheduler::Load(const std::vector<LampIn>& lamps, uint32_t frame) {
    Clear();
    buildPending_ = true, buildMs_ = -1e18; // the lot's scene goes first (no edit box: it dirties nothing more)
    for (const LampIn& in : lamps) {
        Lamp& l = lamps_[in.id];
        l.in = in;
        l.dirtyFrame = frame;
        Enqueue(l, Cause::Load, frame);
    }
}

void Scheduler::OnLamp(Event e, const LampIn& in, uint32_t frame) {
    if (e == Event::Removed) return Drop(in.id);
    auto [it, fresh] = lamps_.try_emplace(in.id);
    Lamp& l = it->second;
    l.in = in;
    if (fresh || e == Event::Added) return Dirty(l, Cause::Added, frame);
    if (e == Event::Moved) return Dirty(l, Cause::Moved, frame);
    // switched, dimmed, recoloured: light is linear, the layer stays (compose only)
    ++c_.composeOnly;
    ComposeBox(Shown(l));
}

void Scheduler::OnBuildEdit(const Box& box, double nowMs) {
    buildPending_ = true;
    buildMs_ = nowMs;
    if (box.Empty()) buildAll_ = true;
    else buildBox_ = buildBox_.Union(box);
}

const std::vector<uintptr_t>* Scheduler::TileLamps(uint64_t tile) const {
    const auto it = tiles_.find(tile);
    return it == tiles_.end() ? nullptr : &it->second;
}
float Scheduler::Blend(uintptr_t id) const {
    const auto it = lamps_.find(id);
    return it == lamps_.end() || it->second.fadeT < 0 ? 1.0f : static_cast<float>(it->second.fadeT);
}
bool Scheduler::HasLayer(uintptr_t id) const {
    const auto it = lamps_.find(id);
    return it != lamps_.end() && it->second.hasLayer;
}
Box Scheduler::LayerBox(uintptr_t id) const {
    const auto it = lamps_.find(id);
    return it == lamps_.end() ? Box{} : it->second.box;
}
bool Scheduler::Queued(uintptr_t id) const {
    const auto it = lamps_.find(id);
    return it != lamps_.end() && (it->second.phase == Phase::Queued || it->second.phase == Phase::Parked || it->second.phase == Phase::WaitStill);
}
bool Scheduler::InFlight(uintptr_t id) const {
    const auto it = lamps_.find(id);
    return it != lamps_.end() && it->second.phase == Phase::InFlight;
}

namespace {
bool InView(const View& v, const float p[3]) {
    if (!v.hasVp) return false;
    float c[4];
    for (int k = 0; k < 4; k++) c[k] = p[0] * v.vp[0][k] + p[1] * v.vp[1][k] + p[2] * v.vp[2][k] + v.vp[3][k];
    return c[3] > 0 && std::fabs(c[0]) <= c[3] && std::fabs(c[1]) <= c[3] && c[2] >= 0 && c[2] <= c[3];
}
} // namespace

void Scheduler::Tick(uint32_t frame, double nowMs, const View& view) {
    // last frame's tiles out; the ones events added since stay
    compose_.erase(compose_.begin(), compose_.begin() + static_cast<ptrdiff_t>(std::min(published_, compose_.size())));
    composeSet_.clear();
    composeSet_.insert(compose_.begin(), compose_.end());
    const double dt = lastMs_ ? std::max(0.0, nowMs - lastMs_) : 0.0;
    lastMs_ = nowMs;
    if (backend_) backend_->BeginFrame(frame);

    // crossfades: a landed layer fades in over kFadeMs, its tiles composed every frame meanwhile
    for (auto& [id, l] : lamps_) {
        if (l.fadeT < 0) continue;
        ComposeBox(Shown(l));
        l.fadeT += dt / kFadeMs;
        if (l.fadeT >= 1.0) EndFade(id, l);
    }

    // build edits: once they stopped for kBuildIdleMs, the scene first, then the lamps whose touch box holds the edit
    if (buildPending_ && nowMs - buildMs_ >= kBuildIdleMs) {
        Box edit = buildBox_;
        bool all = buildAll_;
        if (!backend_ || backend_->SubmitScene(edit, all)) {
            ++c_.sceneSubmits;
            for (auto& [id, l] : lamps_) {
                const Box touch = Shown(l);
                // shortcut: a lamp with no layer yet has no box and is already queued or waiting; it is left as it is
                if (all ? l.hasLayer || l.phase == Phase::InFlight : touch.Overlaps(edit)) Dirty(l, Cause::Build, frame);
            }
            buildPending_ = buildAll_ = false;
            buildBox_ = {};
        }
    }

    // still lamps queue
    for (auto& [id, l] : lamps_)
        if (l.phase == Phase::WaitStill && frame - l.movedFrame >= static_cast<uint32_t>(kStillFrames)) {
            l.phase = Phase::Idle;
            Enqueue(l, l.cause, frame);
        }

    // poll the bakes in flight
    int inFlight = 0;
    if (backend_)
        for (auto& [id, l] : lamps_) {
            if (l.phase != Phase::InFlight) continue;
            Box touched;
            const int r = backend_->Poll(l.ticket, touched);
            if (r == 0) {
                ++inFlight;
                continue;
            }
            if (r < 0) { // failed: waits in the idle list, not retried every frame
                ++c_.failed;
                l.phase = Phase::Parked;
                continue;
            }
            ++c_.landed;
            const uint32_t lat = frame - l.dirtyFrame;
            if (latencies_.size() < 256) latencies_.push_back(lat);
            else latencies_[latencyNext_++ % 256] = lat;
            l.phase = Phase::Idle;
            // the new layer goes to the second slot; both are in the tile lists while it fades in. A layer still fading
            // in is taken as shown first (a layer is replaced whole)
            if (l.fadeT >= 0) EndFade(id, l);
            IndexBox(id, l.box, false);
            l.fadeBox = touched;
            l.fadeT = 0;
            IndexBox(id, Shown(l), true);
            ComposeBox(Shown(l));
            l.hasLayer = true;
        }

    // the queue: the cap first (the lowest entries wait in the idle list), then issue in priority order
    std::vector<std::pair<std::tuple<int, int, int, float, int, uint32_t>, Lamp*>> q;
    int parked = 0;
    for (auto& [id, l] : lamps_) {
        if (l.phase == Phase::Parked) ++parked;
        if (l.phase != Phase::Queued) continue;
        const uint32_t age = frame - l.queuedFrame;
        float d = 0;
        if (view.known) {
            const float dx = l.in.pos[0] - view.eye[0], dy = l.in.pos[1] - view.eye[1], dz = l.in.pos[2] - view.eye[2];
            d = std::floor(std::sqrt(dx * dx + dy * dy + dz * dz) / 2.0f); // 2 m steps, so cause and age still order nearby lamps
        }
        q.push_back({{age >= kStarveFrames ? 0 : 1, l.in.on ? 0 : 1, InView(view, l.in.pos) ? 0 : 1, d, static_cast<int>(l.cause),
                      ~age},
                     &l});
    }
    std::sort(q.begin(), q.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = kQueueCap; i < q.size(); i++) {
        q[i].second->phase = Phase::Parked;
        ++c_.dropped, ++parked;
    }
    if (q.size() > static_cast<size_t>(kQueueCap)) q.resize(kQueueCap);
    // the idle list refills the queue once it and the bakes in flight are done (and no build edit is pending)
    if (q.empty() && inFlight == 0 && parked && !buildPending_)
        for (auto& [id, l] : lamps_)
            if (l.phase == Phase::Parked) l.phase = Phase::Queued, l.queuedFrame = frame;

    int issued = 0;
    if (backend_ && !buildPending_) // nothing is issued while build edits go on (the scene is about to change)
        for (auto& [key, l] : q) {
            if (issued >= budget_.issuePerFrame || inFlight >= budget_.maxInFlight) break;
            if (l->fadeT >= 0) continue; // (its two slots are both shown until the fade ends)
            uint32_t ticket = 0;
            if (!backend_->Solve(l->in, ticket)) break;
            l->phase = Phase::InFlight, l->ticket = ticket;
            ++c_.issued[static_cast<int>(l->cause)];
            ++issued, ++inFlight;
        }
    if (issued > budget_.issuePerFrame || inFlight > budget_.maxInFlight) ++c_.overBudget;
    c_.maxInFlight = std::max(c_.maxInFlight, inFlight);
    c_.tilesComposed += compose_.size();
    published_ = compose_.size();
}

Counters Scheduler::Stats() const {
    Counters c = c_;
    c.queued = c.inFlight = c.idle = c.fading = 0;
    for (const auto& [id, l] : lamps_) {
        c.queued += l.phase == Phase::Queued || l.phase == Phase::WaitStill;
        c.inFlight += l.phase == Phase::InFlight;
        c.idle += l.phase == Phase::Parked;
        c.fading += l.fadeT >= 0;
    }
    if (!latencies_.empty()) {
        std::vector<uint32_t> s = latencies_;
        std::sort(s.begin(), s.end());
        c.latencyP50 = s[s.size() / 2], c.latencyMax = s.back();
    }
    return c;
}

} // namespace GiSched
