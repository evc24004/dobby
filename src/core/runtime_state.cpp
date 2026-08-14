#include "core/runtime_state.hpp"

#include "core/config.hpp"
#include "diagnostics/stream_probe.hpp"
#include "platform/files.hpp"
#include "platform/preferences_store.hpp"

namespace dobby {

namespace {

constexpr std::uint8_t kEntityHitboxesBit = 1U << 0U;
constexpr std::uint8_t kChestEspBit = 1U << 1U;
constexpr std::uint8_t kOreEspBit = 1U << 2U;
constexpr std::uint8_t kAllEspBits =
        kEntityHitboxesBit | kChestEspBit | kOreEspBit;

} // namespace

extern "C" [[gnu::visibility("hidden")]]
std::atomic_uint8_t dobby_esp_feature_mask{kAllEspBits};

static_assert(std::atomic_uint8_t::is_always_lock_free);
static_assert(sizeof(std::atomic_uint8_t) == sizeof(std::uint8_t));

namespace {

bool espFeatureEnabled(std::uint8_t feature) {
    return (dobby_esp_feature_mask.load(std::memory_order_relaxed) & feature) != 0;
}

void setEspFeature(std::uint8_t feature, bool enabled) {
    if (enabled) {
        dobby_esp_feature_mask.fetch_or(feature, std::memory_order_relaxed);
    } else {
        dobby_esp_feature_mask.fetch_and(
                static_cast<std::uint8_t>(~feature), std::memory_order_relaxed);
    }
}

bool toggleEspFeature(std::uint8_t feature) {
    const std::uint8_t previous =
            dobby_esp_feature_mask.fetch_xor(feature, std::memory_order_relaxed);
    return ((previous ^ feature) & feature) != 0;
}

} // namespace

RuntimeState::RuntimeState()
: sessionStartedAt_(timestamp()),
  autoPopup_(config().autoPopup),
  verbose_(config().verbose) {
    const DeveloperPreferences preferences = loadDeveloperPreferences({
            .autoPopup = config().autoPopup,
            .entityHitboxes = true,
            .chestEsp = true,
            .oreEsp = true,
            .networkMetricsOverlay = true,
            .packetTrafficOverlay = true,
    });
    autoPopup_.store(preferences.autoPopup, std::memory_order_relaxed);
    std::uint8_t espMask = 0;
    espMask |= preferences.entityHitboxes ? kEntityHitboxesBit : 0;
    espMask |= preferences.chestEsp ? kChestEspBit : 0;
    espMask |= preferences.oreEsp ? kOreEspBit : 0;
    dobby_esp_feature_mask.store(espMask, std::memory_order_relaxed);
    networkMetricsOverlay_.store(
            preferences.networkMetricsOverlay, std::memory_order_relaxed);
    packetTrafficOverlay_.store(
            preferences.packetTrafficOverlay, std::memory_order_relaxed);
}

void RuntimeState::setHookStatus(std::string status, bool warningHook, bool streamProbe) {
    warningHookInstalled_.store(warningHook, std::memory_order_relaxed);
    streamProbeInstalled_.store(streamProbe, std::memory_order_relaxed);
    std::lock_guard lock(mutex_);
    hookStatus_ = std::move(status);
}

void RuntimeState::addDiagnostic(Diagnostic diagnostic) {
    totalViolations_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(mutex_);
    history_.push_back(std::move(diagnostic));
    if (history_.size() > config().historyLimit)
        history_.erase(history_.begin(), history_.begin() +
                static_cast<std::ptrdiff_t>(history_.size() - config().historyLimit));
}

std::optional<Diagnostic> RuntimeState::latestDiagnostic() const {
    std::lock_guard lock(mutex_);
    if (history_.empty())
        return std::nullopt;
    return history_.back();
}

void RuntimeState::clearDiagnostics() {
    {
        std::lock_guard lock(mutex_);
        history_.clear();
    }
    totalViolations_.store(0, std::memory_order_relaxed);
    clearStreamProbe();
}

RuntimeSnapshot RuntimeState::snapshot() const {
    RuntimeSnapshot result;
    {
        std::lock_guard lock(mutex_);
        result.sessionStartedAt = sessionStartedAt_;
        result.hookStatus = hookStatus_;
        result.retainedViolations = history_.size();
    }
    result.hookInstalled = warningHookInstalled_.load(std::memory_order_relaxed);
    result.streamProbeInstalled = streamProbeInstalled_.load(std::memory_order_relaxed);
    result.autoPopup = autoPopup();
    result.verbose = verbose();
    result.totalViolations = totalViolations_.load(std::memory_order_relaxed);
    return result;
}

bool RuntimeState::autoPopup() const {
    return autoPopup_.load(std::memory_order_relaxed);
}

bool RuntimeState::toggleAutoPopup() {
    return !autoPopup_.exchange(!autoPopup(), std::memory_order_relaxed);
}

bool RuntimeState::verbose() const {
    return verbose_.load(std::memory_order_relaxed);
}

bool RuntimeState::toggleVerbose() {
    return !verbose_.exchange(!verbose(), std::memory_order_relaxed);
}

bool RuntimeState::anyEspEnabled() const {
    return dobby_esp_feature_mask.load(std::memory_order_relaxed) != 0;
}

bool RuntimeState::entityHitboxes() const {
    return espFeatureEnabled(kEntityHitboxesBit);
}

void RuntimeState::setEntityHitboxes(bool enabled) {
    setEspFeature(kEntityHitboxesBit, enabled);
}

bool RuntimeState::entityHitboxesAvailable() const {
    return entityHitboxesAvailable_.load(std::memory_order_relaxed);
}

void RuntimeState::setEntityHitboxesAvailable(bool available) {
    entityHitboxesAvailable_.store(available, std::memory_order_relaxed);
}

bool RuntimeState::chestEsp() const {
    return espFeatureEnabled(kChestEspBit);
}

bool RuntimeState::toggleChestEsp() {
    return toggleEspFeature(kChestEspBit);
}

bool RuntimeState::chestEspAvailable() const {
    return chestEspAvailable_.load(std::memory_order_relaxed);
}

void RuntimeState::setChestEspAvailable(bool available) {
    chestEspAvailable_.store(available, std::memory_order_relaxed);
}

bool RuntimeState::oreEsp() const {
    return espFeatureEnabled(kOreEspBit);
}

bool RuntimeState::toggleOreEsp() {
    return toggleEspFeature(kOreEspBit);
}

bool RuntimeState::oreEspAvailable() const {
    return oreEspAvailable_.load(std::memory_order_relaxed);
}

void RuntimeState::setOreEspAvailable(bool available) {
    oreEspAvailable_.store(available, std::memory_order_relaxed);
}

bool RuntimeState::networkMetricsOverlay() const {
    return networkMetricsOverlay_.load(std::memory_order_relaxed);
}

bool RuntimeState::toggleNetworkMetricsOverlay() {
    return !networkMetricsOverlay_.exchange(
            !networkMetricsOverlay(), std::memory_order_relaxed);
}

bool RuntimeState::packetTrafficOverlay() const {
    return packetTrafficOverlay_.load(std::memory_order_relaxed);
}

bool RuntimeState::togglePacketTrafficOverlay() {
    return !packetTrafficOverlay_.exchange(
            !packetTrafficOverlay(), std::memory_order_relaxed);
}

bool RuntimeState::packetTrafficAvailable() const {
    return packetTrafficAvailable_.load(std::memory_order_relaxed);
}

void RuntimeState::setPacketTrafficAvailable(bool available) {
    packetTrafficAvailable_.store(available, std::memory_order_relaxed);
}

DeveloperPreferences RuntimeState::developerPreferences() const {
    return {
            .autoPopup = autoPopup(),
            .entityHitboxes = entityHitboxes(),
            .chestEsp = chestEsp(),
            .oreEsp = oreEsp(),
            .networkMetricsOverlay = networkMetricsOverlay(),
            .packetTrafficOverlay = packetTrafficOverlay(),
    };
}

RuntimeState& runtimeState() {
    static RuntimeState state;
    return state;
}

} // namespace dobby
