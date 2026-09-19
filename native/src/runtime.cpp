#include "showmesh/runtime.h"

#include <algorithm>
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
                                 SequenceFileStore* sequenceStore, DefinitionPublisher* definitionPublisher,
                                 BrightnessFileStore* brightnessStore, int safeCeilingPercent,
                                 FallbackActivationRecorder* fallbackRecorder)
    : definitions_(definitions),
      sink_(sink),
      clock_(clock),
      sequenceStore_(sequenceStore),
      definitionPublisher_(definitionPublisher),
      fallbackRecorder_(fallbackRecorder),
      brightnessStore_(brightnessStore),
      handoff_(16) {
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
    // Restored before start(): nothing else can have touched engine_ yet,
    // so this needs no lock for correctness, but takes engineMutex_ anyway
    // to match every other access to engine_ in this class and to keep
    // TSan happy about the mutex's own initialization-order assumptions.
    //
    // Three outcomes, not two: a load that never found any record at all
    // (loaded.ok=false, loaded.recordExpectedButUnreadable=false) is the
    // very first run, where the engine's built-in defaults already ARE
    // what a process with no persisted state starts at, so there is
    // nothing to restore. Anything else -- a trusted primary
    // (loaded.trustedAsCurrent), a backup recovered only because the
    // primary was corrupt, or neither file parsing despite one existing
    // -- means state was durably written here at some point. Only the
    // trusted-primary case is handed to restoreFromPersisted; the other
    // two hand the engine nothing it can vouch for, so it settles at the
    // operator-configured safe ceiling rather than risk the backup's
    // superseded numbers or its own bright defaults being wrong in the
    // brighter direction. A read error must not turn the rig off, so the
    // settle lands at safeCeilingPercent (clamped below), not at zero.
    const int clampedSafeCeilingPercent = std::min(std::max(safeCeilingPercent, kMinPercent), kMaxPercent);
    if (brightnessStore_ != nullptr) {
        BrightnessStateLoad loaded = brightnessStore_->load();
        if (loaded.ok && loaded.trustedAsCurrent) {
            std::lock_guard<std::mutex> lock(engineMutex_);
            engine_.restoreFromPersisted(loaded.state, clock_());
        } else if (loaded.ok) {
            // Primary unreadable; only the superseded backup parsed. The
            // backup is still a previous good record for the gate's own
            // purposes, so it stays closed here if the backup said so,
            // even though the ceiling and gain settle to the safe value.
            brightnessRestartTrust_ = BrightnessRestartTrust::kPrimaryUnreadableBackupRecovered;
            std::lock_guard<std::mutex> lock(engineMutex_);
            engine_.settleSafeAfterUntrustedRestart(clampedSafeCeilingPercent, clock_(), loaded.state.weatherGateClosed);
        } else if (loaded.recordExpectedButUnreadable) {
            // Neither file parsed, despite one existing.
            brightnessRestartTrust_ = BrightnessRestartTrust::kNeitherRecordReadable;
            std::lock_guard<std::mutex> lock(engineMutex_);
            engine_.settleSafeAfterUntrustedRestart(clampedSafeCeilingPercent, clock_());
        }
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
                                      const char* sequenceFilename, const char* mediaFilename,
                                      std::optional<int> playlistLoop) {
    CallbackEvidence evidence;
    evidence.setPlaylistName(playlistName);
    evidence.setSection(section);
    evidence.setSequenceFilename(sequenceFilename);
    evidence.setMediaFilename(mediaFilename);
    evidence.position = item;
    evidence.action = playlistActionFromName(action == nullptr ? std::string() : std::string(action));
    evidence.observedAtMillis = clock_();
    evidence.playlistLoop = playlistLoop;
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

TransitionGainResponse ShowMeshRuntime::applyTransitionGain(const std::string& body) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    return applyTransitionGainRequest(body, &engine_, &lastTransitionGainRequestId_, clock_());
}

WeatherGateResponse ShowMeshRuntime::applyWeatherGate(const std::string& body) {
    WeatherGateResponse result;
    {
        std::lock_guard<std::mutex> lock(engineMutex_);
        result = applyWeatherGateRequest(body, &engine_, clock_());
    }
    // See the declaration's comment: an applied gate change must be
    // durable before this returns, not merely marked dirty for the next
    // output frame, which may never come if fppd is not currently playing.
    if (result.status == 200) flushBrightnessState();
    return result;
}

WeatherGateResponse ShowMeshRuntime::weatherGateState() {
    std::lock_guard<std::mutex> lock(engineMutex_);
    return renderWeatherGateState(&engine_, clock_());
}

void ShowMeshRuntime::RuntimeSweepRecord::requestSweep() {
    runtime_->sweepRequested_.fetch_add(1);
    // Woken the same way an observation wakes it, so the sweep starts now
    // rather than at the end of the worker's 250ms poll.
    {
        std::lock_guard<std::mutex> lock(runtime_->wakeMutex_);
        runtime_->hasWork_ = true;
    }
    runtime_->wake_.notify_one();
}

bool ShowMeshRuntime::RuntimeSweepRecord::sweepPending() const {
    return runtime_->sweepRequested_.load() != runtime_->sweepCompleted_.load();
}

DefinitionRepublishResponse ShowMeshRuntime::applyDefinitionRepublish(const std::string& body) {
    std::lock_guard<std::mutex> lock(republishMutex_);
    return applyDefinitionRepublishRequest(body, definitionPublisher_, &sweepRecord_, &lastRepublishRequestId_);
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
    // Carried on every observation, including the unavailable ones below:
    // the pass counter is corroborating evidence, never identity, so it
    // stays true even when identity could not be resolved.
    observation.playlistLoop = evidence.playlistLoop;
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

    // Read once and reused by both unavailable paths below, as well as
    // the resolved path further down: an unavailable observation still
    // carries whatever instance UUID the plugin actually has. Omitting it
    // is not "identity partially unknown", it is a different observation
    // the coordinator refuses outright (buildObservationBody() refuses
    // any observation with an empty instanceUuid, matching the
    // coordinator's own step 6 refusal), which is only correct when the
    // UUID is genuinely unavailable.
    const std::string instanceUuid = definitions_ == nullptr ? std::string() : definitions_->instanceUuid();

    if (evidence.identityFieldTruncated()) {
        // A truncated playlist name or section can share its bounded
        // prefix with a different entry's; reporting it as identity would
        // be confidently wrong, so it is reported unavailable instead and
        // never reaches resolveEntryIdentity.
        observation.unavailable = IdentityUnavailable::kTruncatedIdentityField;
        observation.identity.instanceUuid = instanceUuid;
        ++unavailable_;
        // An unavailable observation is still an observation the
        // coordinator can acknowledge: only clear the gap on acceptance,
        // never on a refusal, which must still ride forward.
        if (sink_ != nullptr && sink_->publishUnavailable(observation)) {
            unacknowledgedCoalesced_ = 0;
        }
        return true;
    }

    const std::string definition =
        definitions_ == nullptr ? std::string() : definitions_->definitionFor(evidence.playlistName);

    IdentityResolution resolution =
        resolveEntryIdentity(instanceUuid, evidence.playlistName, definition, evidence.section, evidence.position);

    if (!resolution.ok) {
        observation.unavailable = resolution.reason;
        observation.identity.instanceUuid = instanceUuid;
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
    // Right where entryKey is known, and nowhere else: this is the one
    // key the fallback resolver looks up, already computed by
    // resolveEntryIdentity() above, never re-derived a second way. Never
    // gated on definitionPublisher_ or sink_'s outcome below: recording
    // what the installed fallback program says about this entry does not
    // depend on whether the coordinator accepted the observation.
    if (fallbackRecorder_ != nullptr) {
        fallbackRecorder_->recordEntryKeyResolution(resolution.entryKey, evidence.observedAtMillis);
    }
    // Before the observation citing it, not after: an observation whose
    // definition has not arrived is still accepted, but Track H holds the
    // binding as having no definition until it does. The return value is
    // deliberately not gated on: a definition the coordinator could not
    // take is not a reason to withhold the observation, and the next
    // sweep retries the definition anyway.
    if (definitionPublisher_ != nullptr) {
        definitionPublisher_->publishDefinition(resolution.identity.instanceUuid, resolution.identity.playlistName,
                                                resolution.identity.playlistHash, resolution.canonicalDefinition,
                                                evidence.observedAtMillis);
    }
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

bool ShowMeshRuntime::flushBrightnessState() {
    if (brightnessStore_ == nullptr) return true;
    BrightnessState state;
    {
        std::lock_guard<std::mutex> lock(engineMutex_);
        // Nothing changed since the last successful flush: skip the
        // write rather than rotate an identical record into the backup
        // slot, which is also what stops an operator command's own
        // synchronous flush and the next frame's dirty mark from writing
        // the same state to disk twice.
        if (brightnessEverFlushed_ && engine_.revision() == flushedBrightnessRevision_) return true;
        state = engine_.captureState(clock_());
    }
    // The store's read, hash, write, two fsyncs, and rename all run here,
    // with engineMutex_ already released: a slow write against a real SD
    // card must never hold modifyChannelData (or another flush caller)
    // waiting on the same mutex for its duration.
    const bool ok = brightnessStore_->store(state);
    if (ok) {
        std::lock_guard<std::mutex> lock(engineMutex_);
        flushedBrightnessRevision_ = state.revision;
        brightnessEverFlushed_ = true;
    }
    return ok;
}

void ShowMeshRuntime::markBrightnessDirty() {
    {
        std::lock_guard<std::mutex> lock(wakeMutex_);
        brightnessDirty_ = true;
        hasWork_ = true;
    }
    wake_.notify_one();
}

bool ShowMeshRuntime::flushBrightnessIfDirty() {
    bool dirty;
    {
        std::lock_guard<std::mutex> lock(wakeMutex_);
        dirty = brightnessDirty_;
        brightnessDirty_ = false;
    }
    if (!dirty) return true;
    const bool ok = flushBrightnessState();
    if (!ok && brightnessFlushFailureHandler_) brightnessFlushFailureHandler_();
    return ok;
}

bool ShowMeshRuntime::sweepDefinitions() {
    if (definitions_ == nullptr || definitionPublisher_ == nullptr) return false;
    const std::string instanceUuid = definitions_->instanceUuid();
    // Without an instance UUID a definition cannot be filed against an
    // instance, and FPP reports one only once the host identity is
    // established. Leaving sweptOnce_ unset means the next worker pass
    // tries again rather than waiting out the re-scan interval.
    if (instanceUuid.empty()) return false;

    bool stopping = false;
    for (const std::string& playlistName : definitions_->playlistNames()) {
        // stop() has to be able to join promptly: a sweep of many
        // definitions against an unreachable coordinator spends its
        // bounded backoff once per definition.
        if (workerActive_.load() && !running_.load()) break;
        if (playlistNameIsPathSafe(playlistName)) {
            const std::string definition = definitions_->definitionFor(playlistName);
            // Section and position are not part of the playlist hash, so
            // any valid pair resolves the same definition hash; the entry
            // key this also produces is discarded.
            IdentityResolution resolution =
                resolveEntryIdentity(instanceUuid, playlistName, definition, std::string(), 0);
            if (resolution.ok) {
                definitionPublisher_->publishDefinition(instanceUuid, playlistName, resolution.identity.playlistHash,
                                                        resolution.canonicalDefinition, clock_());
            }
        }
        // Yield to the observation queue between definitions rather than
        // only after the whole sweep: against a slow or unreachable
        // coordinator, one definition can cost the retry policy's full
        // backoff budget, and the handoff holds only 16 pending events.
        // A sweep of many playlists that never came back to drainOnce()
        // let real-time callback events overflow the handoff and
        // coalesce away underneath it.
        while (drainOnce()) {
            if (workerActive_.load() && !running_.load()) {
                stopping = true;
                break;
            }
        }
        if (stopping) break;
    }

    lastSweepMillis_ = clock_();
    sweptOnce_ = true;
    return true;
}

bool ShowMeshRuntime::maybeSweepDefinitions() {
    // An owed sweep is served regardless of how recently the last one ran,
    // and the record is cleared only once the sweep it asked for has
    // actually completed, so sweepPending() answers a poller honestly.
    const std::uint64_t requested = sweepRequested_.load();
    const bool owed = requested != sweepCompleted_.load();
    if (!owed && sweptOnce_ && clock_() - lastSweepMillis_ < kDefinitionRescanIntervalMillis) return false;
    const bool swept = sweepDefinitions();
    if (swept && owed) sweepCompleted_.store(requested);
    return swept;
}

void ShowMeshRuntime::workerLoop() {
    workerActive_.store(true);
    // Exactly once, before this thread's first pass over drainOnce():
    // see FallbackActivationRecorder::performStartupFetch()'s own doc
    // comment for why this runs here rather than in that recorder's
    // constructor. workerLoop() itself only ever runs once per start(),
    // so no separate "already attempted" flag is needed to keep this to
    // one call.
    if (fallbackRecorder_ != nullptr) fallbackRecorder_->performStartupFetch();
    while (running_.load()) {
        while (drainOnce()) {
            if (!running_.load()) {
                workerActive_.store(false);
                return;
            }
        }
        // Off the frame thread and the command thread: see
        // markBrightnessDirty() and flushBrightnessState().
        flushBrightnessIfDirty();
        maybeSweepDefinitions();
        if (!running_.load()) break;
        if (testHookBeforeWait_) testHookBeforeWait_();
        std::unique_lock<std::mutex> lock(wakeMutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(250), [this] { return hasWork_ || !running_.load(); });
        hasWork_ = false;
    }
    workerActive_.store(false);
}

void ShowMeshRuntime::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread(&ShowMeshRuntime::workerLoop, this);
}

void ShowMeshRuntime::stop() {
    if (!running_.exchange(false)) return;
    // A worker stuck inside publish() or publishDefinition(), retrying
    // against an unreachable coordinator, must be interrupted before the
    // join below waits on it, not after: the join itself has no timeout.
    // A worker stuck inside performStartupFetch() is the identical
    // hazard, one time only, at start rather than per observation.
    if (sink_ != nullptr) sink_->requestStop();
    if (definitionPublisher_ != nullptr) definitionPublisher_->requestStop();
    if (fallbackRecorder_ != nullptr) fallbackRecorder_->requestStop();
    {
        std::lock_guard<std::mutex> lock(wakeMutex_);
        hasWork_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

}  // namespace showmesh
