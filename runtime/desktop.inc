// ARC-managed window descriptions; the optional native backend is loaded only by run().
#ifndef _WIN32
#include <dlfcn.h>
#endif
#ifndef FW_DESKTOP_BACKEND
#define FW_DESKTOP_BACKEND ""
#endif

namespace {
using DesktopShow = int (*)(const char *, int, int, const char *, char *, size_t);
DesktopShow desktopBackend(const char *configuredPath) {
    const char *overridePath = std::getenv("FLYWHEEL_DESKTOP_BACKEND");
    std::string path = overridePath ? overridePath : configuredPath;
    if (path.empty() && !overridePath)
        path = FW_DESKTOP_BACKEND;
    if (path.empty())
        fail("desktop backend is unavailable; rebuild with -DFLYWHEEL_DESKTOP=ON and the native "
             "webview dependencies",
             "DesktopError");
    // GUI libraries register process-wide types/callbacks; retain loaded modules until exit.
    static std::map<std::string, DesktopShow> loaded;
    if (auto found = loaded.find(path); found != loaded.end())
        return found->second;
#ifdef _WIN32
    auto nativePath = std::filesystem::u8path(path).wstring();
    auto library = LoadLibraryW(nativePath.c_str());
    if (!library)
        fail("cannot load desktop backend '" + path + "' (Windows error " +
                 std::to_string(GetLastError()) + ")",
             "DesktopError");
    auto show = reinterpret_cast<DesktopShow>(GetProcAddress(library, "fw_desktop_show_v1"));
    if (!show) {
        FreeLibrary(library);
        fail("desktop backend does not provide fw_desktop_show_v1", "DesktopError");
    }
#else
    auto library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        const char *message = dlerror();
        fail("cannot load desktop backend '" + path +
                 "': " + (message ? message : "unknown loader error"),
             "DesktopError");
    }
    auto show = reinterpret_cast<DesktopShow>(dlsym(library, "fw_desktop_show_v1"));
    if (!show) {
        dlclose(library);
        fail("desktop backend does not provide fw_desktop_show_v1", "DesktopError");
    }
#endif
    loaded.emplace(path, show);
    return show;
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
extern "C" void fw_desktop_load(Object *window, Object *tree) {
    nativeBoundary([&] {
        if (window->desktop->running)
            fail("cannot load a window while its event loop is running", "DesktopError");
        std::string html;
        render(tree, html);
        if (html.find('\0') != std::string::npos)
            fail("window HTML cannot contain NUL bytes", "DesktopError");
        window->text = std::move(html);
    });
}
extern "C" void fw_desktop_run(Object *window, const char *backendPath) {
    nativeBoundary([&] {
        auto &state = *window->desktop;
        if (state.running)
            fail("window event loop is already running", "DesktopError");
        auto show = desktopBackend(backendPath);
        state.running = true;
        struct Reset {
            bool &running;
            ~Reset() {
                running = false;
            }
        } reset{state.running};
        std::cout.flush();
        std::cerr.flush();
        char error[2048]{};
        if (show(state.title.c_str(), state.width, state.height, window->text.c_str(), error,
                 sizeof(error)) != 0)
            fail(*error ? error : "native webview failed", "DesktopError");
    });
}
