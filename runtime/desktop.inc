// ARC-managed windows; the optional native backend is loaded only by run(). Windows keep their
// loaded tree so event handlers and JavaScript bindings can run Flywheel code and the page is
// re-rendered after every callback.
#ifndef _WIN32
#include <dlfcn.h>
#endif
#ifndef FW_DESKTOP_BACKEND
#define FW_DESKTOP_BACKEND ""
#endif

// Backend ABI v2, shared with runtime/desktop_backend.cpp.
struct fw_desktop_window_v2 {
    const char *title;
    int width, height;
    const char *html;
    const char *const *bindings;
    int binding_count;
};
struct fw_desktop_host_v2 {
    void *context;
    // Runs binding `name` of window `window` with the JSON argument array. Stores a JSON result
    // (or an error message) in *result; returns 0 on success, 1 for a JavaScript-visible
    // failure, and 2 when the program must stop (an uncaught Flywheel error).
    int (*call)(void *context, int window, const char *name, const char *arguments,
                char **result);
    // Script to evaluate in `window` now, or null.
    char *(*pending)(void *context, int window);
    // Nonzero once desktop.quit() has been called.
    int (*quit)(void *context);
    void (*release)(char *text);
};
using DesktopRun = int (*)(const fw_desktop_window_v2 *, int, const fw_desktop_host_v2 *, char *,
                           size_t);

namespace {
DesktopRun desktopBackend(const char *configuredPath) {
    const char *overridePath = std::getenv("FLYWHEEL_DESKTOP_BACKEND");
    std::string path = overridePath ? overridePath : configuredPath;
    if (path.empty() && !overridePath)
        path = FW_DESKTOP_BACKEND;
    if (path.empty())
        fail("desktop backend is unavailable; rebuild with -DFLYWHEEL_DESKTOP=ON and the native "
             "webview dependencies",
             "DesktopError");
    // GUI libraries register process-wide types/callbacks; retain loaded modules until exit.
    static std::map<std::string, DesktopRun> loaded;
    if (auto found = loaded.find(path); found != loaded.end())
        return found->second;
#ifdef _WIN32
    auto nativePath = std::filesystem::u8path(path).wstring();
    auto library = LoadLibraryW(nativePath.c_str());
    if (!library)
        fail("cannot load desktop backend '" + path + "' (Windows error " +
                 std::to_string(GetLastError()) + ")",
             "DesktopError");
    auto run = reinterpret_cast<DesktopRun>(GetProcAddress(library, "fw_desktop_run_v2"));
    if (!run) {
        FreeLibrary(library);
        fail("desktop backend does not provide fw_desktop_run_v2", "DesktopError");
    }
#else
    auto library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        const char *message = dlerror();
        fail("cannot load desktop backend '" + path +
                 "': " + (message ? message : "unknown loader error"),
             "DesktopError");
    }
    auto run = reinterpret_cast<DesktopRun>(dlsym(library, "fw_desktop_run_v2"));
    if (!run) {
        dlclose(library);
        fail("desktop backend does not provide fw_desktop_run_v2", "DesktopError");
    }
#endif
    loaded.emplace(path, run);
    return run;
}
DesktopState &desktopOf(Object *window) {
    if (!window || window->kind != Object::DesktopWindow || !window->desktop)
        fail("expected desktop.window", "DesktopError");
    return *window->desktop;
}
// Page helpers: __fw_event forwards an element event (with the target's value) to Flywheel,
// and __fw_render replaces the body while keeping the focused element and its selection.
const char *desktopScript = R"(<script>
window.__fw_dispatch = function (index, event) {
  var target = event && event.target;
  var value = target && target.value !== undefined ? String(target.value) : "";
  window.__fw_event(index, value);
};
window.__fw_render = function (html) {
  var active = document.activeElement, id = active && active.id;
  var start = active && active.selectionStart, end = active && active.selectionEnd;
  document.body.innerHTML = html;
  if (id) {
    var again = document.getElementById(id);
    if (again) {
      again.focus();
      try { again.setSelectionRange(start, end); } catch (e) {}
    }
  }
};
</script>)";
// Renders like render() but turns event handlers into calls of __fw_dispatch.
void renderDesktop(Object *p, std::string &out, DesktopState &state) {
    if (!p)
        return;
    if (p->kind == Object::String) {
        out += escape(p->text);
        return;
    }
    if (p->kind != Object::Element)
        fail("expected markup node");
    bool fragment = p->text.empty();
    if (!fragment) {
        out += '<' + p->text;
        for (auto &[name, value] : p->attributes)
            out += ' ' + name + "=\"" + escape(value) + '"';
        for (auto &event : p->eventList()) {
            if (!event.handler.object)
                continue;
            retain(event.handler.object);
            state.handlers.emplace_back(event.handler.object, event.arity);
            out += ' ' + event.name + "=\"__fw_dispatch(" + std::to_string(state.handlers.size() - 1) +
                   ", event)\"";
        }
        out += '>';
    }
    for (auto &c : p->slots)
        renderDesktop(c.object, out, state);
    static const std::string voids =
        " area base br col embed hr img input link meta param source track wbr ";
    if (!fragment && voids.find(" " + p->text + " ") == std::string::npos)
        out += "</" + p->text + '>';
}
Object *findBody(Object *p) {
    if (!p || p->kind != Object::Element)
        return nullptr;
    if (p->text == "body")
        return p;
    for (auto &c : p->slots)
        if (auto found = findBody(c.object))
            return found;
    return nullptr;
}
// The document around the body, with `body` as the body's content. Only body content carries
// event handlers.
void renderDocument(Object *p, std::string &out, const std::string &body) {
    if (!p || p->kind != Object::Element) {
        render(p, out);
        return;
    }
    if (p->text.empty()) {
        for (auto &c : p->slots)
            renderDocument(c.object, out, body);
        return;
    }
    out += '<' + p->text;
    for (auto &[name, value] : p->attributes)
        out += ' ' + name + "=\"" + escape(value) + '"';
    out += '>';
    static const std::string voids =
        " area base br col embed hr img input link meta param source track wbr ";
    if (voids.find(" " + p->text + " ") != std::string::npos)
        return;
    if (p->text == "body")
        out += body;
    else
        for (auto &c : p->slots)
            renderDocument(c.object, out, body);
    out += "</" + p->text + '>';
}
// The body content of the window's tree, rebuilding the handler table.
std::string renderBody(DesktopState &state) {
    for (auto &[closure, arity] : state.handlers)
        release(closure);
    state.handlers.clear();
    std::string out;
    auto body = findBody(state.tree);
    if (body)
        for (auto &c : body->slots)
            renderDesktop(c.object, out, state);
    else
        renderDesktop(state.tree, out, state);
    if (out.find('\0') != std::string::npos)
        fail("window HTML cannot contain NUL bytes", "DesktopError");
    return out;
}
struct DesktopSession {
    std::vector<Object *> windows;
    bool quit = false, failed = false;
    std::string failure;
};
DesktopSession *&activeSession() {
    static DesktopSession *session = nullptr;
    return session;
}
char *copyText(const std::string &text) {
    auto out = static_cast<char *>(std::malloc(text.size() + 1));
    if (out)
        std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}
// After any callback, windows whose rendering changed receive the new body.
void refreshWindows(DesktopSession &session) {
    for (auto window : session.windows) {
        auto &state = *window->desktop;
        if (!state.tree)
            continue;
        auto body = renderBody(state);
        if (body != state.rendered) {
            state.rendered = body;
            state.pending += "window.__fw_render(" + jsonQuote(body) + ");";
        }
    }
}
int desktopCall(void *context, int index, const char *name, const char *arguments, char **result) {
    auto &session = *static_cast<DesktopSession *>(context);
    *result = nullptr;
    try {
        auto &state = *session.windows.at(static_cast<size_t>(index))->desktop;
        auto json = JsonParser(arguments).parse();
        Frame frame;
        std::string reply = "null";
        if (std::string(name) == "__fw_event") {
            if (json->kind != Json::List || json->items.size() != 2)
                fail("invalid desktop event", "DesktopError");
            size_t handler = std::stoul(json->items[0]->text);
            if (handler >= state.handlers.size())
                return 0; // an event from an earlier rendering
            auto [closure, arity] = state.handlers[handler];
            retain(closure);
            struct Hold {
                Object *value;
                ~Hold() {
                    release(value);
                }
            } hold{closure};
            if (arity == 0)
                reinterpret_cast<void (*)(Object *)>(closure->code)(closure);
            else
                reinterpret_cast<void (*)(Object *, Object *)>(closure->code)(
                    closure, string(&frame, json->items[1]->text));
        } else {
            auto binding = std::find_if(state.bindings.begin(), state.bindings.end(),
                                        [&](auto &entry) { return entry.first == name; });
            if (binding == state.bindings.end())
                fail(std::string("no binding named ") + name, "DesktopError");
            // One string argument arrives as that string; anything else as its JSON text.
            std::string argument = json->kind == Json::List && json->items.size() == 1 &&
                                           json->items[0]->kind == Json::Text
                                       ? json->items[0]->text
                                       : jsonRender(*json);
            auto closure = binding->second;
            auto value = reinterpret_cast<Object *(*)(Object *, Object *)>(closure->code)(
                closure, string(&frame, argument));
            if (!pendingError.value) {
                reply = jsonQuote(value ? value->text : "");
                release(value);
            }
        }
        if (pendingError.value) {
            session.failed = true;
            *result = copyText("Flywheel error");
            return 2;
        }
        refreshWindows(session);
        *result = copyText(reply);
        return 0;
    } catch (const NativeFailure &e) {
        recordFailure(e.type, e.message);
    } catch (const std::exception &e) {
        recordFailure("RuntimeError", e.what());
    }
    session.failed = true;
    *result = copyText("Flywheel error");
    return 2;
}
char *desktopPending(void *context, int index) {
    auto &session = *static_cast<DesktopSession *>(context);
    auto &state = *session.windows.at(static_cast<size_t>(index))->desktop;
    if (state.pending.empty())
        return nullptr;
    auto out = copyText(state.pending);
    state.pending.clear();
    return out;
}
int desktopQuit(void *context) {
    auto &session = *static_cast<DesktopSession *>(context);
    return session.quit || session.failed;
}
void desktopRelease(char *text) {
    std::free(text);
}
void runWindows(const std::vector<Object *> &windows, const char *backendPath) {
    if (windows.empty())
        fail("desktop.run needs at least one window", "DesktopError");
    if (activeSession())
        fail("a desktop event loop is already running", "DesktopError");
    for (auto window : windows)
        if (desktopOf(window).running)
            fail("window event loop is already running", "DesktopError");
    auto run = desktopBackend(backendPath);
    DesktopSession session;
    session.windows = windows;
    std::vector<std::string> pages;
    std::vector<std::vector<const char *>> names;
    std::vector<fw_desktop_window_v2> specs;
    for (auto window : windows) {
        auto &state = *window->desktop;
        retain(window);
        state.running = true;
        state.rendered = state.tree ? renderBody(state) : window->text;
        auto scripts = std::string(desktopScript) +
                       (state.pending.empty() ? "" : "<script>" + state.pending + "</script>");
        std::string page;
        if (state.tree && findBody(state.tree))
            renderDocument(state.tree, page, state.rendered + scripts);
        else
            page = state.rendered + scripts;
        state.pending.clear();
        pages.push_back(std::move(page));
        std::vector<const char *> list{"__fw_event"};
        for (auto &[name, closure] : state.bindings)
            list.push_back(name.c_str());
        names.push_back(std::move(list));
    }
    for (size_t i = 0; i < windows.size(); ++i) {
        auto &state = *windows[i]->desktop;
        specs.push_back({state.title.c_str(), state.width, state.height, pages[i].c_str(),
                         names[i].data(), static_cast<int>(names[i].size())});
    }
    struct Reset {
        DesktopSession &session;
        ~Reset() {
            activeSession() = nullptr;
            for (auto window : session.windows) {
                window->desktop->running = false;
                release(window);
            }
        }
    } reset{session};
    activeSession() = &session;
    fw_desktop_host_v2 host{&session, desktopCall, desktopPending, desktopQuit, desktopRelease};
    std::cout.flush();
    std::cerr.flush();
    char error[2048]{};
    int status = run(specs.data(), static_cast<int>(specs.size()), &host, error, sizeof(error));
    if (session.failed) {
        if (!pendingError.value)
            fail("desktop callback failed", "DesktopError");
        return; // the callback's error stays pending for the caller
    }
    if (status != 0)
        fail(*error ? error : "native webview failed", "DesktopError");
}
} // namespace

extern "C" Object *fw_desktop_window(Frame *frame, Object *title, int64_t width, int64_t height) {
    return nativeBoundary([&] {
        if (width <= 0 || height <= 0 || width > std::numeric_limits<int>::max() ||
            height > std::numeric_limits<int>::max())
            fail("window dimensions must be positive 32-bit integers", "DesktopError");
        if (title->text.find('\0') != std::string::npos)
            fail("window title cannot contain NUL bytes", "DesktopError");
        auto object = managed(frame, Object::DesktopWindow);
        object->desktop = std::make_unique<DesktopState>();
        object->desktop->title = title->text;
        object->desktop->width = static_cast<int>(width);
        object->desktop->height = static_cast<int>(height);
        return object;
    });
}
// Keeps the tree; while running, the page shows the new tree after the current callback.
extern "C" void fw_desktop_load(Object *window, Object *tree) {
    nativeBoundary([&] {
        auto &state = desktopOf(window);
        std::string html;
        render(tree, html);
        if (html.find('\0') != std::string::npos)
            fail("window HTML cannot contain NUL bytes", "DesktopError");
        retain(tree);
        release(state.tree);
        state.tree = tree;
        window->text = std::move(html);
    });
}
extern "C" void fw_desktop_bind(Object *window, Object *name, Object *closure) {
    nativeBoundary([&] {
        auto &state = desktopOf(window);
        if (state.running)
            fail("bindings must be added before the window runs", "DesktopError");
        auto &text = name->text;
        if (text.empty() || text.rfind("__fw", 0) == 0 ||
            !std::all_of(text.begin(), text.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '_' || c == '$';
            }) ||
            std::isdigit(static_cast<unsigned char>(text[0])))
            fail("binding name must be a JavaScript identifier", "DesktopError");
        for (auto &[existing, old] : state.bindings)
            if (existing == text)
                fail("duplicate binding '" + text + "'", "DesktopError");
        retain(closure);
        state.bindings.emplace_back(text, closure);
    });
}
// Runs JavaScript in the page: after loading when called before run, else after the callback.
extern "C" void fw_desktop_eval(Object *window, Object *script) {
    nativeBoundary([&] {
        auto &state = desktopOf(window);
        state.pending += script->text;
        state.pending += ";\n";
    });
}
extern "C" void fw_desktop_quit() {
    nativeBoundary([&] {
        if (!activeSession())
            fail("desktop.quit requires a running window", "DesktopError");
        activeSession()->quit = true;
    });
}
extern "C" void fw_desktop_run(Object *window, const char *backendPath) {
    nativeBoundary([&] { runWindows({window}, backendPath); });
}
extern "C" void fw_desktop_run_all(Object *windows, const char *backendPath) {
    nativeBoundary([&] {
        std::vector<Object *> list;
        for (auto &slot : windows->slots)
            list.push_back(slot.object);
        runWindows(list, backendPath);
    });
}
