// HTTP listeners, routing, requests and responses. Included by runtime.cpp.
//
// Field order of web.request and web.response must match the classes the compiler
// declares for the web module (src/compiler.cpp, webClasses).
namespace {
constexpr size_t maxHeaderBytes = 64 * 1024;
constexpr size_t maxBodyBytes = 8 * 1024 * 1024;
struct SocketOwner {
    Socket value = invalidSocket;
    explicit SocketOwner(Socket socket) : value(socket) {}
    SocketOwner(SocketOwner &&other) noexcept : value(std::exchange(other.value, invalidSocket)) {}
    SocketOwner(const SocketOwner &) = delete;
    ~SocketOwner() {
        if (value != invalidSocket)
            closeSocket(value);
    }
};
struct Network {
    Network() {
#ifdef _WIN32
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            fail("socket initialization failed");
#endif
    }
    ~Network() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};
using Handler0 = Object *(*)(Object *);
using Handler1 = Object *(*)(Object *, Object *);
struct HttpRoute {
    std::string method;
    std::string pattern;
    std::vector<std::string> segments;
    Object *handler = nullptr;
    int arity = 0;
    std::filesystem::path directory; // non-empty for static file routes
    HttpRoute() = default;
    HttpRoute(const HttpRoute &) = delete;
    ~HttpRoute() {
        release(handler);
    }
};
struct HttpServer {
    SocketOwner socket;
    int port = 0;
    std::vector<std::unique_ptr<HttpRoute>> routes;
    explicit HttpServer(SocketOwner &&socket, int port) : socket(std::move(socket)), port(port) {}
};
struct HttpRuntime {
    Network network;
    std::vector<std::unique_ptr<HttpServer>> servers;
    bool running = false;
};
std::unique_ptr<HttpRuntime> &httpRuntime() {
    static std::unique_ptr<HttpRuntime> state;
    return state;
}
std::vector<std::string> pathSegments(const std::string &path) {
    std::vector<std::string> out;
    size_t begin = 1;
    while (begin <= path.size()) {
        auto end = path.find('/', begin);
        if (end == std::string::npos)
            end = path.size();
        out.push_back(path.substr(begin, end - begin));
        begin = end + 1;
    }
    return out;
}
std::string percentDecode(const std::string &text, bool plus) {
    std::string out;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && digit(text[i + 1]) >= 0 &&
            digit(text[i + 2]) >= 0) {
            out += static_cast<char>(digit(text[i + 1]) * 16 + digit(text[i + 2]));
            i += 2;
        } else if (plus && text[i] == '+')
            out += ' ';
        else
            out += text[i];
    }
    return out;
}
std::string lower(std::string text) {
    for (auto &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
std::string trim(const std::string &text) {
    auto begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos)
        return "";
    return text.substr(begin, text.find_last_not_of(" \t") - begin + 1);
}
Object *stringMap(Frame *frame) {
    auto map = own(frame, new Object(Object::Map));
    map->objectKeys = true;
    map->keyKind = Slot::Reference;
    return map;
}
Slot text(Frame *frame, const std::string &value) {
    Slot slot;
    slot.kind = Slot::Reference;
    slot.object = string(frame, value);
    retain(slot.object);
    return slot;
}
void put(Frame *frame, Object *map, const std::string &key, const std::string &value,
         bool join = false) {
    auto keyObject = string(frame, key);
    auto i = findKey(map, 0, keyObject);
    if (i < 0) {
        appendKey(map, text(frame, key), text(frame, value));
        return;
    }
    auto &slot = map->slots[static_cast<size_t>(i)];
    auto previous = slot.object ? slot.object->text : "";
    slot = text(frame, join ? previous + ", " + value : value);
}
void parseForm(Frame *frame, Object *map, const std::string &text) {
    size_t begin = 0;
    while (begin <= text.size()) {
        auto end = text.find('&', begin);
        if (end == std::string::npos)
            end = text.size();
        auto pair = text.substr(begin, end - begin);
        if (!pair.empty()) {
            auto equals = pair.find('=');
            put(frame, map, percentDecode(pair.substr(0, equals), true),
                equals == std::string::npos ? "" : percentDecode(pair.substr(equals + 1), true));
        }
        begin = end + 1;
    }
}
bool matchRoute(const HttpRoute &route, const std::vector<std::string> &segments,
                std::vector<std::pair<std::string, std::string>> &params) {
    params.clear();
    auto &pattern = route.segments;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == "*" && i + 1 == pattern.size()) {
            std::string rest;
            for (size_t j = i; j < segments.size(); ++j)
                rest += (j > i ? "/" : "") + percentDecode(segments[j], false);
            params.emplace_back("*", rest);
            return true;
        }
        if (i >= segments.size())
            return false;
        if (!pattern[i].empty() && pattern[i][0] == ':') {
            if (segments[i].empty())
                return false;
            params.emplace_back(pattern[i].substr(1), percentDecode(segments[i], false));
        } else if (pattern[i] != segments[i])
            return false;
    }
    return pattern.size() == segments.size();
}
bool methodMatches(const std::string &route, const std::string &method) {
    return route == "*" || route == method || (route == "GET" && method == "HEAD");
}
std::string contentType(const std::filesystem::path &path) {
    static const std::map<std::string, std::string> types = {
        {".html", "text/html; charset=utf-8"},
        {".htm", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".mjs", "text/javascript; charset=utf-8"},
        {".json", "application/json"},
        {".txt", "text/plain; charset=utf-8"},
        {".svg", "image/svg+xml"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},
        {".webp", "image/webp"},
        {".ico", "image/x-icon"},
        {".woff", "font/woff"},
        {".woff2", "font/woff2"},
        {".wasm", "application/wasm"},
        {".pdf", "application/pdf"}};
    auto it = types.find(lower(path.extension().string()));
    return it == types.end() ? "application/octet-stream" : it->second;
}
struct HttpResponse {
    int status = 200;
    std::string body, type = "text/plain; charset=utf-8";
    std::vector<std::pair<std::string, std::string>> headers;
};
const char *reason(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 204:
        return "No Content";
    case 301:
        return "Moved Permanently";
    case 302:
        return "Found";
    case 303:
        return "See Other";
    case 304:
        return "Not Modified";
    case 307:
        return "Temporary Redirect";
    case 308:
        return "Permanent Redirect";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 413:
        return "Payload Too Large";
    case 422:
        return "Unprocessable Content";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 503:
        return "Service Unavailable";
    default:
        return "Status";
    }
}
HttpResponse plain(int status) {
    HttpResponse r;
    r.status = status;
    r.body = reason(status);
    return r;
}
bool validHeader(const std::string &text) {
    return text.find_first_of("\r\n") == std::string::npos;
}
// Converts a handler result (element, fragment, string, or web.response) to a response.
HttpResponse responseFrom(Object *result) {
    HttpResponse r;
    if (!result)
        fail("HTTP handler returned null");
    if (result->kind == Object::Element) {
        render(result, r.body);
        r.type = "text/html; charset=utf-8";
        return r;
    }
    if (result->kind == Object::String) {
        r.body = result->text;
        return r;
    }
    if (result->kind != Object::Instance || result->typeName != "web.response" ||
        result->slots.size() != 3)
        fail("HTTP handler returned an unsupported value");
    auto status = result->slots[0].integer;
    if (status < 100 || status > 599)
        fail("HTTP status " + std::to_string(status) + " is outside 100..599");
    r.status = static_cast<int>(status);
    r.body = result->slots[1].object ? result->slots[1].object->text : "";
    if (auto headers = result->slots[2].object) {
        for (size_t i = 0; i < headers->keys.size(); ++i) {
            auto name = headers->keys[i].object ? headers->keys[i].object->text : "";
            auto value = headers->slots[i].object ? headers->slots[i].object->text : "";
            if (name.empty() || !validHeader(name) || !validHeader(value) ||
                name.find(':') != std::string::npos)
                fail("invalid HTTP response header '" + name + "'");
            auto key = lower(name);
            if (key == "content-type")
                r.type = value;
            else if (key != "content-length" && key != "connection")
                r.headers.emplace_back(name, value);
        }
    }
    return r;
}
HttpResponse staticFile(const HttpRoute &route, const std::string &relative) {
    std::filesystem::path path = route.directory;
    for (auto &segment : pathSegments("/" + relative)) {
        if (segment.empty() || segment == ".")
            continue;
        if (segment == ".." || segment.find('\\') != std::string::npos ||
            segment.find('\0') != std::string::npos || segment.find(':') != std::string::npos)
            return plain(404);
        path /= std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t *>(segment.data()), segment.size()));
    }
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec))
        path /= "index.html";
    if (!std::filesystem::is_regular_file(path, ec))
        return plain(404);
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return plain(404);
    std::ostringstream out;
    out << stream.rdbuf();
    HttpResponse r;
    r.body = out.str();
    r.type = contentType(path);
    return r;
}
void sendAll(Socket client, const std::string &data) {
    size_t sent = 0;
    while (sent < data.size()) {
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        int n = static_cast<int>(
            send(client, data.data() + sent,
                 static_cast<int>(std::min<size_t>(data.size() - sent, 65536)), flags));
        if (n <= 0)
            break;
        sent += static_cast<size_t>(n);
    }
}
void sendResponse(Socket client, const HttpResponse &r, bool head) {
    std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) +
                      "\r\nContent-Type: " + r.type +
                      "\r\nContent-Length: " + std::to_string(r.body.size());
    for (auto &[name, value] : r.headers)
        out += "\r\n" + name + ": " + value;
    out += "\r\nConnection: close\r\n\r\n";
    if (!head)
        out += r.body;
    sendAll(client, out);
}
// Returns false when a handler raised an error; the error remains pending.
bool serveRequest(HttpServer &server) {
    SocketOwner connection(accept(server.socket.value, nullptr, nullptr));
    Socket client = connection.value;
    if (client == invalidSocket)
        return true;
#ifdef _WIN32
    DWORD timeout = 5000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout),
               sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout),
               sizeof(timeout));
#else
    timeval timeout{5, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
    int noSignal = 1;
    setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
#endif
#endif
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() <= maxHeaderBytes) {
        int n = static_cast<int>(recv(client, buffer, sizeof(buffer), 0));
        if (n <= 0)
            break;
        request.append(buffer, static_cast<size_t>(n));
    }
    auto headerEnd = request.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        sendResponse(client, plain(request.size() > maxHeaderBytes ? 431 : 400), false);
        return true;
    }
    auto lineEnd = request.find("\r\n");
    auto line = request.substr(0, lineEnd);
    auto first = line.find(' '), second = first == std::string::npos ? first : line.find(' ', first + 1);
    if (first == std::string::npos || second == std::string::npos) {
        sendResponse(client, plain(400), false);
        return true;
    }
    auto method = line.substr(0, first);
    auto target = line.substr(first + 1, second - first - 1);
    bool head = method == "HEAD";
    if (target.empty() || target[0] != '/') {
        sendResponse(client, plain(400), head);
        return true;
    }
    Frame frame;
    auto headers = stringMap(&frame);
    size_t length = 0;
    for (size_t p = lineEnd + 2; p < headerEnd;) {
        auto end = request.find("\r\n", p);
        auto header = request.substr(p, end - p);
        p = end + 2;
        auto colon = header.find(':');
        if (colon == std::string::npos || colon == 0) {
            sendResponse(client, plain(400), head);
            return true;
        }
        auto name = lower(header.substr(0, colon));
        auto value = trim(header.substr(colon + 1));
        put(&frame, headers, name, value, true);
        if (name == "content-length") {
            auto [last, ec] = std::from_chars(value.data(), value.data() + value.size(), length);
            if (ec != std::errc{} || last != value.data() + value.size()) {
                sendResponse(client, plain(400), head);
                return true;
            }
        }
        if (name == "transfer-encoding") {
            sendResponse(client, plain(501), head);
            return true;
        }
    }
    if (length > maxBodyBytes) {
        sendResponse(client, plain(413), head);
        return true;
    }
    std::string body = request.substr(headerEnd + 4);
    while (body.size() < length) {
        int n = static_cast<int>(recv(client, buffer, sizeof(buffer), 0));
        if (n <= 0)
            break;
        body.append(buffer, static_cast<size_t>(n));
    }
    if (body.size() < length) {
        sendResponse(client, plain(400), head);
        return true;
    }
    body.resize(length);
    std::string query;
    auto mark = target.find('?');
    if (mark != std::string::npos) {
        query = target.substr(mark + 1);
        target.resize(mark);
    }
    auto path = httpPath(target);
    auto segments = pathSegments(path);
    std::vector<std::pair<std::string, std::string>> params;
    std::string allowed;
    HttpRoute *selected = nullptr;
    for (auto &route : server.routes) {
        if (!matchRoute(*route, segments, params))
            continue;
        if (methodMatches(route->method, method)) {
            selected = route.get();
            break;
        }
        if (allowed.find(route->method) == std::string::npos)
            allowed += (allowed.empty() ? "" : ", ") + route->method;
    }
    if (!selected) {
        auto r = plain(allowed.empty() ? 404 : 405);
        if (!allowed.empty())
            r.headers.emplace_back("Allow", allowed);
        sendResponse(client, r, head);
        return true;
    }
    if (!selected->directory.empty()) {
        sendResponse(client, staticFile(*selected, params.empty() ? "" : params.back().second),
                     head);
        return true;
    }
    Object *result = nullptr;
    if (selected->arity == 0)
        result = reinterpret_cast<Handler0>(selected->handler->code)(selected->handler);
    else {
        auto object = own(&frame, new Object(Object::Instance));
        object->typeName = object->text = "web.request";
        object->fieldNames = {"method", "path", "query", "headers", "body", "params", "form"};
        auto queryMap = stringMap(&frame), paramMap = stringMap(&frame), form = stringMap(&frame);
        parseForm(&frame, queryMap, query);
        for (auto &[name, value] : params)
            put(&frame, paramMap, name, value);
        auto type = findKey(headers, 0, string(&frame, "content-type"));
        if (type >= 0 &&
            lower(headers->slots[static_cast<size_t>(type)].object->text)
                    .rfind("application/x-www-form-urlencoded", 0) == 0)
            parseForm(&frame, form, body);
        object->slots.push_back(text(&frame, method));
        object->slots.push_back(text(&frame, percentDecode(target, false)));
        for (auto map : {queryMap, headers})
            object->slots.push_back(reference(map));
        object->slots.push_back(text(&frame, body));
        for (auto map : {paramMap, form})
            object->slots.push_back(reference(map));
        result = reinterpret_cast<Handler1>(selected->handler->code)(selected->handler, object);
    }
    struct ResultOwner {
        Object *value;
        ~ResultOwner() {
            release(value);
        }
    } owned{result};
    if (pendingError.value)
        return false;
    sendResponse(client, responseFrom(result), head);
    return true;
}
HttpServer &listener(int64_t port) {
    if (port < 0 || port > 65535)
        fail("HTTP port must be in 0..65535");
    auto &storage = httpRuntime();
    if (!storage)
        storage = std::make_unique<HttpRuntime>();
    auto &state = *storage;
    if (port != 0)
        for (auto &server : state.servers)
            if (server->port == port)
                return *server;
    if (state.servers.size() >= FD_SETSIZE)
        fail("too many HTTP listeners");
    SocketOwner socketOwner(socket(AF_INET, SOCK_STREAM, 0));
    auto socket = socketOwner.value;
    if (socket == invalidSocket)
        fail("cannot create HTTP socket");
#ifndef _WIN32
    if (socket >= FD_SETSIZE)
        fail("HTTP socket exceeds select descriptor limit");
#endif
    int reuse = 1;
#ifdef _WIN32
    setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&reuse),
               sizeof(reuse));
#else
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse),
               sizeof(reuse));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
        fail("cannot bind HTTP port " + std::to_string(port));
    if (listen(socket, 16) != 0)
        fail("cannot listen on HTTP socket");
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (getsockname(socket, reinterpret_cast<sockaddr *>(&address), &length) != 0)
        fail("cannot determine HTTP address");
    state.servers.push_back(
        std::make_unique<HttpServer>(std::move(socketOwner), ntohs(address.sin_port)));
    return *state.servers.back();
}
Object *addRoute(Frame *frame, const std::string &method, const std::string &route,
                 std::unique_ptr<HttpRoute> entry, int64_t port) {
    if (route.empty() || route[0] != '/')
        fail("HTTP route must start with '/'");
    if (method.empty() || !validHeader(method) || method.find(' ') != std::string::npos)
        fail("invalid HTTP method '" + method + "'");
    auto encoded = httpPath(route);
    entry->method = method;
    entry->pattern = encoded;
    entry->segments = pathSegments(encoded);
    for (size_t i = 0; i < entry->segments.size(); ++i)
        if (entry->segments[i] == "*" && i + 1 != entry->segments.size())
            fail("'*' is only allowed as the last HTTP route segment");
    auto &server = listener(port);
    for (auto &existing : server.routes)
        if (existing->pattern == encoded && existing->method == method)
            fail("duplicate HTTP route " + method + " " + route);
    std::string base = encoded;
    server.routes.push_back(std::move(entry));
    return string(frame, "http://127.0.0.1:" + std::to_string(server.port) + base);
}
} // namespace
extern "C" void fw_web_shutdown() {
    if (auto &state = httpRuntime())
        state->servers.clear();
}
extern "C" Object *fw_serve(Frame *frame, Object *method, Object *route, Object *handler,
                            int64_t arity, int64_t port) {
    return nativeBoundary([&] {
        auto entry = std::make_unique<HttpRoute>();
        retain(handler);
        entry->handler = handler;
        entry->arity = static_cast<int>(arity);
        return addRoute(frame, method->text, route->text, std::move(entry), port);
    });
}
extern "C" Object *fw_serve_static(Frame *frame, Object *route, Object *directory, int64_t port) {
    return nativeBoundary([&] {
        std::filesystem::path root(std::u8string(
            reinterpret_cast<const char8_t *>(directory->text.data()), directory->text.size()));
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec))
            fail("static directory '" + directory->text + "' does not exist", "FileError");
        auto entry = std::make_unique<HttpRoute>();
        entry->directory = std::filesystem::absolute(root);
        auto prefix = route->text;
        if (prefix.empty() || prefix.back() != '/')
            prefix += '/';
        auto url = addRoute(frame, "GET", prefix + "*", std::move(entry), port);
        url->text.resize(url->text.size() - 1);
        return url;
    });
}
extern "C" void fw_web_wait() {
    bool entered = false;
    try {
        auto &storage = httpRuntime();
        if (!storage || storage->servers.empty())
            return;
        auto &state = *storage;
        if (state.running)
            fail("web.wait cannot be called from an HTTP handler");
        struct Running {
            bool &value;
            explicit Running(bool &value) : value(value) {
                value = true;
            }
            ~Running() {
                value = false;
            }
        } running(state.running);
        entered = true;
        std::cout.flush();
        while (!state.servers.empty()) {
            fd_set readable;
            FD_ZERO(&readable);
            Socket maximum = 0;
            for (auto &server : state.servers) {
                FD_SET(server->socket.value, &readable);
                if (server->socket.value > maximum)
                    maximum = server->socket.value;
            }
#ifdef _WIN32
            int ready = select(0, &readable, nullptr, nullptr, nullptr);
#else
            int ready = select(maximum + 1, &readable, nullptr, nullptr, nullptr);
#endif
            if (ready < 0) {
#ifdef _WIN32
                if (WSAGetLastError() == WSAEINTR)
                    continue;
#else
                if (errno == EINTR)
                    continue;
#endif
                fail("HTTP listener wait failed");
            }
            std::vector<HttpServer *> selected;
            for (auto &server : state.servers)
                if (FD_ISSET(server->socket.value, &readable))
                    selected.push_back(server.get());
            for (auto server : selected)
                if (!serveRequest(*server)) {
                    fw_web_shutdown();
                    return;
                }
        }
    } catch (const NativeFailure &e) {
        if (entered)
            fw_web_shutdown();
        recordFailure(e.type, e.message);
    } catch (const std::exception &e) {
        if (entered)
            fw_web_shutdown();
        recordFailure("RuntimeError", e.what());
    }
}
