#include "ui/window_policy.hpp"

namespace dobby {

std::uint32_t dobbyWindowFlags(std::string_view title, std::uint32_t flags) {
    if (title != "Dobby" && !title.starts_with("Dobby##dobby_"))
        return flags;

    constexpr std::uint32_t noResize = 1U << 1U;
    constexpr std::uint32_t noCollapse = 1U << 5U;
    constexpr std::uint32_t alwaysAutoResize = 1U << 6U;
    constexpr std::uint32_t noSavedSettings = 1U << 8U;
    return flags | noResize | noCollapse | alwaysAutoResize | noSavedSettings;
}

} // namespace dobby

#if defined(__ANDROID__)

#include "platform/launcher.hpp"
#include "platform/log.hpp"
#include "ui/developer_ui.hpp"

#include <atomic>
#include <cstddef>

namespace dobby {
namespace {

using FindWindowByNameFn = void* (*)(const char* name);
using SetWindowCollapsedFn = void (*)(const char* name, bool collapsed, int condition);

constexpr char kFindWindowByNameSymbol[] = "_ZN5ImGui16FindWindowByNameEPKc";
constexpr char kSetWindowCollapsedSymbol[] = "_ZN5ImGui18SetWindowCollapsedEPKcbi";
constexpr std::ptrdiff_t kImGuiWindowFlagsOffset = 0x14;
constexpr int kAlwaysCondition = 1;
constexpr std::uint32_t kStatusWindowRequest = 1U << 0U;
constexpr std::uint32_t kViolationWindowRequest = 1U << 1U;
constexpr char kStatusWindowTitle[] = "Dobby##dobby_status_v2";
constexpr char kViolationWindowTitle[] = "Dobby##dobby_violation_v3";

FindWindowByNameFn findWindowByName = nullptr;
SetWindowCollapsedFn setWindowCollapsed = nullptr;
std::atomic<std::uint32_t> requestedWindows{0};

std::uint32_t requestForTitle(std::string_view title) {
    if (title == kStatusWindowTitle)
        return kStatusWindowRequest;
    if (title == kViolationWindowTitle)
        return kViolationWindowRequest;
    return 0;
}

bool applyPolicyNow(const char* title) {
    if (findWindowByName == nullptr)
        return false;
    void* window = findWindowByName(title);
    if (window == nullptr)
        return false;

    // ImGuiWindow::Flags is +0x14 in the launcher's pinned ImGui 1.92.5 ABI.
    // The launcher invokes this between rendered frames, never from the network thread.
    auto* flags = reinterpret_cast<std::uint32_t*>(
            static_cast<std::byte*>(window) + kImGuiWindowFlagsOffset);
    *flags = dobbyWindowFlags(title, *flags);
    if (setWindowCollapsed != nullptr)
        setWindowCollapsed(title, false, kAlwaysCondition);
    return true;
}

void applyRequestedWindows(void*, void*, void*) {
    // Popup requests originate from packet/network callbacks. Consume them
    // here on the launcher's render/UI thread so worker-thread callbacks
    // cannot lose the window behind Minecraft's generic Block screen.
    showPendingViolationPopup();

    const std::uint32_t requests = requestedWindows.load(std::memory_order_acquire);
    std::uint32_t applied = 0;
    if ((requests & kStatusWindowRequest) != 0 && applyPolicyNow(kStatusWindowTitle))
        applied |= kStatusWindowRequest;
    if ((requests & kViolationWindowRequest) != 0 && applyPolicyNow(kViolationWindowTitle))
        applied |= kViolationWindowRequest;
    if (applied != 0) {
        requestedWindows.fetch_and(~applied, std::memory_order_release);
        logLine("UI: fixed-size policy applied to Dobby window");
    }
}

} // namespace

bool installDobbyWindowPolicy() {
    findWindowByName = reinterpret_cast<FindWindowByNameFn>(
            resolveHostSymbol(kFindWindowByNameSymbol));
    setWindowCollapsed = reinterpret_cast<SetWindowCollapsedFn>(
            resolveHostSymbol(kSetWindowCollapsedSymbol));
    if (findWindowByName == nullptr || setWindowCollapsed == nullptr) {
        logLine("ERROR: compatible ImGui window policy API unavailable");
        return false;
    }
    if (!addLauncherSwapBuffersCallback(nullptr, applyRequestedWindows)) {
        logLine("ERROR: launcher render callback unavailable; fixed Dobby windows disabled");
        return false;
    }
    logLine("fixed-size Dobby ImGui window policy ready");
    return true;
}

void applyDobbyWindowPolicy(const char* title) {
    if (title == nullptr)
        return;
    const std::uint32_t request = requestForTitle(title);
    if (request != 0)
        requestedWindows.fetch_or(request, std::memory_order_release);
}

} // namespace dobby

#else

namespace dobby {

bool installDobbyWindowPolicy() { return false; }
void applyDobbyWindowPolicy(const char*) {}

} // namespace dobby

#endif
