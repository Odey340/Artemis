#pragma once
// Artemis z-score signal generator, re-used to track the Kalshi/Polymarket
// basis. Inherit and override evaluate() to plug in a different rule.

#include <string>

#include "core/RollingStatistics.hpp"

namespace artemis {

enum class Signal { Flat = 0, Long = 1, Short = -1 };

inline const char* toString(Signal s) {
    return s == Signal::Long ? "LONG" : s == Signal::Short ? "SHORT" : "FLAT";
}

class SignalGenerator {
public:
    SignalGenerator(double entryZ, double exitZ) : entryZ_(entryZ), exitZ_(exitZ) {}
    virtual ~SignalGenerator() = default;

    // Hysteresis rule: enter when |z| >= entry, exit when |z| <= exit.
    // LONG means the series is unusually *low* (expected to rise).
    virtual Signal evaluate(double z) {
        switch (state_) {
            case Signal::Flat:
                if (z <= -entryZ_) state_ = Signal::Long;
                else if (z >= entryZ_) state_ = Signal::Short;
                break;
            case Signal::Long:
                if (z >= -exitZ_) state_ = Signal::Flat;
                break;
            case Signal::Short:
                if (z <= exitZ_) state_ = Signal::Flat;
                break;
        }
        return state_;
    }

    Signal state() const { return state_; }
    void reset() { state_ = Signal::Flat; }

protected:
    double entryZ_;
    double exitZ_;
    Signal state_ = Signal::Flat;
};

// Rolling basis (Kalshi YES mid - Polymarket YES mid, in cents) for one pair.
struct BasisMonitor {
    BasisMonitor(double halfLife, int warmup, double entryZ, double exitZ)
        : stats(halfLife, static_cast<std::uint32_t>(warmup)), signal(entryZ, exitZ) {}

    Signal update(double basisCents) {
        stats.update(basisCents);
        last = signal.evaluate(stats.lastZ());
        return last;
    }

    RollingStatistics stats;
    SignalGenerator signal;
    Signal last = Signal::Flat;
};

}  // namespace artemis
