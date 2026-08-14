#include "platform/launcher.hpp"

#if defined(__ANDROID__)

#include "platform/files.hpp"

#include <dlfcn.h>
#include <string>

namespace dobby {
namespace {

using HostSetClipboardTextFn = void (*)(const char* text);
using AddMenuFn = void (*)(std::size_t length, LauncherMenuEntry* entries);
using ShowWindowFn = void (*)(
        const char* title, int isModal, void* user, void (*onClose)(void* user),
        int count, LauncherControl* controls);
using AddSwapBuffersCallbackFn = void (*)(void* user, LauncherSwapBuffersCallback callback);
using EglQuerySurfaceFn = unsigned int (*)(void* display, void* surface, int attribute, int* value);

HostSetClipboardTextFn hostSetClipboardText = nullptr;
AddMenuFn launcherAddMenu = nullptr;
ShowWindowFn launcherShowWindow = nullptr;
AddSwapBuffersCallbackFn launcherAddSwapBuffersCallback = nullptr;
EglQuerySurfaceFn eglQuerySurface = nullptr;

} // namespace

void resolveLauncherApi() {
    if (mcpelauncher_addmenu != nullptr)
        launcherAddMenu = mcpelauncher_addmenu;
    if (mcpelauncher_show_window != nullptr)
        launcherShowWindow = mcpelauncher_show_window;

    void* menuLibrary = dlopen("libmcpelauncher_menu.so", RTLD_NOW | RTLD_NOLOAD);
    if (menuLibrary == nullptr)
        menuLibrary = dlopen("libmcpelauncher_menu.so", RTLD_NOW);
    if (menuLibrary != nullptr) {
        if (launcherAddMenu == nullptr)
            launcherAddMenu = reinterpret_cast<AddMenuFn>(dlsym(menuLibrary, "mcpelauncher_addmenu"));
        if (launcherShowWindow == nullptr)
            launcherShowWindow = reinterpret_cast<ShowWindowFn>(
                    dlsym(menuLibrary, "mcpelauncher_show_window"));
    }

    if (launcherAddSwapBuffersCallback == nullptr) {
        void* gameWindowLibrary = dlopen("libmcpelauncher_gamewindow.so", RTLD_NOW | RTLD_NOLOAD);
        if (gameWindowLibrary == nullptr)
            gameWindowLibrary = dlopen("libmcpelauncher_gamewindow.so", RTLD_NOW);
        if (gameWindowLibrary != nullptr) {
            launcherAddSwapBuffersCallback = reinterpret_cast<AddSwapBuffersCallbackFn>(
                    dlsym(gameWindowLibrary, "game_window_add_swap_buffers_callback"));
        }
    }

    if (eglQuerySurface == nullptr) {
        void* eglLibrary = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
        if (eglLibrary == nullptr)
            eglLibrary = dlopen("libEGL.so", RTLD_NOW);
        if (eglLibrary != nullptr) {
            eglQuerySurface = reinterpret_cast<EglQuerySurfaceFn>(
                    dlsym(eglLibrary, "eglQuerySurface"));
        }
    }

    if (hostSetClipboardText == nullptr && mcpelauncher_host_dlopen != nullptr &&
        mcpelauncher_host_dlsym != nullptr) {
        void* host = mcpelauncher_host_dlopen(nullptr, RTLD_NOW);
        if (host != nullptr) {
            hostSetClipboardText = reinterpret_cast<HostSetClipboardTextFn>(
                    mcpelauncher_host_dlsym(host, "_ZN5ImGui16SetClipboardTextEPKc"));
        }
    }
}

bool launcherWindowAvailable() { return launcherShowWindow != nullptr; }
bool launcherMenuAvailable() { return launcherAddMenu != nullptr; }
bool launcherClipboardAvailable() { return hostSetClipboardText != nullptr; }

void* resolveHostSymbol(const char* name) {
    if (name == nullptr || mcpelauncher_host_dlopen == nullptr ||
        mcpelauncher_host_dlsym == nullptr) {
        return nullptr;
    }
    void* host = mcpelauncher_host_dlopen(nullptr, RTLD_NOW);
    return host == nullptr ? nullptr : mcpelauncher_host_dlsym(host, name);
}

bool addLauncherSwapBuffersCallback(void* user, LauncherSwapBuffersCallback callback) {
    resolveLauncherApi();
    if (launcherAddSwapBuffersCallback == nullptr || callback == nullptr)
        return false;
    launcherAddSwapBuffersCallback(user, callback);
    return true;
}

bool addMinecraftImageLoadedCallback(
        void* user, MinecraftImageLoadedCallback callback) {
    if (mcpelauncher_preinithook2 == nullptr || callback == nullptr)
        return false;
    // The launcher builds the libminecraftpe hook table after all preinit mods
    // run, then invokes this callback once the image is loaded and registered.
    // The replacement is intentionally null: this is a lifecycle notification,
    // not an ELF-symbol interposition.
    mcpelauncher_preinithook2(
            "__dobby_minecraft_image_loaded__", nullptr, user, callback);
    return true;
}

bool launcherSurfaceSize(void* display, void* surface, int& width, int& height) {
    resolveLauncherApi();
    constexpr int eglWidth = 0x3057;
    constexpr int eglHeight = 0x3056;
    width = 0;
    height = 0;
    return eglQuerySurface != nullptr && surface != nullptr &&
            eglQuerySurface(display, surface, eglWidth, &width) != 0U &&
            eglQuerySurface(display, surface, eglHeight, &height) != 0U &&
            width > 0 && height > 0;
}

void addLauncherMenu(std::span<LauncherMenuEntry> entries) {
    resolveLauncherApi();
    if (launcherAddMenu != nullptr)
        launcherAddMenu(entries.size(), entries.data());
}

void showLauncherWindow(
        const char* title, std::span<LauncherControl> controls,
        void (*onClose)(void*)) {
    resolveLauncherApi();
    if (launcherShowWindow != nullptr) {
        launcherShowWindow(title, 0, nullptr, onClose,
                           static_cast<int>(controls.size()), controls.data());
    }
}

bool copyToClipboard(std::string_view text) {
    writeFile(clipboardPath(), text, "w");
    resolveLauncherApi();
    if (hostSetClipboardText == nullptr)
        return false;
    const std::string owned(text);
    hostSetClipboardText(owned.c_str());
    return true;
}

LauncherControl textControl(const char* text, int size) {
    LauncherControl control{};
    control.type = 3;
    control.data.text.label = text;
    control.data.text.size = size;
    return control;
}

LauncherControl buttonControl(const char* label, void (*callback)(void*)) {
    LauncherControl control{};
    control.type = 0;
    control.data.button.label = label;
    control.data.button.user = nullptr;
    control.data.button.onClick = callback;
    return control;
}

} // namespace dobby
#endif
