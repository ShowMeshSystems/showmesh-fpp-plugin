#pragma once

#include <cstdint>

namespace showmesh {

// Milliseconds since the Unix epoch. Time is always passed in rather than
// read from a clock so the engine is host-neutral and every fade test is
// deterministic.
using TimeMillis = std::int64_t;

// FadingValue is one linearly interpolated 0-100 percentage with an
// optional active fade. Both the brightness ceiling and the transition
// gain are instances of it, which is what lets them fade concurrently and
// compose per frame without either owning the other's timing.
//
// The value is held as a double so a long fade advances smoothly between
// integer percentages; callers round only where a percentage is applied.
class FadingValue {
 public:
    FadingValue() = default;
    explicit FadingValue(double value) : start_(value), target_(value) {}

    // Sets a new target. A zero or negative duration applies immediately.
    // A fade started while another is running begins at the currently
    // interpolated value, never at the previous start or target, so
    // replacing a fade introduces no discontinuity.
    void fadeTo(double target, std::int64_t durationMillis, TimeMillis now) {
        const double from = valueAt(now);
        if (durationMillis <= 0) {
            start_ = target;
            target_ = target;
            startMillis_ = 0;
            endMillis_ = 0;
            return;
        }
        start_ = from;
        target_ = target;
        startMillis_ = now;
        endMillis_ = now + durationMillis;
    }

    double valueAt(TimeMillis now) const {
        if (!fading()) {
            return target_;
        }
        if (now <= startMillis_) {
            return start_;
        }
        if (now >= endMillis_) {
            return target_;
        }
        const double elapsed = static_cast<double>(now - startMillis_);
        const double span = static_cast<double>(endMillis_ - startMillis_);
        return start_ + (target_ - start_) * (elapsed / span);
    }

    // settle drops any active fade and holds v. Used by restart recovery,
    // which must never resume timing it cannot trust.
    void settle(double v) {
        start_ = v;
        target_ = v;
        startMillis_ = 0;
        endMillis_ = 0;
    }

    bool fading() const { return endMillis_ > startMillis_; }
    bool fadingAt(TimeMillis now) const { return fading() && now < endMillis_; }
    double start() const { return start_; }
    double target() const { return target_; }
    TimeMillis startMillis() const { return startMillis_; }
    TimeMillis endMillis() const { return endMillis_; }

    // Restores a fade exactly as another node or a previous process
    // recorded it. No validation happens here; callers decide whether the
    // timing is trustworthy before adopting it.
    void restore(double start, double target, TimeMillis startMillis, TimeMillis endMillis) {
        start_ = start;
        target_ = target;
        startMillis_ = startMillis;
        endMillis_ = endMillis;
    }

 private:
    double start_ = 100.0;
    double target_ = 100.0;
    TimeMillis startMillis_ = 0;
    TimeMillis endMillis_ = 0;
};

}  // namespace showmesh
