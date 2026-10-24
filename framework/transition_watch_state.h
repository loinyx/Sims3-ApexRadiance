#pragma once
#include <cstdint>

namespace TransitionWatch {
enum class Event { None, Begin, Waiting, Resumed, Interrupted };
// A missing scene is an observation, not proof of an Edit Town dialog or its cause.
struct Tracker {
    uint64_t world = 0, missingAt = 0, drawnAt = 0, reportAt = 0;
    bool armed = false, missing = false, tracking = false, drawing = false;
    Event Update(uint64_t now, uint64_t identity, bool active, bool counting, int draws) {
        if (!active || !counting || !identity || (world && world != identity)) {
            const bool interrupted = tracking;
            *this = {};
            world = identity;
            return interrupted ? Event::Interrupted : Event::None;
        }
        world = identity;
        if (draws > 0) {
            if (!drawing) { drawing = true; drawnAt = now; }
            if (now - drawnAt >= 1000) {
                armed = true;
                if (tracking) {
                    tracking = missing = false;
                    return Event::Resumed;
                }
                missing = false;
            } else if (!tracking) missing = false;
        } else {
            drawing = false;
            if (armed && !missing) { missing = true; missingAt = now; }
            if (missing && !tracking && now - missingAt >= 1000) {
                tracking = true;
                reportAt = now;
                return Event::Begin;
            }
        }
        if (tracking && now - reportAt >= 10000) {
            reportAt = now;
            return Event::Waiting;
        }
        return Event::None;
    }
};
}
