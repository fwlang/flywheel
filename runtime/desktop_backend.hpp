// Shared by the desktop backends (desktop_gtk.cpp, desktop_webview.cpp). A separate shared
// library keeps GUI dependencies out of command-line programs.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstring>

#ifdef _WIN32
#define FW_DESKTOP_EXPORT __declspec(dllexport)
#else
#define FW_DESKTOP_EXPORT __attribute__((visibility("default")))
#endif

// Backend ABI v2; must match runtime/desktop.inc.
struct fw_desktop_window_v2 {
    const char *title;
    int width, height;
    const char *html;
    const char *const *bindings;
    int binding_count;
};
struct fw_desktop_host_v2 {
    void *context;
    int (*call)(void *context, int window, const char *name, const char *arguments,
                char **result);
    char *(*pending)(void *context, int window);
    int (*quit)(void *context);
    void (*release)(char *text);
};

// Copies `message` into the caller's error buffer and returns the failure status.
inline int fw_desktop_failure(const char *message, char *error, size_t capacity) {
    if (capacity) {
        // Parenthesized so <windows.h>'s min macro (pulled in by webview.h) cannot expand here.
        auto length = (std::min)(std::strlen(message), capacity - 1);
        std::memcpy(error, message, length);
        error[length] = '\0';
    }
    return 1;
}
