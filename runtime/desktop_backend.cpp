// A separate shared library keeps GUI dependencies out of command-line programs.
#include <algorithm>
#include <cstring>
#include <exception>
#include <webview/webview.h>

#ifdef _WIN32
#define FW_DESKTOP_EXPORT __declspec(dllexport)
#else
#define FW_DESKTOP_EXPORT __attribute__((visibility("default")))
#endif

extern "C" FW_DESKTOP_EXPORT int fw_desktop_show_v1(const char *title, int width, int height,
                                                    const char *html, char *error,
                                                    size_t capacity) {
    try {
        webview::webview window(false, nullptr);
        window.set_title(title).ensure_ok();
        auto size = window.set_size(width, height, WEBVIEW_HINT_NONE);
#if defined(WEBVIEW_GTK) && WEBVIEW_VERSION_MAJOR == 0 && WEBVIEW_VERSION_MINOR == 12 &&           \
    WEBVIEW_VERSION_PATCH == 0
        // Upstream 0.12.0 applies valid GTK sizes, then unconditionally reports this error.
        // Ignore only that exact false error; all other operations/errors remain checked.
        if (!(size.has_error() && size.error().code() == WEBVIEW_ERROR_INVALID_ARGUMENT &&
              size.error().message() == "Invalid hint"))
            size.ensure_ok();
#else
        size.ensure_ok();
#endif
        window.set_html(html).ensure_ok();
        window.run().ensure_ok();
        return 0;
    } catch (const std::exception &failure) {
        if (capacity) {
            auto length = std::min(std::strlen(failure.what()), capacity - 1);
            std::memcpy(error, failure.what(), length);
            error[length] = '\0';
        }
        return 1;
    } catch (...) {
        if (capacity)
            error[0] = '\0';
        return 1;
    }
}
