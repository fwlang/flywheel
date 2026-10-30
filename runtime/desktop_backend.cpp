// A separate shared library keeps GUI dependencies out of command-line programs.
#include <algorithm>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <webview/webview.h>

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

namespace {
void sizeWindow(webview::webview &window, int width, int height) {
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
}
// Loads the page with the working directory as its base URL, so relative resources such as
// <img src="image.jpg"> resolve as they do for the program's own file paths.
void loadPage(webview::webview &window, const char *html) {
#ifdef WEBVIEW_GTK
    std::error_code ec;
    auto directory = std::filesystem::current_path(ec);
    if (!ec) {
        auto view = window.browser_controller();
        view.ensure_ok();
        gchar *base = g_filename_to_uri((directory / "").c_str(), nullptr, nullptr);
        if (base) {
            webkit_web_view_load_html(WEBKIT_WEB_VIEW(view.value()), html, base);
            g_free(base);
            return;
        }
    }
#endif
    window.set_html(html).ensure_ok();
}
} // namespace

// Shows every window and runs one event loop until all are closed or the host asks to quit.
// Binding callbacks run on this (the calling) thread.
extern "C" FW_DESKTOP_EXPORT int fw_desktop_run_v2(const fw_desktop_window_v2 *windows, int count,
                                                   const fw_desktop_host_v2 *host, char *error,
                                                   size_t capacity) {
    try {
        std::vector<std::unique_ptr<webview::webview>> views;
        auto flush = [&] {
            for (size_t i = 0; i < views.size(); ++i)
                if (auto script = host->pending(host->context, static_cast<int>(i))) {
                    views[i]->eval(script);
                    host->release(script);
                }
            if (host->quit(host->context))
                views.front()->terminate();
        };
        for (int i = 0; i < count; ++i) {
            auto &spec = windows[i];
            views.push_back(std::make_unique<webview::webview>(false, nullptr));
            auto &view = *views.back();
            view.set_title(spec.title).ensure_ok();
            sizeWindow(view, spec.width, spec.height);
            for (int b = 0; b < spec.binding_count; ++b) {
                std::string name = spec.bindings[b];
                view.bind(
                    name,
                    [&, i, name](const std::string &id, const std::string &arguments, void *) {
                        char *result = nullptr;
                        int status = host->call(host->context, i, name.c_str(), arguments.c_str(),
                                                &result);
                        views[static_cast<size_t>(i)]->resolve(id, status == 0 ? 0 : 1,
                                                               result ? result : "null");
                        host->release(result);
                        flush();
                    },
                    nullptr)
                    .ensure_ok();
            }
            loadPage(view, spec.html);
        }
        if (!views.empty())
            views.front()->run().ensure_ok();
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
