#pragma once

// Decides when the coordinator is confirmed lost (ADR-048 decision 2). One
// failed or slow request never confirms anything: loss needs a run of failed
// probes that also spans a minimum time.

#include <cerrno>
#include <cstdlib>
#include <string>

#include "showmesh/fading_value.h"

namespace showmesh {
namespace fallback {

// HYPOTHESES, not measurements. ADR-048 requires a measured default and
// nothing has been measured on a real coordinator and network yet.
constexpr int kHypothesisProbeIntervalMillis = 5000;
constexpr int kHypothesisProbeTimeoutMillis = 3000;
constexpr int kHypothesisFailedProbesToConfirm = 3;
constexpr int kHypothesisMinimumLossMillis = 15000;

// The unauthenticated route the probe reads. Only a 2xx counts as reached.
constexpr const char* kCoordinatorHealthPath = "/healthz";

constexpr const char* kProbeIntervalSettingName = "ShowMeshCoordinatorProbeIntervalMillis";
constexpr const char* kProbeTimeoutSettingName = "ShowMeshCoordinatorProbeTimeoutMillis";
constexpr const char* kFailedProbesSettingName = "ShowMeshCoordinatorLossFailedProbes";
constexpr const char* kMinimumLossSettingName = "ShowMeshCoordinatorLossMinimumMillis";

struct OutageDetectorConfig {
    int probeIntervalMillis = kHypothesisProbeIntervalMillis;
    int probeTimeoutMillis = kHypothesisProbeTimeoutMillis;
    int failedProbesToConfirm = kHypothesisFailedProbesToConfirm;
    int minimumLossMillis = kHypothesisMinimumLossMillis;
};

// A whole number within [minimum, maximum], or false with *out untouched.
inline bool parseOutageSetting(const std::string& text, int minimum, int maximum, int* out) {
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || value < minimum || value > maximum) return false;
    *out = static_cast<int>(value);
    return true;
}

class CoordinatorOutageDetector {
 public:
    explicit CoordinatorOutageDetector(OutageDetectorConfig config = OutageDetectorConfig()) : config_(config) {}

    void recordProbe(bool reached, TimeMillis now) {
        probed_ = true;
        if (reached) {
            consecutiveFailures_ = 0;
            return;
        }
        // A clock stepped backwards restarts the span instead of making it negative.
        if (consecutiveFailures_ == 0 || now < firstFailureAtMillis_) firstFailureAtMillis_ = now;
        ++consecutiveFailures_;
        lastFailureAtMillis_ = now;
    }

    bool confirmedLost() const {
        return consecutiveFailures_ >= config_.failedProbesToConfirm &&
               lastFailureAtMillis_ - firstFailureAtMillis_ >= config_.minimumLossMillis;
    }

    // True only when the most recent probe got a 2xx.
    bool reachable() const { return probed_ && consecutiveFailures_ == 0; }
    int consecutiveFailures() const { return consecutiveFailures_; }
    TimeMillis firstFailureAtMillis() const { return consecutiveFailures_ == 0 ? 0 : firstFailureAtMillis_; }
    const OutageDetectorConfig& config() const { return config_; }

 private:
    OutageDetectorConfig config_;
    bool probed_ = false;
    int consecutiveFailures_ = 0;
    TimeMillis firstFailureAtMillis_ = 0;
    TimeMillis lastFailureAtMillis_ = 0;
};

}  // namespace fallback
}  // namespace showmesh
