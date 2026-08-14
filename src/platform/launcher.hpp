#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#if defined(__ANDROID__)

extern "C" void* mcpelauncher_patch(void* address, void* data, std::size_t size)
        __attribute__((weak));
extern "C" void mcpelauncher_addmenu(std::size_t length, struct LauncherMenuEntry* entries)
        __attribute__((weak));
extern "C" void mcpelauncher_show_window(
        const char* title, int isModal, void* user, void (*onClose)(void* user),
        int count, struct LauncherControl* controls) __attribute__((weak));
extern "C" void* mcpelauncher_host_dlopen(const char* path, int mode)
        __attribute__((weak));
extern "C" void* mcpelauncher_host_dlsym(void* handle, const char* symbol)
        __attribute__((weak));
extern "C" void mcpelauncher_preinithook2(
        const char* name, void* replacement, void* user,
        void (*callback)(void* user, void* replacement)) __attribute__((weak));

struct LauncherMenuEntry {
    const char* name;
    void* user;
    bool (*selected)(void* user);
    void (*click)(void* user);
    std::size_t length;
    LauncherMenuEntry* subentries;
};

struct LauncherControl {
    int type;
    union Data {
        struct {
            const char* label;
            void* user;
            void (*onClick)(void* user);
        } button;
        struct {
            const char* label;
            int size;
        } text;
        struct {
            const char* label;
            const char* def;
            const char* placeholder;
            void* user;
            void (*onChange)(void* user, const char* value);
        } textinput;
    } data;
};

namespace dobby {

using LauncherSwapBuffersCallback = void (*)(void* user, void* display, void* surface);
using MinecraftImageLoadedCallback = void (*)(void* user, void* replacement);

void resolveLauncherApi();
bool launcherWindowAvailable();
bool launcherMenuAvailable();
bool launcherClipboardAvailable();
void* resolveHostSymbol(const char* name);
bool addLauncherSwapBuffersCallback(void* user, LauncherSwapBuffersCallback callback);
bool addMinecraftImageLoadedCallback(
        void* user, MinecraftImageLoadedCallback callback);
bool launcherSurfaceSize(void* display, void* surface, int& width, int& height);
void addLauncherMenu(std::span<LauncherMenuEntry> entries);
void showLauncherWindow(
        const char* title, std::span<LauncherControl> controls,
        void (*onClose)(void*) = nullptr);
bool copyToClipboard(std::string_view text);
LauncherControl textControl(const char* text, int size = 0);
LauncherControl buttonControl(const char* label, void (*callback)(void*));

} // namespace dobby
#endif
