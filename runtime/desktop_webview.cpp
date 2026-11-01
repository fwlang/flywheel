// Desktop backend for Windows, built on webview/webview (WebView2). Linux uses desktop_gtk.cpp.
#include <exception>
#include <memory>
#include <string>
#include <vector>
#include <webview/webview.h>

#include "desktop_backend.hpp"

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
            view.set_size(spec.width, spec.height, WEBVIEW_HINT_NONE).ensure_ok();
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
            view.set_html(spec.html).ensure_ok();
        }
        if (!views.empty())
            views.front()->run().ensure_ok();
        return 0;
    } catch (const std::exception &failure) {
        return fw_desktop_failure(failure.what(), error, capacity);
    } catch (...) {
        return fw_desktop_failure("", error, capacity);
    }
}
