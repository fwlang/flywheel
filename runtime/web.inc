// HTTP listeners, routing, requests and responses. Included by runtime.cpp.
//
// Field order of web.request and web.response must match the classes the compiler
// declares for the web module (src/compiler.cpp, webClasses).
#include <condition_variable>
#include <deque>
#ifndef _WIN32
#include <dlfcn.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#endif
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
// TLS uses the system OpenSSL 3 library, loaded on first use so programs without TLS have
// no dependency on it.
struct Tls {
    void *library = nullptr;
    void *(*serverMethod)() = nullptr;
    void *(*newContext)(void *) = nullptr;
    void (*freeContext)(void *) = nullptr;
    int (*useChain)(void *, const char *) = nullptr;
    int (*useKey)(void *, const char *, int) = nullptr;
    int (*checkKey)(void *) = nullptr;
    long (*contextControl)(void *, int, long, void *) = nullptr;
    void *(*newSession)(void *) = nullptr;
    void (*freeSession)(void *) = nullptr;
    int (*setDescriptor)(void *, int) = nullptr;
    int (*accept)(void *) = nullptr;
    int (*read)(void *, void *, int) = nullptr;
    int (*write)(void *, const void *, int) = nullptr;
    int (*error)(const void *, int) = nullptr;
    int (*shutdown)(void *) = nullptr;
};
Tls &tls() {
    static Tls api;
    if (api.library)
        return api;
#ifdef _WIN32
    for (auto name : {"libssl-3-x64.dll", "libssl-3.dll"})
        if ((api.library = reinterpret_cast<void *>(LoadLibraryA(name))))
            break;
    auto symbol = [&](const char *name) {
        return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(api.library), name));
    };
#else
    for (auto name : {"libssl.so.3", "libssl.3.dylib", "libssl.so"})
        if ((api.library = dlopen(name, RTLD_NOW | RTLD_LOCAL)))
            break;
    auto symbol = [&](const char *name) { return dlsym(api.library, name); };
#endif
    if (!api.library)
        fail("TLS requires the OpenSSL 3 library (libssl), which could not be loaded");
    auto bind = [&](auto &target, const char *name) {
        auto found = symbol(name);
        if (!found)
            fail(std::string("OpenSSL is missing ") + name);
        target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(found);
    };
    bind(api.serverMethod, "TLS_server_method");
    bind(api.newContext, "SSL_CTX_new");
    bind(api.freeContext, "SSL_CTX_free");
    bind(api.useChain, "SSL_CTX_use_certificate_chain_file");
    bind(api.useKey, "SSL_CTX_use_PrivateKey_file");
    bind(api.checkKey, "SSL_CTX_check_private_key");
    bind(api.contextControl, "SSL_CTX_ctrl");
    bind(api.newSession, "SSL_new");
    bind(api.freeSession, "SSL_free");
    bind(api.setDescriptor, "SSL_set_fd");
    bind(api.accept, "SSL_accept");
    bind(api.read, "SSL_read");
    bind(api.write, "SSL_write");
    bind(api.error, "SSL_get_error");
    bind(api.shutdown, "SSL_shutdown");
    return api;
}
constexpr int tlsWantRead = 2, tlsWantWrite = 3, tlsZeroReturn = 6;
struct HttpServer {
    SocketOwner socket;
    int port = 0;
    std::string host;
    void *tlsContext = nullptr;
    std::vector<std::unique_ptr<HttpRoute>> routes;
    explicit HttpServer(SocketOwner &&socket, int port, std::string host)
        : socket(std::move(socket)), port(port), host(std::move(host)) {}
    ~HttpServer() {
        if (tlsContext)
            tls().freeContext(tlsContext);
    }
};
// One accepted client. Requests on a keep-alive connection are answered in order; reads and
// writes never block, so slow clients do not stall other connections.
struct HttpConnection {
    SocketOwner socket;
    HttpServer *server;
    std::string input, output;
    void *session = nullptr;
    bool handshaken = false, closeAfterWrite = false, dead = false;
    bool busy = false; // a worker is handling this connection's current request
    std::chrono::steady_clock::time_point active = std::chrono::steady_clock::now();
    HttpConnection(SocketOwner &&socket, HttpServer *server)
        : socket(std::move(socket)), server(server) {}
    ~HttpConnection() {
        if (session) {
            tls().shutdown(session);
            tls().freeSession(session);
        }
    }
};
struct ParsedRequest;
struct HttpResponse;
// web.workers(n): handlers run on a pool of threads; each connection still answers its requests
// in order. Handler errors travel back to the serving thread and stop the server there.
struct HttpJob {
    HttpConnection *connection;
    std::shared_ptr<ParsedRequest> request;
};
struct HttpDone {
    HttpConnection *connection;
    std::shared_ptr<HttpResponse> response;
    bool head = false, keepAlive = true;
    std::string errorType;
    Object *error = nullptr;
};
struct HttpWorkers {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<HttpJob> jobs;
    std::deque<HttpDone> done;
    std::vector<std::thread> threads;
    bool stop = false;
    size_t outstanding = 0;
    ~HttpWorkers() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        wake.notify_all();
        for (auto &t : threads)
            t.join();
        for (auto &d : done)
            release(d.error);
    }
};
struct HttpRuntime {
    Network network;
    std::vector<std::unique_ptr<HttpServer>> servers;
    std::vector<std::unique_ptr<HttpConnection>> connections;
    bool running = false;
    int workerCount = 0;
    std::unique_ptr<HttpWorkers> workers;
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
void setNonBlocking(Socket socket) {
#ifdef _WIN32
    u_long yes = 1;
    ioctlsocket(socket, FIONBIO, &yes);
#else
    fcntl(socket, F_SETFL, fcntl(socket, F_GETFL, 0) | O_NONBLOCK);
#endif
}
bool wouldBlock() {
#ifdef _WIN32
    auto code = WSAGetLastError();
    return code == WSAEWOULDBLOCK || code == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}
std::string serializeResponse(const HttpResponse &r, bool head, bool keepAlive) {
    std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) +
                      "\r\nContent-Type: " + r.type +
                      "\r\nContent-Length: " + std::to_string(r.body.size());
    for (auto &[name, value] : r.headers)
        out += "\r\n" + name + ": " + value;
    out += keepAlive ? "\r\nConnection: keep-alive\r\n\r\n" : "\r\nConnection: close\r\n\r\n";
    if (!head)
        out += r.body;
    return out;
}
struct ParsedRequest {
    std::string method, target, version, body;
    std::vector<std::pair<std::string, std::string>> headers;
    bool keepAlive = true;
};
// Parses one request from the front of `input`. Returns 0 when more bytes are needed, 1 when
// `request` is complete (its bytes are consumed), or an HTTP error status.
int parseRequest(std::string &input, ParsedRequest &request) {
    auto headerEnd = input.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
        return input.size() > maxHeaderBytes ? 431 : 0;
    if (headerEnd > maxHeaderBytes)
        return 431;
    auto lineEnd = input.find("\r\n");
    auto line = input.substr(0, lineEnd);
    auto first = line.find(' '),
         second = first == std::string::npos ? first : line.find(' ', first + 1);
    if (first == std::string::npos || second == std::string::npos)
        return 400;
    request = ParsedRequest{};
    request.method = line.substr(0, first);
    request.target = line.substr(first + 1, second - first - 1);
    request.version = line.substr(second + 1);
    if (request.target.empty() || request.target[0] != '/')
        return 400;
    request.keepAlive = request.version == "HTTP/1.1";
    size_t length = 0;
    bool chunked = false;
    for (size_t p = lineEnd + 2; p < headerEnd;) {
        auto end = input.find("\r\n", p);
        auto header = input.substr(p, end - p);
        p = end + 2;
        auto colon = header.find(':');
        if (colon == std::string::npos || colon == 0)
            return 400;
        auto name = lower(header.substr(0, colon));
        auto value = trim(header.substr(colon + 1));
        request.headers.emplace_back(name, value);
        if (name == "content-length") {
            auto [last, ec] = std::from_chars(value.data(), value.data() + value.size(), length);
            if (ec != std::errc{} || last != value.data() + value.size())
                return 400;
        } else if (name == "transfer-encoding") {
            if (lower(value) != "chunked")
                return 501;
            chunked = true;
        } else if (name == "connection") {
            auto option = lower(value);
            if (option.find("close") != std::string::npos)
                request.keepAlive = false;
            else if (option.find("keep-alive") != std::string::npos)
                request.keepAlive = true;
        }
    }
    size_t position = headerEnd + 4;
    if (chunked) {
        // Each chunk is "size[;extensions]\r\ndata\r\n"; a zero size ends the body.
        std::string body;
        while (true) {
            auto sizeEnd = input.find("\r\n", position);
            if (sizeEnd == std::string::npos)
                return input.size() - position > 1024 ? 400 : 0;
            auto sizeText = input.substr(position, sizeEnd - position);
            sizeText = sizeText.substr(0, sizeText.find(';'));
            size_t size = 0;
            auto [last, ec] =
                std::from_chars(sizeText.data(), sizeText.data() + sizeText.size(), size, 16);
            if (ec != std::errc{} || last != sizeText.data() + sizeText.size() || sizeText.empty())
                return 400;
            if (body.size() + size > maxBodyBytes)
                return 413;
            if (size == 0) {
                auto trailerEnd = input.find("\r\n\r\n", sizeEnd);
                if (input.compare(sizeEnd, 4, "\r\n\r\n") == 0)
                    trailerEnd = sizeEnd;
                if (trailerEnd == std::string::npos)
                    return 0;
                position = trailerEnd + 4;
                break;
            }
            if (input.size() < sizeEnd + 2 + size + 2)
                return 0;
            body.append(input, sizeEnd + 2, size);
            if (input.compare(sizeEnd + 2 + size, 2, "\r\n") != 0)
                return 400;
            position = sizeEnd + 2 + size + 2;
        }
        request.body = std::move(body);
    } else {
        if (length > maxBodyBytes)
            return 413;
        if (input.size() < position + length)
            return 0;
        request.body = input.substr(position, length);
        position += length;
    }
    input.erase(0, position);
    return 1;
}
Object *nativeRequest(Frame *frame) {
    return nativeInstance(frame, "web.request",
                          {"method", "path", "query", "headers", "body", "params", "form"});
}
// Runs the matching route. Returns false when a handler raised an error; it remains pending.
bool handleRequest(HttpServer &server, ParsedRequest &request, HttpResponse &response) {
    bool head = request.method == "HEAD";
    (void)head;
    Frame frame;
    auto headers = stringMap(&frame);
    for (auto &[name, value] : request.headers)
        put(&frame, headers, name, value, true);
    auto target = request.target;
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
        if (methodMatches(route->method, request.method)) {
            selected = route.get();
            break;
        }
        if (allowed.find(route->method) == std::string::npos)
            allowed += (allowed.empty() ? "" : ", ") + route->method;
    }
    if (!selected) {
        response = plain(allowed.empty() ? 404 : 405);
        if (!allowed.empty())
            response.headers.emplace_back("Allow", allowed);
        return true;
    }
    if (!selected->directory.empty()) {
        response = staticFile(*selected, params.empty() ? "" : params.back().second);
        return true;
    }
    Object *result = nullptr;
    if (selected->arity == 0)
        result = reinterpret_cast<Handler0>(selected->handler->code)(selected->handler);
    else {
        auto object = nativeRequest(&frame);
        auto queryMap = stringMap(&frame), paramMap = stringMap(&frame), form = stringMap(&frame);
        parseForm(&frame, queryMap, query);
        for (auto &[name, value] : params)
            put(&frame, paramMap, name, value);
        auto type = findKey(headers, 0, string(&frame, "content-type"));
        if (type >= 0 &&
            lower(headers->slots[static_cast<size_t>(type)].object->text)
                    .rfind("application/x-www-form-urlencoded", 0) == 0)
            parseForm(&frame, form, request.body);
        object->slots.push_back(text(&frame, request.method));
        object->slots.push_back(text(&frame, percentDecode(target, false)));
        for (auto map : {queryMap, headers})
            object->slots.push_back(reference(map));
        object->slots.push_back(text(&frame, request.body));
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
    response = responseFrom(result);
    return true;
}
// Reads available bytes; returns false at end of stream or on a connection error.
bool receiveAvailable(HttpConnection &c) {
    char buffer[16384];
    while (true) {
        int n;
        if (c.session) {
            n = tls().read(c.session, buffer, sizeof(buffer));
            if (n <= 0) {
                auto code = tls().error(c.session, n);
                return code == tlsWantRead || code == tlsWantWrite;
            }
        } else {
            n = static_cast<int>(recv(c.socket.value, buffer, sizeof(buffer), 0));
            if (n < 0)
                return wouldBlock();
            if (n == 0)
                return false;
        }
        c.input.append(buffer, static_cast<size_t>(n));
        c.active = std::chrono::steady_clock::now();
        if (c.input.size() > maxHeaderBytes + maxBodyBytes + 65536)
            return true;
    }
}
bool sendPending(HttpConnection &c) {
    while (!c.output.empty()) {
        auto chunk = static_cast<int>(std::min<size_t>(c.output.size(), 65536));
        int n;
        if (c.session) {
            n = tls().write(c.session, c.output.data(), chunk);
            if (n <= 0) {
                auto code = tls().error(c.session, n);
                return code == tlsWantRead || code == tlsWantWrite;
            }
        } else {
#ifdef MSG_NOSIGNAL
            constexpr int flags = MSG_NOSIGNAL;
#else
            constexpr int flags = 0;
#endif
            n = static_cast<int>(send(c.socket.value, c.output.data(), chunk, flags));
            if (n < 0)
                return wouldBlock();
        }
        c.output.erase(0, static_cast<size_t>(n));
        c.active = std::chrono::steady_clock::now();
    }
    return true;
}
// Answers every complete request buffered on `c`. Returns false after a handler error.
bool processConnection(HttpConnection &c, HttpWorkers *workers) {
    while (!c.closeAfterWrite && !c.busy) {
        auto request = std::make_shared<ParsedRequest>();
        int status = parseRequest(c.input, *request);
        if (status == 0)
            return true;
        HttpResponse response;
        bool head = false;
        if (status != 1) {
            response = plain(status);
            c.closeAfterWrite = true;
        } else if (workers) {
            c.busy = true;
            {
                std::lock_guard<std::mutex> lock(workers->mutex);
                workers->jobs.push_back({&c, request});
                ++workers->outstanding;
            }
            workers->wake.notify_one();
            return true;
        } else {
            head = request->method == "HEAD";
            if (!handleRequest(*c.server, *request, response))
                return false;
            c.closeAfterWrite = !request->keepAlive;
        }
        c.output += serializeResponse(response, head, !c.closeAfterWrite);
    }
    return true;
}
void workerLoop(HttpWorkers &workers) {
    while (true) {
        HttpJob job;
        {
            std::unique_lock<std::mutex> lock(workers.mutex);
            workers.wake.wait(lock, [&] { return workers.stop || !workers.jobs.empty(); });
            if (workers.stop)
                return;
            job = workers.jobs.front();
            workers.jobs.pop_front();
        }
        HttpDone result{job.connection, std::make_shared<HttpResponse>()};
        result.head = job.request->method == "HEAD";
        result.keepAlive = job.request->keepAlive;
        try {
            if (!handleRequest(*job.connection->server, *job.request, *result.response)) {
                result.errorType = pendingError.type;
                result.error = pendingError.value;
                retain(result.error);
                pendingError.clear();
            }
        } catch (const std::exception &) {
            *result.response = plain(500);
            result.keepAlive = false;
        } catch (const NativeFailure &) {
            *result.response = plain(500);
            result.keepAlive = false;
        }
        std::lock_guard<std::mutex> lock(workers.mutex);
        workers.done.push_back(std::move(result));
    }
}
// Applies finished worker results. Returns false (with the error pending) after a handler error.
bool collectWorkers(HttpRuntime &state) {
    std::deque<HttpDone> finished;
    {
        std::lock_guard<std::mutex> lock(state.workers->mutex);
        finished.swap(state.workers->done);
        state.workers->outstanding -= finished.size();
    }
    bool ok = true;
    for (auto &d : finished) {
        if (d.error) {
            if (ok)
                pendingError.set(d.errorType, d.error);
            release(d.error);
            d.error = nullptr;
            ok = false;
            continue;
        }
        auto &c = *d.connection;
        c.busy = false;
        c.closeAfterWrite = !d.keepAlive;
        c.output += serializeResponse(*d.response, d.head, d.keepAlive);
    }
    return ok;
}
#ifdef _WIN32
using PollEntry = WSAPOLLFD;
int pollSockets(std::vector<PollEntry> &entries, int milliseconds) {
    return WSAPoll(entries.data(), static_cast<ULONG>(entries.size()), milliseconds);
}
#else
using PollEntry = pollfd;
int pollSockets(std::vector<PollEntry> &entries, int milliseconds) {
    return poll(entries.data(), entries.size(), milliseconds);
}
#endif
constexpr auto idleTimeout = std::chrono::seconds(15);
// Serves all listeners until they are shut down. Returns false after a handler error.
bool serveLoop(HttpRuntime &state) {
    if (state.workerCount > 1 && !state.workers) {
        state.workers = std::make_unique<HttpWorkers>();
        for (int i = 0; i < state.workerCount; ++i) {
            ++activeThreads; // also keeps the cycle collector paused while workers exist
            RaceClock start;
            raceRelease(start);
            state.workers->threads.emplace_back([&workers = *state.workers, start] {
                guardStack();
                raceAcquire(start);
                workerLoop(workers);
                --activeThreads;
            });
        }
    }
    auto workers = state.workers.get();
    while (!state.servers.empty()) {
        std::vector<PollEntry> entries;
        for (auto &server : state.servers)
            entries.push_back({server->socket.value, POLLIN, 0});
        for (auto &c : state.connections) {
            short events = c->closeAfterWrite ? 0 : POLLIN;
            if (!c->output.empty())
                events |= POLLOUT;
            entries.push_back({c->socket.value, events, 0});
        }
        int ready = pollSockets(entries, workers && workers->outstanding ? 2 : 1000);
        if (workers && !collectWorkers(state))
            return false;
        if (ready < 0) {
            if (wouldBlock())
                continue;
            fail("HTTP listener wait failed");
        }
        size_t listeners = state.servers.size();
        for (size_t i = 0; i < listeners; ++i) {
            if (!(entries[i].revents & POLLIN))
                continue;
            auto &server = *state.servers[i];
            while (true) {
                SocketOwner client(accept(server.socket.value, nullptr, nullptr));
                if (client.value == invalidSocket)
                    break;
                setNonBlocking(client.value);
#ifdef SO_NOSIGPIPE
                int noSignal = 1;
                setsockopt(client.value, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
#endif
                auto connection = std::make_unique<HttpConnection>(std::move(client), &server);
                if (server.tlsContext) {
                    connection->session = tls().newSession(server.tlsContext);
                    if (!connection->session)
                        continue;
                    tls().setDescriptor(connection->session,
                                        static_cast<int>(connection->socket.value));
                }
                state.connections.push_back(std::move(connection));
            }
        }
        auto now = std::chrono::steady_clock::now();
        for (size_t i = 0; i < state.connections.size(); ++i) {
            auto &c = *state.connections[i];
            auto events = i + listeners < entries.size() ? entries[i + listeners].revents : 0;
            if (c.session && !c.handshaken && events) {
                int result = tls().accept(c.session);
                if (result == 1)
                    c.handshaken = true;
                else {
                    auto code = tls().error(c.session, result);
                    if (code != tlsWantRead && code != tlsWantWrite)
                        c.dead = true;
                    continue;
                }
            }
            if (events & (POLLERR | POLLNVAL))
                c.dead = true;
            if (!c.dead && (events & (POLLIN | POLLHUP)) && !c.closeAfterWrite) {
                bool open = receiveAvailable(c);
                if (!processConnection(c, workers))
                    return false;
                if (!open) {
                    // The client finished sending: answer what arrived, then close.
                    c.closeAfterWrite = true;
                    c.input.clear();
                }
            }
            if (!c.dead && !c.output.empty() && !sendPending(c))
                c.dead = true;
            if (!c.dead && !c.busy && !processConnection(c, workers))
                return false; // pipelined requests waiting behind a finished worker job
            if (c.output.empty() && c.closeAfterWrite && !c.busy)
                c.dead = true;
            if (now - c.active > idleTimeout && !c.busy)
                c.dead = true;
        }
        // Connections with a request in a worker stay until the worker reports back.
        std::erase_if(state.connections, [](auto &c) { return c->dead && !c->busy; });
    }
    return true;
}
HttpServer &listener(int64_t port, const std::string &host) {
    if (port < 0 || port > 65535)
        fail("HTTP port must be in 0..65535");
    auto &storage = httpRuntime();
    if (!storage)
        storage = std::make_unique<HttpRuntime>();
    auto &state = *storage;
    if (port != 0)
        for (auto &server : state.servers)
            if (server->port == port) {
                if (server->host != host)
                    fail("HTTP port " + std::to_string(port) + " is already bound to " +
                         server->host);
                return *server;
            }
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    auto service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &found) != 0 || !found)
        fail("cannot resolve HTTP host '" + host + "'");
    std::unique_ptr<addrinfo, void (*)(addrinfo *)> addresses(found, freeaddrinfo);
    SocketOwner socketOwner(socket(found->ai_family, SOCK_STREAM, 0));
    auto socket = socketOwner.value;
    if (socket == invalidSocket)
        fail("cannot create HTTP socket");
    int reuse = 1;
#ifdef _WIN32
    setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&reuse),
               sizeof(reuse));
#else
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse),
               sizeof(reuse));
#endif
    if (bind(socket, found->ai_addr, static_cast<int>(found->ai_addrlen)) != 0)
        fail("cannot bind HTTP port " + std::to_string(port) + " on " + host);
    if (listen(socket, 128) != 0)
        fail("cannot listen on HTTP socket");
    setNonBlocking(socket);
    sockaddr_storage address{};
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (getsockname(socket, reinterpret_cast<sockaddr *>(&address), &length) != 0)
        fail("cannot determine HTTP address");
    int bound = address.ss_family == AF_INET6
                    ? ntohs(reinterpret_cast<sockaddr_in6 *>(&address)->sin6_port)
                    : ntohs(reinterpret_cast<sockaddr_in *>(&address)->sin_port);
    state.servers.push_back(std::make_unique<HttpServer>(std::move(socketOwner), bound, host));
    return *state.servers.back();
}
std::string serverUrl(const HttpServer &server) {
    auto host = server.host.find(':') != std::string::npos ? "[" + server.host + "]" : server.host;
    return std::string(server.tlsContext ? "https" : "http") + "://" + host + ":" +
           std::to_string(server.port);
}
Object *addRoute(Frame *frame, const std::string &method, const std::string &route,
                 std::unique_ptr<HttpRoute> entry, int64_t port, const std::string &host) {
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
    auto &server = listener(port, host);
    for (auto &existing : server.routes)
        if (existing->pattern == encoded && existing->method == method)
            fail("duplicate HTTP route " + method + " " + route);
    std::string base = encoded;
    server.routes.push_back(std::move(entry));
    return string(frame, serverUrl(server) + base);
}
} // namespace
extern "C" void fw_web_shutdown() {
    if (auto &state = httpRuntime()) {
        state->workers.reset(); // joins the pool before connections go away
        state->connections.clear();
        state->servers.clear();
    }
}
extern "C" void fw_web_workers(int64_t count) {
    nativeBoundary([&] {
        if (count < 0 || count > 1024)
            fail("web.workers expects 0..1024 threads");
        auto &storage = httpRuntime();
        if (!storage)
            storage = std::make_unique<HttpRuntime>();
        if (storage->running)
            fail("web.workers must be called before serving starts");
        storage->workerCount = static_cast<int>(count);
    });
}
extern "C" Object *fw_serve(Frame *frame, Object *method, Object *route, Object *handler,
                            int64_t arity, int64_t port, Object *host) {
    return nativeBoundary([&] {
        auto entry = std::make_unique<HttpRoute>();
        retain(handler);
        entry->handler = handler;
        entry->arity = static_cast<int>(arity);
        return addRoute(frame, method->text, route->text, std::move(entry), port, host->text);
    });
}
extern "C" Object *fw_serve_static(Frame *frame, Object *route, Object *directory, int64_t port,
                                   Object *host) {
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
        auto url = addRoute(frame, "GET", prefix + "*", std::move(entry), port, host->text);
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
        if (!serveLoop(state))
            fw_web_shutdown();
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
// Serves HTTPS on `port` using a PEM certificate chain and private key.
extern "C" Object *fw_web_tls(Frame *frame, Object *certificate, Object *key, int64_t port,
                              Object *host) {
    return nativeBoundary([&] {
        auto &server = listener(port, host->text);
        auto &api = tls();
        if (server.tlsContext)
            api.freeContext(server.tlsContext);
        server.tlsContext = api.newContext(api.serverMethod());
        if (!server.tlsContext)
            fail("cannot create TLS context");
        // SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER
        api.contextControl(server.tlsContext, 33, 3, nullptr);
        if (api.useChain(server.tlsContext, certificate->text.c_str()) != 1)
            fail("cannot load TLS certificate '" + certificate->text + "'", "FileError");
        if (api.useKey(server.tlsContext, key->text.c_str(), 1) != 1 ||
            api.checkKey(server.tlsContext) != 1)
            fail("cannot load TLS private key '" + key->text + "'", "FileError");
        return string(frame, serverUrl(server));
    });
}
