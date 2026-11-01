// Desktop backend for Linux/BSD, written directly against GTK and WebKitGTK (GTK 3 with
// webkit2gtk-4.0/4.1, or GTK 4 with webkitgtk-6.0); it needs no third-party wrapper library.
#include <filesystem>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <gtk/gtk.h>
#if GTK_MAJOR_VERSION >= 4
#include <webkit/webkit.h>
#else
#include <webkit2/webkit2.h>
#endif
#ifdef GDK_WINDOWING_X11
#if GTK_MAJOR_VERSION >= 4
#include <gdk/x11/gdkx.h>
#else
#include <gdk/gdkx.h>
#endif
#endif

#include "desktop_backend.hpp"

namespace {
// Page side of bindings: window[name](...) posts "id\nname\njson-arguments" and returns a promise
// that __fw_reply(id, ok, json) settles. Injected at document start before the page's scripts.
const char *bridgeScript = R"((function () {
  'use strict';
  var next = 0, waiting = {};
  window.__fw_reply = function (id, ok, text) {
    var call = waiting[id];
    if (!call) return;
    delete waiting[id];
    var value;
    try { value = JSON.parse(text); }
    catch (e) { call.reject(new Error("Failed to parse binding result as JSON")); return; }
    (ok ? call.resolve : call.reject)(value);
  };
  __FW_NAMES__.forEach(function (name) {
    window[name] = function () {
      var id = ++next, args = JSON.stringify(Array.prototype.slice.call(arguments));
      return new Promise(function (resolve, reject) {
        waiting[id] = { resolve: resolve, reject: reject };
        window.webkit.messageHandlers.__fw.postMessage(id + "\n" + name + "\n" + args);
      });
    };
  });
})();)";

std::string jsString(const std::string &text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                static const char digits[] = "0123456789abcdef";
                out += "\\u00";
                out += digits[c >> 4];
                out += digits[c & 15];
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    // U+2028/U+2029 are valid in JSON strings but historically ended JavaScript string literals.
    for (const char *separator : {"\xe2\x80\xa8", "\xe2\x80\xa9"})
        for (size_t at; (at = out.find(separator)) != std::string::npos;)
            out.replace(at, 3, separator[2] == '\xa8' ? "\\u2028" : "\\u2029");
    return out + '"';
}

struct Session;
struct View {
    Session *session;
    int index;
    GtkWidget *window = nullptr;
    WebKitWebView *web = nullptr;
};
struct Session {
    const fw_desktop_host_v2 *host;
    std::vector<std::unique_ptr<View>> views;
    int open = 0;
    bool stopped = false;
};
struct Message {
    View *view;
    std::string text;
};

void eval(View &view, const char *script) {
    if (!view.web || !webkit_web_view_get_uri(view.web))
        return;
#if WEBKIT_CHECK_VERSION(2, 40, 0)
    webkit_web_view_evaluate_javascript(view.web, script, -1, nullptr, nullptr, nullptr, nullptr,
                                        nullptr);
#else
    webkit_web_view_run_javascript(view.web, script, nullptr, nullptr, nullptr);
#endif
}

// Runs one binding call outside the WebKit signal, then pushes any re-render scripts and the reply.
gboolean deliver(gpointer data) {
    std::unique_ptr<Message> message(static_cast<Message *>(data));
    auto &view = *message->view;
    auto &session = *view.session;
    if (session.stopped)
        return G_SOURCE_REMOVE;
    auto &text = message->text;
    auto first = text.find('\n'), second = text.find('\n', first + 1);
    if (first == std::string::npos || second == std::string::npos)
        return G_SOURCE_REMOVE;
    auto id = text.substr(0, first), name = text.substr(first + 1, second - first - 1);
    char *result = nullptr;
    auto host = session.host;
    int status = host->call(host->context, view.index, name.c_str(), text.c_str() + second + 1,
                            &result);
    for (auto &other : session.views)
        if (auto script = host->pending(host->context, other->index)) {
            eval(*other, script);
            host->release(script);
        }
    auto reply = "window.__fw_reply(" + id + ", " + (status == 0 ? "true" : "false") + ", " +
                 jsString(result ? result : "null") + ")";
    host->release(result);
    eval(view, reply.c_str());
    if (host->quit(host->context))
        session.stopped = true;
    return G_SOURCE_REMOVE;
}

#if GTK_MAJOR_VERSION >= 4
void received(WebKitUserContentManager *, JSCValue *value, gpointer data) {
#else
void received(WebKitUserContentManager *, WebKitJavascriptResult *result, gpointer data) {
    JSCValue *value = webkit_javascript_result_get_js_value(result);
#endif
    char *text = jsc_value_to_string(value);
    g_idle_add_full(G_PRIORITY_HIGH_IDLE, deliver,
                    new Message{static_cast<View *>(data), text ? text : ""}, nullptr);
    g_free(text);
}

void destroyed(GtkWidget *, gpointer data) {
    auto &view = *static_cast<View *>(data);
    view.window = nullptr;
    if (--view.session->open <= 0)
        view.session->stopped = true;
}

// WebKitGTK 2.42+ renders blank windows with the NVIDIA driver on X11 unless DMA-BUF is disabled.
void avoidDmabufBug() {
#ifdef GDK_WINDOWING_X11
    if (webkit_get_major_version() != 2 || webkit_get_minor_version() < 42 ||
        g_getenv("WEBKIT_DISABLE_DMABUF_RENDERER") || g_getenv("WAYLAND_DISPLAY"))
        return;
    struct stat module{};
    if (stat("/sys/module/nvidia", &module) != 0 || !S_ISDIR(module.st_mode))
        return;
    if (GDK_IS_X11_DISPLAY(gdk_display_get_default()))
        g_setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", TRUE);
#endif
}

std::string bridgeFor(const fw_desktop_window_v2 &spec) {
    std::string names = "[";
    for (int b = 0; b < spec.binding_count; ++b)
        names += (b ? "," : "") + jsString(spec.bindings[b]);
    std::string script = bridgeScript;
    script.replace(script.find("__FW_NAMES__"), 12, names + "]");
    return script;
}

void openWindow(Session &session, const fw_desktop_window_v2 &spec, int index) {
    auto &view = *session.views.emplace_back(new View{&session, index});
#if GTK_MAJOR_VERSION >= 4
    view.window = gtk_window_new();
#else
    view.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#endif
    ++session.open;
    g_signal_connect(view.window, "destroy", G_CALLBACK(destroyed), &view);
    gtk_window_set_title(GTK_WINDOW(view.window), spec.title);
    gtk_window_set_default_size(GTK_WINDOW(view.window), spec.width, spec.height);

    view.web = WEBKIT_WEB_VIEW(webkit_web_view_new());
    g_object_ref_sink(view.web);
    auto manager = webkit_web_view_get_user_content_manager(view.web);
    g_signal_connect(manager, "script-message-received::__fw", G_CALLBACK(received), &view);
#if GTK_MAJOR_VERSION >= 4
    webkit_user_content_manager_register_script_message_handler(manager, "__fw", nullptr);
#else
    webkit_user_content_manager_register_script_message_handler(manager, "__fw");
#endif
    auto bridge = bridgeFor(spec);
    auto script = webkit_user_script_new(bridge.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                         WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr,
                                         nullptr);
    webkit_user_content_manager_add_script(manager, script);
    webkit_user_script_unref(script);
    webkit_settings_set_javascript_can_access_clipboard(webkit_web_view_get_settings(view.web),
                                                        TRUE);
#if GTK_MAJOR_VERSION >= 4
    gtk_window_set_child(GTK_WINDOW(view.window), GTK_WIDGET(view.web));
#else
    gtk_container_add(GTK_CONTAINER(view.window), GTK_WIDGET(view.web));
    gtk_widget_show(GTK_WIDGET(view.web));
#endif

    // The working directory is the base URL, so relative resources such as <img src="image.jpg">
    // resolve as they do for the program's own file paths.
    gchar *base = nullptr;
    std::error_code ec;
    auto directory = std::filesystem::current_path(ec);
    if (!ec)
        base = g_filename_to_uri((directory / "").c_str(), nullptr, nullptr);
    webkit_web_view_load_html(view.web, spec.html, base);
    g_free(base);

    gtk_widget_grab_focus(GTK_WIDGET(view.web));
#if GTK_MAJOR_VERSION >= 4
    gtk_window_present(GTK_WINDOW(view.window));
#else
    gtk_widget_show(view.window);
#endif
}

// Closes what is still open and lets GTK finish the resulting events before views are freed.
void closeAll(Session &session) {
    session.stopped = true;
    for (auto &view : session.views) {
        if (view->window) {
            g_signal_handlers_disconnect_by_data(view->window, view.get());
#if GTK_MAJOR_VERSION >= 4
            gtk_window_destroy(GTK_WINDOW(view->window));
#else
            gtk_widget_destroy(view->window);
#endif
            view->window = nullptr;
        }
        if (view->web) {
            g_signal_handlers_disconnect_by_data(
                webkit_web_view_get_user_content_manager(view->web), view.get());
            g_object_unref(view->web);
            view->web = nullptr;
        }
    }
    bool drained = false;
    g_idle_add_full(
        G_PRIORITY_LOW,
        [](gpointer flag) -> gboolean {
            *static_cast<bool *>(flag) = true;
            return G_SOURCE_REMOVE;
        },
        &drained, nullptr);
    while (!drained)
        g_main_context_iteration(nullptr, TRUE);
}
} // namespace

// Shows every window and runs one event loop until all are closed or the host asks to quit.
// Binding callbacks run on this (the calling) thread.
extern "C" FW_DESKTOP_EXPORT int fw_desktop_run_v2(const fw_desktop_window_v2 *windows, int count,
                                                   const fw_desktop_host_v2 *host, char *error,
                                                   size_t capacity) {
#if GTK_MAJOR_VERSION >= 4
    if (!gtk_init_check())
#else
    if (!gtk_init_check(nullptr, nullptr))
#endif
        return fw_desktop_failure("GTK init failed", error, capacity);
    avoidDmabufBug();
    Session session{host, {}};
    for (int i = 0; i < count; ++i)
        openWindow(session, windows[i], i);
    while (!session.stopped && session.open > 0)
        g_main_context_iteration(nullptr, TRUE);
    closeAll(session);
    return 0;
}
