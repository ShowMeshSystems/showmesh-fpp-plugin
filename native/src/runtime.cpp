#include "showmesh/runtime.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <string>

#include "showmesh/brightness_codec.h"
#include "showmesh/saturating_add.h"

namespace showmesh {

const char* const kBrightnessCommandName = "ShowMesh: Set Brightness Ceiling";
const char* const kTargetPercentArgument = "targetPercent";
const char* const kFadeSecondsArgument = "fadeSeconds";
const char* const kPluginName = "fpp-showmesh";

namespace {

// Parses a whole decimal number and refuses anything else, including a
// fractional value, trailing text, and a value that does not fit. FPP
// hands command arguments through as strings, so this is the only place
// the declared integer bounds can actually be enforced. Surrounding
// whitespace is formatting rather than value and is trimmed; anything
// inside the number is not.
bool parseWholeNumber(const std::string& text, long long* out) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    const std::string trimmed = text.substr(begin, end - begin);
    if (trimmed.empty()) return false;

    char* stop = nullptr;
    errno = 0;
    const long long v = std::strtoll(trimmed.c_str(), &stop, 10);
    if (errno != 0 || stop == nullptr || *stop != '\0') return false;
    *out = v;
    return true;
}

}  // namespace

bool playlistNameIsPathSafe(const std::string& name) {
    if (name.empty()) return false;
    // Without a separator a name is a single path component, and the only
    // components that can escape a directory are "." and "..", so refusing
    // a leading dot is what closes it. A "-" left inside the name, or a
    // name like "Act 1..Act 2", is an ordinary playlist name.
    if (name.front() == '.') return false;
    if (name.find('/') != std::string::npos) return false;
    if (name.find('\\') != std::string::npos) return false;
    for (char c : name) {
        if (static_cast<unsigned char>(c) < 0x20) return false;
    }
    return true;
}

ShowMeshRuntime::ShowMeshRuntime(PlaylistDefinitionSource* definitions, ObservationSink* sink, Clock clock,
                                 SequenceFileStore* sequenceStore)
    : definitions_(definitions), sink_(sink), clock_(clock), sequenceStore_(sequenceStore), handoff_(16) {
    if (definitions_ != nullptr) {
        std::lock_guard<std::mutex> lock(engineMutex_);
        engine_.setInstanceId(definitions_->instanceUuid());
    }
    // Resuming above the highest value ever issued, not above the highest
    // value ever acknowledged: sequence_.restore() only moves forward, so
    // this can never rewind a value this same process already holds
    // higher in memory, and it is exactly what turns a plugin restart
    // from a permanent 409 wedge into an ordinary resumption.
    if (sequenceStore_ != nullptr) {
        const SequenceFileStore::LoadResult loaded = sequenceStore_->loadDetailed();
        sequence_.restore(loaded.value);
        sequenceFilesWereAllInvalidAtStartup_ = loaded.filesPresentButInvalid;
    }
}

ShowMeshRuntime::~ShowMeshRuntime() { stop(); }

CommandOutcome ShowMeshRuntime::applyBrightnessCommand(const std::string& targetPercent,
                                                       const std::string& fadeSeconds) {
    long long percent = 0;
    long long seconds = 0;
    if (!parseWholeNumber(targetPercent, &percent)) {
        return CommandOutcome{false, "target percent must be a whole number between 0 and 100"};
    }
    if (!parseWholeNumber(fadeSeconds, &seconds)) {
        return CommandOutcome{false, "fade seconds must be a whole number between 0 and 86400"};
    }
    if (percent < kMinPercent || percent > kMaxPercent) {
        return CommandOutcome{false, "target percent must be between 0 and 100"};
    }
    if (seconds < 0 || seconds > kMaxFadeSeconds) {
        return CommandOutcome{false, "fade seconds must be between 0 and 86400"};
    }

    ValidationResult result;
    {
        std::lock_guard<std::mutex> lock(engineMutex_);
        result = engine_.setCeiling(static_cast<int>(percent), seconds, clock_());
    }
    if (!result.ok) {
        return CommandOutcome{false, result.error};
    }
    if (seconds == 0) {
        return CommandOutcome{true, "brightness ceiling set to " + std::to_string(percent) + " percent"};
    }
    return CommandOutcome{true, "brightness ceiling fading to " + std::to_string(percent) + " percent over " +
                                    std::to_string(seconds) + " seconds"};
}

void ShowMeshRuntime::observeCallback(const char* playlistName, const char* action, const char* section, int item,
                                      const char* sequenceFilename, const char* mediaFilename) {
    CallbackEvidence evidence;
    evidence.setPlaylistName(playlistName);
    evidence.setSection(section);
    evidence.setSequenceFilename(sequenceFilename);
    evidence.setMediaFilename(mediaFilename);
    evidence.position = item;
    evidence.action = playlistActionFromName(action == nullptr ? std::string() : std::string(action));
    evidence.observedAtMillis = clock_();
    handoff_.offer(evidence);
    // hasWork_ is set under wakeMutex_ before notifying so the worker's
    // wait predicate observes it even if this notify lands before the
    // worker calls wait_for; otherwise the notification is lost and the
    // worker sits idle for up to 250ms.
    {
        std::lock_guard<std::mutex> lock(wakeMutex_);
        hasWork_ = true;
    }
    wake_.notify_one();
}

void ShowMeshRuntime::modifyChannelData(std::uint8_t* channelData, std::size_t channelCount) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    engine_.applyToFrame(channelData, channelCount, clock_());
}

std::string ShowMeshRuntime::encodeFullState() {
    std::lock_guard<std::mutex> lock(engineMutex_);
    return encodeBrightnessState(engine_.captureState(clock_()));
}

StateAdoption ShowMeshRuntime::adoptEncodedFullState(const std::uint8_t* data, int length) {
    if (data == nullptr || length <= 0) return StateAdoption::kRejectedUnsupportedVersion;
    BrightnessStateDecode decoded =
        decodeBrightnessState(std::string(reinterpret_cast<const char*>(data), static_cast<std::size_t>(length)));
    if (!decoded.ok) return StateAdoption::kRejectedUnsupportedVersion;
    std::lock_guard<std::mutex> lock(engineMutex_);
    return engine_.adoptState(decoded.state, clock_());
}

bool ShowMeshRuntime::drainOnce() {
    CallbackEvidence evidence;
    std::uint32_t coalesced = 0;
    if (!handoff_.take(&evidence, &coalesced)) return false;
    unacknowledgedCoalesced_ = saturatingAdd(unacknowledgedCoalesced_, coalesced);

    PlaylistEntryObservation observation;
    observation.schemaVersion = kObservationSchemaVersion;
    observation.sequenceFilename = evidence.sequenceFilename;
    observation.mediaFilename = evidence.mediaFilename;
    observation.sequenceFilenameTruncated = evidence.sequenceFilenameTruncated;
    observation.mediaFilenameTruncated = evidence.mediaFilenameTruncated;
    observation.action = evidence.action;
    observation.observedAtMillis = evidence.observedAtMillis;
    observation.sequence = sequence_.next();
    observation.coalescedSincePreviousAcknowledged = unacknowledgedCoalesced_;
    // Persisted for every drained event, whether or not identity
    // resolves and whether or not the sink ultimately accepts it: the
    // number was already minted and must never be reissued after a
    // restart, so it has to be durable before this function returns
    // rather than only once a publish succeeds. The observation still
    // publishes below on failure; the number is already minted and
    // dropping the observation would lose data, so a failed store()
    // is only counted, never treated as a reason to stop.
    if (sequenceStore_ != nullptr && !sequenceStore_->store(observation.sequence)) {
        ++sequencePersistFailures_;
    }

    if (evidence.identityFieldTruncated()) {
        // A truncated playlist name or section can share its bounded
        // prefix with a different entry's; reporting it as identity would
        // be confidently wrong, so it is reported unavailable instead and
        // never reaches resolveEntryIdentity.
        observation.unavailable = IdentityUnavailable::kTruncatedIdentityField;
        ++unavailable_;
        // An unavailable observation is still an observation the
        // coordinator can acknowledge: only clear the gap on acceptance,
        // never on a refusal, which must still ride forward.
        if (sink_ != nullptr && sink_->publishUnavailable(observation)) {
            unacknowledgedCoalesced_ = 0;
        }
        return true;
    }

    const std::string instanceUuid = definitions_ == nullptr ? std::string() : definitions_->instanceUuid();
    const std::string definition =
        definitions_ == nullptr ? std::string() : definitions_->definitionFor(evidence.playlistName);

    IdentityResolution resolution =
        resolveEntryIdentity(instanceUuid, evidence.playlistName, definition, evidence.section, evidence.position);

    if (!resolution.ok) {
        observation.unavailable = resolution.reason;
        observation.identity.playlistName = evidence.playlistName;
        observation.identity.section = evidence.section;
        observation.identity.position = evidence.position;
        ++unavailable_;
        if (sink_ != nullptr && sink_->publishUnavailable(observation)) {
            unacknowledgedCoalesced_ = 0;
        }
        return true;
    }

    observation.identity = resolution.identity;
    observation.entryKey = resolution.entryKey;
    const bool accepted = sink_ != nullptr && sink_->publish(observation);
    if (accepted) {
        ++published_;
        unacknowledgedCoalesced_ = 0;
    }
    return true;
}

// sequence_ is otherwise only touched from the worker thread (inside
// drainOnce()), so a caller invoking this from another thread while the
// worker is still running would race it. Callers must stop() (which
// joins the worker) before calling this from outside the worker thread,
// the same precondition drainOnce() itself already documents for direct
// test use.
bool ShowMeshRuntime::flushSequenceState() {
    if (sequenceStore_ == nullptr) return true;
    return sequenceStore_->store(sequence_.current());
}

void ShowMeshRuntime::workerLoop() {
    while (running_.load()) {
        while (drainOnce()) {
            if (!running_.load()) return;
        }
        if (testHookBeforeWait_) testHookBeforeWait_();
        std::unique_lock<std::mutex> lock(wakeMutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(250), [this] { return hasWork_ || !running_.load(); });
        hasWork_ = false;
    }
}

void ShowMeshRuntime::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread(&ShowMeshRuntime::workerLoop, this);
}

void ShowMeshRuntime::stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(wakeMutex_);
        hasWork_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

}  // namespace showmesh
