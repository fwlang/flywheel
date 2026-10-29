// Native fs, time, regex, io, json, os process and net modules. Included by runtime.cpp after
// web.inc (sockets) and process.inc (process argument helpers).
#include <ctime>
#include <regex>
#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <netdb.h>
#endif

namespace {
Slot integerSlot(int64_t value) {
    Slot slot;
    slot.integer = value;
    return slot;
}
Slot boolSlot(bool value) {
    Slot slot;
    slot.kind = Slot::Boolean;
    slot.integer = value;
    return slot;
}
Slot textSlot(Frame *frame, const std::string &value) {
    return reference(string(frame, value));
}
const std::string &instanceText(Object *instance, size_t field) {
    static const std::string empty;
    auto &slot = instance->slots.at(field);
    return slot.object ? slot.object->text : empty;
}

// ---------------------------------------------------------------------------------------------
// fs
std::filesystem::path fsPath(Object *text) {
    auto &s = requireString(text)->text;
    if (s.find('\0') != std::string::npos)
        fail("paths cannot contain NUL bytes", "FileError");
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(s.data()), s.size()));
}
std::string fsText(const std::filesystem::path &path) {
    auto text = path.generic_u8string();
    return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}
[[noreturn]] void fsFail(const std::string &what, const std::filesystem::path &path,
                         const std::error_code &ec) {
    fail(what + " '" + fsText(path) + "': " + ec.message(), "FileError");
}

// ---------------------------------------------------------------------------------------------
// time
std::tm calendar(double seconds, bool local) {
    auto whole = static_cast<std::time_t>(std::floor(seconds));
    std::tm out{};
#ifdef _WIN32
    if ((local ? localtime_s(&out, &whole) : gmtime_s(&out, &whole)) != 0)
        fail("time is out of range", "RuntimeError");
#else
    if (!(local ? localtime_r(&whole, &out) : gmtime_r(&whole, &out)))
        fail("time is out of range", "RuntimeError");
#endif
    return out;
}
double fromCalendar(std::tm &parts) {
#ifdef _WIN32
    auto value = _mkgmtime(&parts);
#else
    auto value = timegm(&parts);
#endif
    if (value == static_cast<std::time_t>(-1) && parts.tm_year != 69)
        fail("date is out of range", "RuntimeError");
    return static_cast<double>(value);
}

// ---------------------------------------------------------------------------------------------
// regex (ECMAScript syntax over UTF-8 bytes)
const std::regex &compiledRegex(const std::string &pattern) {
    thread_local std::map<std::string, std::regex> cache;
    auto it = cache.find(pattern);
    if (it != cache.end())
        return it->second;
    if (cache.size() > 128)
        cache.clear();
    try {
        return cache.emplace(pattern, std::regex(pattern, std::regex::ECMAScript)).first->second;
    } catch (const std::regex_error &error) {
        fail("invalid regular expression '" + pattern + "': " + error.what(), "RegexError");
    }
}
template <class F> auto regexBoundary(F &&function) -> decltype(function()) {
    return nativeBoundary([&] {
        try {
            return function();
        } catch (const std::regex_error &error) {
            fail(std::string("regular expression failed: ") + error.what(), "RegexError");
        }
    });
}

// ---------------------------------------------------------------------------------------------
// Native resources (files, sockets) owned by managed Resource objects.
struct FileResource : NativeResource {
    std::fstream stream;
    std::string path;
    bool open = false;
};
FileResource &fileOf(Object *p) {
    auto file = p && p->kind == Object::Resource ? dynamic_cast<FileResource *>(p->nativeResource())
                                                 : nullptr;
    if (!file)
        fail("expected io.file", "FileError");
    if (!file->open)
        fail("file '" + file->path + "' is closed", "FileError");
    return *file;
}

// ---------------------------------------------------------------------------------------------
// json encoding of arbitrary Flywheel values
void encodeSlot(const Slot &slot, std::string &out, int indent, int depth,
                std::set<Object *> &active);
void encodeNewline(std::string &out, int indent, int depth) {
    if (indent > 0) {
        out += '\n';
        out.append(static_cast<size_t>(indent * depth), ' ');
    }
}
void encodeObject(Object *object, std::string &out, int indent, int depth,
                  std::set<Object *> &active) {
    if (!object) {
        out += "null";
        return;
    }
    if (depth > 256)
        fail("JSON nesting exceeds 256", "JsonError");
    switch (object->kind) {
    case Object::String:
        out += jsonQuote(object->text);
        return;
    case Object::JsonValue:
        out += jsonRender(*object->json);
        return;
    case Object::Box:
        encodeSlot(object->slots.at(0), out, indent, depth, active);
        return;
    default:
        break;
    }
    if (object->kind == Object::Element) {
        std::string html;
        render(object, html);
        out += jsonQuote(html);
        return;
    }
    if (object->kind == Object::Closure || object->kind == Object::Weak ||
        object->kind == Object::Resource || object->kind == Object::DesktopWindow)
        fail("this value cannot be encoded as JSON", "JsonError");
    if (!active.insert(object).second)
        fail("cannot encode a reference cycle as JSON", "JsonError");
    struct Remove {
        std::set<Object *> &values;
        Object *value;
        ~Remove() {
            values.erase(value);
        }
    } remove{active, object};
    auto list = [&](const std::vector<Slot> &items) {
        out += '[';
        for (size_t i = 0; i < items.size(); ++i) {
            out += i ? "," : "";
            encodeNewline(out, indent, depth + 1);
            encodeSlot(items[i], out, indent, depth + 1, active);
        }
        if (!items.empty())
            encodeNewline(out, indent, depth);
        out += ']';
    };
    auto field = [&](size_t i, const std::string &name, const Slot &value) {
        out += i ? "," : "";
        encodeNewline(out, indent, depth + 1);
        out += jsonQuote(name) + (indent > 0 ? ": " : ":");
        encodeSlot(value, out, indent, depth + 1, active);
    };
    switch (object->kind) {
    case Object::Array:
    case Object::Tuple:
        list(object->slots);
        return;
    case Object::Set:
        list(object->keys);
        return;
    case Object::Map:
        out += '{';
        for (size_t i = 0; i < object->keys.size(); ++i) {
            auto &key = object->keys[i];
            auto name = key.kind == Slot::Reference && key.object &&
                                key.object->kind == Object::String
                            ? key.object->text
                            : formatValue(key);
            field(i, name, object->slots[i]);
        }
        if (!object->keys.empty())
            encodeNewline(out, indent, depth);
        out += '}';
        return;
    case Object::Instance:
        out += '{';
        for (size_t i = 0; i < object->slots.size(); ++i)
            field(i, i < object->fieldNames.size() ? object->fieldNames[i] : std::to_string(i),
                  object->slots[i]);
        if (!object->slots.empty())
            encodeNewline(out, indent, depth);
        out += '}';
        return;
    case Object::Result:
        out += '{';
        field(0, object->slots[0].integer ? "ok" : "error", object->slots[1]);
        encodeNewline(out, indent, depth);
        out += '}';
        return;
    case Object::ErrorValue:
        out += "{" + jsonQuote("type") + ":" + jsonQuote(readableType(object->errorType)) + "," +
               jsonQuote("message") + ":" + jsonQuote(object->text) + "}";
        return;
    default:
        fail("this value cannot be encoded as JSON", "JsonError");
    }
}
void encodeSlot(const Slot &slot, std::string &out, int indent, int depth,
                std::set<Object *> &active) {
    switch (slot.kind) {
    case Slot::Reference:
        encodeObject(slot.object, out, indent, depth, active);
        return;
    case Slot::Real:
        if (!std::isfinite(slot.real))
            fail("JSON cannot represent NaN or infinity", "JsonError");
        [[fallthrough]];
    case Slot::Integer:
    case Slot::Unsigned:
    case Slot::Boolean:
        out += formatValue(slot);
        return;
    case Slot::Char:
    case Slot::Enum:
        out += jsonQuote(formatValue(slot));
        return;
    }
}

// ---------------------------------------------------------------------------------------------
// os: arguments and child processes with captured output
std::vector<std::string> &programArguments() {
    static std::vector<std::string> arguments;
    return arguments;
}
struct ProcessRequest {
    std::vector<std::vector<std::string>> stages; // more than one stage forms a pipeline
    std::string cwd, input;
    std::vector<std::pair<std::string, std::string>> env;
    bool inheritEnv = true, capture = true;
    double timeout = 0;
};
struct ProcessOutcome {
    int64_t status = 0;
    std::string out, err;
    bool timedOut = false;
};
std::vector<std::string> childEnvironment(const ProcessRequest &request) {
    std::map<std::string, std::string> merged;
    if (request.inheritEnv) {
#ifdef _WIN32
        if (auto block = GetEnvironmentStringsW()) {
            for (auto p = block; *p; p += wcslen(p) + 1) {
                std::wstring entry = p;
                auto equals = entry.find(L'=', 1);
                if (equals == std::wstring::npos)
                    continue;
                auto narrow = [](const std::wstring &w) {
                    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                                nullptr, 0, nullptr, nullptr);
                    std::string s(static_cast<size_t>(n), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(),
                                        n, nullptr, nullptr);
                    return s;
                };
                merged[narrow(entry.substr(0, equals))] = narrow(entry.substr(equals + 1));
            }
            FreeEnvironmentStringsW(block);
        }
#else
        for (auto p = environ; p && *p; ++p) {
            std::string entry = *p;
            auto equals = entry.find('=');
            if (equals != std::string::npos)
                merged[entry.substr(0, equals)] = entry.substr(equals + 1);
        }
#endif
    }
    for (auto &[name, value] : request.env) {
        processArgument(name);
        processArgument(value);
        if (name.empty() || name.find('=') != std::string::npos)
            fail("invalid environment variable name '" + name + "'", "ProcessError");
        merged[name] = value;
    }
    std::vector<std::string> out;
    for (auto &[name, value] : merged)
        out.push_back(name + "=" + value);
    return out;
}
#ifdef _WIN32
struct HandleOwner {
    HANDLE value = nullptr;
    HandleOwner() = default;
    HandleOwner(const HandleOwner &) = delete;
    ~HandleOwner() {
        reset();
    }
    void reset(HANDLE next = nullptr) {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
        value = next;
    }
};
// Windows runs pipeline stages one after another, feeding each stage's output to the next.
ProcessOutcome runStage(const std::vector<std::string> &arguments, const ProcessRequest &request,
                        const std::string &input) {
    ProcessOutcome outcome;
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HandleOwner inRead, inWrite, outRead, outWrite, errRead, errWrite;
    auto pipe = [&](HandleOwner &read, HandleOwner &write, bool parentWrites) {
        HANDLE r, w;
        if (!CreatePipe(&r, &w, &inherit, 0))
            fail("cannot create process pipe", "ProcessError");
        read.reset(r);
        write.reset(w);
        SetHandleInformation(parentWrites ? w : r, HANDLE_FLAG_INHERIT, 0);
    };
    bool feed = request.capture || !input.empty();
    if (feed)
        pipe(inRead, inWrite, true);
    if (request.capture) {
        pipe(outRead, outWrite, false);
        pipe(errRead, errWrite, false);
    }
    std::wstring command;
    for (auto &arg : arguments) {
        processArgument(arg);
        command += (command.empty() ? L"" : L" ") + processQuote(processWide(arg));
    }
    std::wstring environment;
    for (auto &entry : childEnvironment(request))
        environment += processWide(entry) + L'\0';
    environment += L'\0';
    auto directory = processWide(request.cwd);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = feed ? inRead.value : GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = request.capture ? outWrite.value : GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = request.capture ? errWrite.value : GetStdHandle(STD_ERROR_HANDLE);
    ProcessHandles child;
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT,
                        environment.data(), directory.empty() ? nullptr : directory.c_str(),
                        &startup, &child.info))
        fail("cannot start process '" + arguments[0] + "' (Windows error " +
                 std::to_string(GetLastError()) + ")",
             "ProcessError");
    inRead.reset();
    outWrite.reset();
    errWrite.reset();
    auto drain = [](HANDLE handle, std::string &target) {
        char buffer[4096];
        DWORD n = 0;
        while (ReadFile(handle, buffer, sizeof(buffer), &n, nullptr) && n)
            target.append(buffer, n);
    };
    std::thread writer, outReader, errReader;
    if (feed)
        writer = std::thread([&] {
            DWORD written = 0;
            size_t offset = 0;
            while (offset < input.size() &&
                   WriteFile(inWrite.value, input.data() + offset,
                             static_cast<DWORD>(std::min<size_t>(input.size() - offset, 65536)),
                             &written, nullptr))
                offset += written;
            inWrite.reset();
        });
    if (request.capture) {
        outReader = std::thread([&] { drain(outRead.value, outcome.out); });
        errReader = std::thread([&] { drain(errRead.value, outcome.err); });
    }
    DWORD wait = request.timeout > 0 ? static_cast<DWORD>(request.timeout * 1000) : INFINITE;
    if (WaitForSingleObject(child.info.hProcess, wait) == WAIT_TIMEOUT) {
        outcome.timedOut = true;
        TerminateProcess(child.info.hProcess, 1);
        WaitForSingleObject(child.info.hProcess, INFINITE);
    }
    for (auto *t : {&writer, &outReader, &errReader})
        if (t->joinable())
            t->join();
    DWORD status = 0;
    GetExitCodeProcess(child.info.hProcess, &status);
    outcome.status = status;
    return outcome;
}
ProcessOutcome runRequest(const ProcessRequest &request) {
    ProcessOutcome total;
    std::string input = request.input;
    for (size_t i = 0; i < request.stages.size(); ++i) {
        auto stage = request;
        stage.capture = request.capture || i + 1 < request.stages.size();
        auto outcome = runStage(request.stages[i], stage, input);
        total.err += outcome.err;
        total.timedOut = total.timedOut || outcome.timedOut;
        input = outcome.out;
        total.status = outcome.status;
        total.out = outcome.out;
    }
    return total;
}
#else
struct FdOwner {
    int value = -1;
    FdOwner() = default;
    FdOwner(const FdOwner &) = delete;
    ~FdOwner() {
        reset();
    }
    void reset(int next = -1) {
        if (value >= 0)
            close(value);
        value = next;
    }
};
void makePipe(FdOwner &read, FdOwner &write) {
    int fds[2];
    if (pipe(fds) != 0)
        fail("cannot create process pipe: " + std::string(std::strerror(errno)), "ProcessError");
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    read.reset(fds[0]);
    write.reset(fds[1]);
}
// Runs every stage at once with stdout of each stage connected to stdin of the next.
ProcessOutcome runRequest(const ProcessRequest &request) {
    ProcessOutcome outcome;
    auto environment = childEnvironment(request);
    std::vector<char *> envp;
    for (auto &entry : environment)
        envp.push_back(const_cast<char *>(entry.c_str()));
    envp.push_back(nullptr);
    std::vector<std::vector<char *>> argvs;
    for (auto &stage : request.stages) {
        if (stage.empty() || stage[0].empty())
            fail("each process needs a nonempty command", "ProcessError");
        std::vector<char *> argv;
        for (auto &arg : stage) {
            processArgument(arg);
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);
        argvs.push_back(std::move(argv));
    }
    processArgument(request.cwd);
    bool feed = request.capture || !request.input.empty();
    FdOwner inRead, inWrite, outRead, outWrite, errRead, errWrite;
    if (feed)
        makePipe(inRead, inWrite);
    if (request.capture) {
        makePipe(outRead, outWrite);
        makePipe(errRead, errWrite);
    }
    // Report exec failures through a close-on-exec pipe.
    std::vector<pid_t> children;
    int previous = feed ? inRead.value : -1;
    std::vector<std::unique_ptr<FdOwner>> links;
    std::string failure;
    for (size_t i = 0; i < argvs.size(); ++i) {
        bool last = i + 1 == argvs.size();
        auto linkRead = std::make_unique<FdOwner>(), linkWrite = std::make_unique<FdOwner>();
        if (!last)
            makePipe(*linkRead, *linkWrite);
        FdOwner errorRead, errorWrite;
        makePipe(errorRead, errorWrite);
        int stdoutFd = last ? (request.capture ? outWrite.value : -1) : linkWrite->value;
        int stderrFd = request.capture ? errWrite.value : -1;
        pid_t child = fork();
        if (child < 0)
            fail("cannot start process: " + std::string(std::strerror(errno)), "ProcessError");
        if (child == 0) {
            if (previous >= 0)
                dup2(previous, 0);
            if (stdoutFd >= 0)
                dup2(stdoutFd, 1);
            if (stderrFd >= 0)
                dup2(stderrFd, 2);
            if (!request.cwd.empty() && chdir(request.cwd.c_str()) != 0) {
                int code = errno;
                (void)!write(errorWrite.value, &code, sizeof(code));
                _exit(127);
            }
            environ = envp.data();
            execvp(argvs[i][0], argvs[i].data());
            int code = errno;
            (void)!write(errorWrite.value, &code, sizeof(code));
            _exit(127);
        }
        children.push_back(child);
        errorWrite.reset();
        int code = 0;
        ssize_t n;
        while ((n = read(errorRead.value, &code, sizeof(code))) < 0 && errno == EINTR) {
        }
        if (n == sizeof(code) && failure.empty())
            failure = "cannot start process '" + request.stages[i][0] + "': " + std::strerror(code);
        if (!last) {
            linkWrite->reset();
            previous = linkRead->value;
            links.push_back(std::move(linkRead));
        }
    }
    inRead.reset();
    outWrite.reset();
    errWrite.reset();
    links.erase(links.begin(), links.end());
    // Feed input and collect output without blocking on either side.
    size_t offset = 0;
    if (feed && request.input.empty())
        inWrite.reset();
    if (inWrite.value >= 0)
        fcntl(inWrite.value, F_SETFL, O_NONBLOCK);
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(request.timeout));
    while (inWrite.value >= 0 || outRead.value >= 0 || errRead.value >= 0) {
        std::vector<pollfd> watched;
        if (inWrite.value >= 0)
            watched.push_back({inWrite.value, POLLOUT, 0});
        if (outRead.value >= 0)
            watched.push_back({outRead.value, POLLIN, 0});
        if (errRead.value >= 0)
            watched.push_back({errRead.value, POLLIN, 0});
        int wait = -1;
        if (request.timeout > 0) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now())
                            .count();
            if (left <= 0) {
                outcome.timedOut = true;
                break;
            }
            wait = static_cast<int>(std::min<int64_t>(left, 1000));
        }
        int ready = poll(watched.data(), watched.size(), wait);
        if (ready < 0 && errno != EINTR)
            break;
        for (auto &entry : watched) {
            if (!entry.revents)
                continue;
            if (entry.fd == inWrite.value) {
                ssize_t n = write(inWrite.value, request.input.data() + offset,
                                  std::min<size_t>(request.input.size() - offset, 65536));
                if (n > 0)
                    offset += static_cast<size_t>(n);
                if (n < 0 && errno != EAGAIN && errno != EINTR)
                    offset = request.input.size();
                if (offset >= request.input.size())
                    inWrite.reset();
                continue;
            }
            char buffer[4096];
            ssize_t n = read(entry.fd, buffer, sizeof(buffer));
            if (n > 0)
                (entry.fd == outRead.value ? outcome.out : outcome.err)
                    .append(buffer, static_cast<size_t>(n));
            else if (n == 0 || (errno != EAGAIN && errno != EINTR))
                (entry.fd == outRead.value ? outRead : errRead).reset();
        }
    }
    if (outcome.timedOut)
        for (auto child : children)
            kill(child, SIGKILL);
    for (size_t i = 0; i < children.size(); ++i) {
        int status = 0;
        bool reaped = false;
        // A child can outlive its output pipes; keep enforcing the timeout while waiting.
        while (request.timeout > 0 && !outcome.timedOut) {
            auto done = waitpid(children[i], &status, WNOHANG);
            if (done == children[i] || (done < 0 && errno != EINTR)) {
                reaped = true;
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                outcome.timedOut = true;
                for (auto child : children)
                    kill(child, SIGKILL);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        while (!reaped && waitpid(children[i], &status, 0) == -1 && errno == EINTR) {
        }
        if (i + 1 == children.size())
            outcome.status = WIFSIGNALED(status) ? -WTERMSIG(status) : WEXITSTATUS(status);
    }
    if (!failure.empty())
        fail(failure, "ProcessError");
    return outcome;
}
#endif
ProcessRequest processRequest(Object *process) {
    // os.process fields: command, cwd, env, inherit_env, input, timeout, capture
    ProcessRequest request;
    auto command = process->slots.at(0).object;
    if (!command || command->slots.empty())
        fail("os.process requires a nonempty command array", "ProcessError");
    std::vector<std::string> stage;
    for (auto &slot : command->slots)
        stage.push_back(slot.object ? slot.object->text : "");
    request.stages.push_back(std::move(stage));
    request.cwd = instanceText(process, 1);
    if (auto env = process->slots.at(2).object)
        for (size_t i = 0; i < env->keys.size(); ++i)
            request.env.emplace_back(env->keys[i].object->text,
                                     env->slots[i].object ? env->slots[i].object->text : "");
    request.inheritEnv = process->slots.at(3).integer != 0;
    request.input = instanceText(process, 4);
    request.timeout = process->slots.at(5).real;
    request.capture = process->slots.at(6).integer != 0;
    return request;
}
Object *processResult(Frame *frame, const ProcessOutcome &outcome) {
    auto result = nativeInstance(frame, "os.result", {"status", "stdout", "stderr", "timed_out"});
    result->slots = {integerSlot(outcome.status), textSlot(frame, outcome.out),
                     textSlot(frame, outcome.err), boolSlot(outcome.timedOut)};
    return result;
}

// ---------------------------------------------------------------------------------------------
// net: blocking TCP sockets over IPv4 and IPv6
struct SocketResource : NativeResource {
    Socket socket = invalidSocket;
    bool listener = false;
    std::string buffered; // bytes read past a line by receive_line
    ~SocketResource() override {
        if (socket != invalidSocket)
            closeSocket(socket);
    }
};
void networkReady() {
    static Network network;
}
int socketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}
bool interrupted() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}
std::string socketMessage(int code) {
#ifdef _WIN32
    return "socket error " + std::to_string(code);
#else
    return std::strerror(code);
#endif
}
SocketResource &socketOf(Object *p, bool listener) {
    auto resource = p && p->kind == Object::Resource
                        ? dynamic_cast<SocketResource *>(p->nativeResource())
                        : nullptr;
    if (!resource || resource->listener != listener)
        fail(listener ? "expected net.listener" : "expected net.socket", "NetworkError");
    if (resource->socket == invalidSocket)
        fail("socket is closed", "NetworkError");
    return *resource;
}
Object *socketObject(Frame *frame, Socket socket, bool listener) {
    auto object = managed(frame, Object::Resource);
    auto resource = std::make_unique<SocketResource>();
    resource->socket = socket;
    resource->listener = listener;
    object->text = listener ? "net.listener" : "net.socket";
    object->more().resource = std::move(resource);
    return object;
}
struct AddressList {
    addrinfo *head = nullptr;
    ~AddressList() {
        if (head)
            freeaddrinfo(head);
    }
};
void resolveAddresses(AddressList &list, const std::string &host, int64_t port, bool passive) {
    networkReady();
    if (port < 0 || port > 65535)
        fail("port must be in 0..65535", "NetworkError");
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    auto service = std::to_string(port);
    int status = getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints,
                             &list.head);
    if (status != 0)
#ifdef _WIN32
        fail("cannot resolve '" + host + "' (error " + std::to_string(status) + ")", "NetworkError");
#else
        fail("cannot resolve '" + host + "': " + gai_strerror(status), "NetworkError");
#endif
}
std::string addressText(const sockaddr *address) {
    char host[NI_MAXHOST] = {}, service[NI_MAXSERV] = {};
    socklen_t length = address->sa_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    if (getnameinfo(address, length, host, sizeof(host), service, sizeof(service),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0)
        return "";
    std::string h = host;
    return (address->sa_family == AF_INET6 ? "[" + h + "]" : h) + ":" + service;
}
void setTimeout(Socket socket, double seconds) {
#ifdef _WIN32
    DWORD value = static_cast<DWORD>(seconds * 1000);
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&value),
               sizeof(value));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&value),
               sizeof(value));
#else
    timeval value{static_cast<time_t>(seconds),
                  static_cast<suseconds_t>((seconds - std::floor(seconds)) * 1e6)};
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
#endif
}
} // namespace

extern "C" {
void fw_register_class(const char *name, const void *id) {
    std::lock_guard<std::mutex> lock(classRegistryMutex);
    classRegistry[name] = id;
}

// fs -------------------------------------------------------------------------------------------
// 0 list (sorted names), 1 walk (sorted relative paths, recursive)
Object *fw_fs_list(Frame *frame, Object *path, int64_t recursive) {
    return nativeBoundary([&] {
        auto root = fsPath(path);
        std::error_code ec;
        std::vector<std::string> names;
        if (recursive) {
            for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
                names.push_back(fsText(it->path().lexically_relative(root)));
        } else
            for (auto it = std::filesystem::directory_iterator(root, ec);
                 !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
                names.push_back(fsText(it->path().filename()));
        if (ec)
            fsFail("cannot list directory", root, ec);
        std::sort(names.begin(), names.end());
        return stringArray(frame, names);
    });
}
void fw_fs_mkdir(Object *path) {
    nativeBoundary([&] {
        auto target = fsPath(path);
        std::error_code ec;
        std::filesystem::create_directories(target, ec);
        if (ec || !std::filesystem::is_directory(target))
            fsFail("cannot create directory", target, ec);
    });
}
void fw_fs_append(Object *path, Object *data) {
    nativeBoundary([&] {
        auto target = fsPath(path);
        std::ofstream out(target, std::ios::binary | std::ios::app);
        if (!(out << data->text))
            fail("cannot append to file '" + fsText(target) + "'", "FileError");
    });
}
// 0 is_dir, 1 is_file, 2 size
int64_t fw_fs_query(Object *path, int64_t operation) {
    return nativeBoundary([&]() -> int64_t {
        auto target = fsPath(path);
        std::error_code ec;
        if (operation == 0)
            return std::filesystem::is_directory(target, ec);
        if (operation == 1)
            return std::filesystem::is_regular_file(target, ec);
        auto size = std::filesystem::file_size(target, ec);
        if (ec)
            fsFail("cannot read size of", target, ec);
        return static_cast<int64_t>(size);
    });
}
// 0 copy (recursive, overwriting), 1 rename/move
void fw_fs_copy(Object *from, Object *to, int64_t operation) {
    nativeBoundary([&] {
        auto source = fsPath(from), target = fsPath(to);
        std::error_code ec;
        if (operation == 1)
            std::filesystem::rename(source, target, ec);
        else
            std::filesystem::copy(source, target,
                                  std::filesystem::copy_options::recursive |
                                      std::filesystem::copy_options::overwrite_existing,
                                  ec);
        if (ec)
            fsFail(operation ? "cannot move" : "cannot copy", source, ec);
    });
}
int64_t fw_fs_remove_all(Object *path) {
    return nativeBoundary([&]() -> int64_t {
        auto target = fsPath(path);
        std::error_code ec;
        auto removed = std::filesystem::remove_all(target, ec);
        if (ec)
            fsFail("cannot remove", target, ec);
        return static_cast<int64_t>(removed);
    });
}
// 0 basename, 1 dirname, 2 extension, 3 stem, 4 absolute, 5 normalize
Object *fw_fs_path(Frame *frame, Object *path, int64_t operation) {
    return nativeBoundary([&] {
        auto p = fsPath(path);
        std::error_code ec;
        switch (operation) {
        case 0:
            return string(frame, fsText(p.filename()));
        case 1:
            return string(frame, fsText(p.parent_path()));
        case 2:
            return string(frame, fsText(p.extension()));
        case 3:
            return string(frame, fsText(p.stem()));
        case 4: {
            auto absolute = std::filesystem::absolute(p, ec);
            if (ec)
                fsFail("cannot make absolute", p, ec);
            return string(frame, fsText(absolute.lexically_normal()));
        }
        default:
            return string(frame, fsText(p.lexically_normal()));
        }
    });
}
Object *fw_fs_join(Frame *frame, Object *parts) {
    return nativeBoundary([&] {
        std::filesystem::path out;
        for (auto &part : parts->slots)
            out /= fsPath(part.object);
        return string(frame, fsText(out));
    });
}
// 0 current directory, 1 temporary directory
Object *fw_fs_location(Frame *frame, int64_t which) {
    return nativeBoundary([&] {
        std::error_code ec;
        auto p = which ? std::filesystem::temp_directory_path(ec) : std::filesystem::current_path(ec);
        if (ec)
            fail("cannot determine directory: " + ec.message(), "FileError");
        return string(frame, fsText(p));
    });
}

// time -----------------------------------------------------------------------------------------
double fw_time_now() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}
Object *fw_time_format(Frame *frame, double seconds, Object *pattern, int64_t local) {
    return nativeBoundary([&] {
        auto parts = calendar(seconds, local != 0);
        std::ostringstream out;
        out << std::put_time(&parts, requireString(pattern)->text.c_str());
        return string(frame, out.str());
    });
}
double fw_time_parse(Object *text, Object *pattern) {
    return nativeBoundary([&]() -> double {
        std::tm parts{};
        std::istringstream in(requireString(text)->text);
        in >> std::get_time(&parts, requireString(pattern)->text.c_str());
        if (in.fail())
            fail("'" + text->text + "' does not match time pattern '" + pattern->text + "'",
                 "RuntimeError");
        return fromCalendar(parts);
    });
}
// time.datetime fields: year, month, day, hour, minute, second, weekday (0 = Sunday), yearday
Object *fw_time_date(Frame *frame, double seconds, int64_t local) {
    return nativeBoundary([&] {
        auto parts = calendar(seconds, local != 0);
        auto date = nativeInstance(frame, "time.datetime",
                                   {"year", "month", "day", "hour", "minute", "second", "weekday",
                                    "yearday"});
        date->slots = {integerSlot(parts.tm_year + 1900), integerSlot(parts.tm_mon + 1),
                       integerSlot(parts.tm_mday),        integerSlot(parts.tm_hour),
                       integerSlot(parts.tm_min),         integerSlot(parts.tm_sec),
                       integerSlot(parts.tm_wday),        integerSlot(parts.tm_yday + 1)};
        return date;
    });
}
double fw_time_from_date(int64_t year, int64_t month, int64_t day, int64_t hour, int64_t minute,
                         double second) {
    return nativeBoundary([&]() -> double {
        if (month < 1 || month > 12 || day < 1 || day > 31)
            fail("invalid calendar date", "RuntimeError");
        std::tm parts{};
        parts.tm_year = static_cast<int>(year - 1900);
        parts.tm_mon = static_cast<int>(month - 1);
        parts.tm_mday = static_cast<int>(day);
        parts.tm_hour = static_cast<int>(hour);
        parts.tm_min = static_cast<int>(minute);
        return fromCalendar(parts) + second;
    });
}

// regex ----------------------------------------------------------------------------------------
int64_t fw_regex_test(Object *pattern, Object *text, int64_t full) {
    return regexBoundary([&]() -> int64_t {
        auto &re = compiledRegex(requireString(pattern)->text);
        return full ? std::regex_match(requireString(text)->text, re)
                    : std::regex_search(requireString(text)->text, re);
    });
}
Object *fw_regex_find(Frame *frame, Object *pattern, Object *text) {
    return regexBoundary([&]() -> Object * {
        std::smatch match;
        auto &source = requireString(text)->text;
        if (!std::regex_search(source, match, compiledRegex(requireString(pattern)->text)))
            return nullptr;
        return string(frame, match.str(0));
    });
}
// 0 find_all (whole matches), 1 groups of the first match (null when nothing matches)
Object *fw_regex_list(Frame *frame, Object *pattern, Object *text, int64_t groups) {
    return regexBoundary([&]() -> Object * {
        auto &source = requireString(text)->text;
        auto &re = compiledRegex(requireString(pattern)->text);
        std::vector<std::string> out;
        if (groups) {
            std::smatch match;
            if (!std::regex_search(source, match, re))
                return nullptr;
            for (size_t i = 1; i < match.size(); ++i)
                out.push_back(match[i].matched ? match.str(i) : "");
        } else
            for (std::sregex_iterator it(source.begin(), source.end(), re), end; it != end; ++it)
                out.push_back(it->str(0));
        return stringArray(frame, out);
    });
}
Object *fw_regex_replace(Frame *frame, Object *pattern, Object *text, Object *replacement) {
    return regexBoundary([&] {
        return string(frame, std::regex_replace(requireString(text)->text,
                                                compiledRegex(requireString(pattern)->text),
                                                requireString(replacement)->text));
    });
}
Object *fw_regex_split(Frame *frame, Object *pattern, Object *text) {
    return regexBoundary([&] {
        auto &source = requireString(text)->text;
        std::vector<std::string> out;
        std::sregex_token_iterator it(source.begin(), source.end(),
                                      compiledRegex(requireString(pattern)->text), -1),
            end;
        for (; it != end; ++it)
            out.push_back(it->str());
        return stringArray(frame, out);
    });
}

// io -------------------------------------------------------------------------------------------
void fw_io_write(Object *text, int64_t error) {
    nativeBoundary([&] {
        auto &stream = error ? std::cerr : std::cout;
        if (!(stream << requireString(text)->text))
            fail("cannot write to standard output", "FileError");
    });
}
void fw_io_flush() {
    std::cout.flush();
    std::cerr.flush();
}
Object *fw_io_read_line(Frame *frame) {
    return nativeBoundary([&]() -> Object * {
        std::string line;
        if (!std::getline(std::cin, line))
            return nullptr;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        return string(frame, line);
    });
}
Object *fw_io_read_all(Frame *frame) {
    return nativeBoundary([&] {
        std::ostringstream out;
        out << std::cin.rdbuf();
        std::cin.clear();
        return string(frame, out.str());
    });
}
// mode "r", "w", "a", "r+" (read and write)
Object *fw_io_open(Frame *frame, Object *path, Object *mode) {
    return nativeBoundary([&] {
        auto target = fsPath(path);
        auto &m = requireString(mode)->text;
        std::ios::openmode flags = std::ios::binary;
        if (m == "r")
            flags |= std::ios::in;
        else if (m == "w")
            flags |= std::ios::out | std::ios::trunc;
        else if (m == "a")
            flags |= std::ios::out | std::ios::app;
        else if (m == "r+")
            flags |= std::ios::in | std::ios::out;
        else
            fail("file mode must be \"r\", \"w\", \"a\", or \"r+\"", "FileError");
        auto resource = std::make_unique<FileResource>();
        resource->path = fsText(target);
        resource->stream.open(target, flags);
        if (!resource->stream.is_open())
            fail("cannot open file '" + resource->path + "'", "FileError");
        resource->open = true;
        auto object = managed(frame, Object::Resource);
        object->text = "io.file";
        object->more().resource = std::move(resource);
        return object;
    });
}
// 0 read_line (null at end), 1 read_all, 2 lines
Object *fw_io_file_read(Frame *frame, Object *file, int64_t operation) {
    return nativeBoundary([&]() -> Object * {
        auto &f = fileOf(file);
        if (operation == 1) {
            std::ostringstream out;
            out << f.stream.rdbuf();
            f.stream.clear();
            return string(frame, out.str());
        }
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(f.stream, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (operation == 0)
                return string(frame, line);
            lines.push_back(line);
        }
        f.stream.clear();
        return operation == 0 ? nullptr : stringArray(frame, lines);
    });
}
void fw_io_file_write(Object *file, Object *text) {
    nativeBoundary([&] {
        auto &f = fileOf(file);
        if (!(f.stream << requireString(text)->text))
            fail("cannot write to file '" + f.path + "'", "FileError");
    });
}
void fw_io_file_close(Object *file) {
    nativeBoundary([&] {
        auto &f = fileOf(file);
        f.stream.close();
        f.open = false;
    });
}

// json -----------------------------------------------------------------------------------------
Object *fw_json_at(Frame *frame, Object *value, int64_t index) {
    return nativeBoundary([&] {
        auto &j = requireJson(value);
        if (j.kind != Json::List)
            fail("expected JSON array", "JsonError");
        if (index < 0 || static_cast<size_t>(index) >= j.items.size())
            fail("JSON array index out of bounds", "IndexError");
        return jsonObject(frame, j.items[static_cast<size_t>(index)]);
    });
}
int64_t fw_json_length(Object *value) {
    return nativeBoundary([&]() -> int64_t {
        auto &j = requireJson(value);
        if (j.kind == Json::List)
            return static_cast<int64_t>(j.items.size());
        if (j.kind == Json::Dictionary)
            return static_cast<int64_t>(j.fields.size());
        fail("expected JSON array or object", "JsonError");
    });
}
// 0 items (array elements), 1 keys (object field names)
Object *fw_json_list(Frame *frame, Object *value, int64_t keys) {
    return nativeBoundary([&] {
        auto &j = requireJson(value);
        if (keys) {
            if (j.kind != Json::Dictionary)
                fail("expected JSON object", "JsonError");
            std::vector<std::string> names;
            for (auto &[name, item] : j.fields)
                names.push_back(name);
            return stringArray(frame, names);
        }
        if (j.kind != Json::List)
            fail("expected JSON array", "JsonError");
        auto out = managed(frame, Object::Array);
        for (auto &item : j.items) {
            auto element = new Object(Object::JsonValue);
            element->json = item;
            Slot slot;
            slot.kind = Slot::Reference;
            slot.object = element;
            out->slots.push_back(std::move(slot));
        }
        return out;
    });
}
int64_t fw_json_is_null(Object *value) {
    return nativeBoundary([&]() -> int64_t { return requireJson(value).kind == Json::Null; });
}
// Encodes the value boxed in slot 0 of `box`; indent > 0 pretty-prints.
Object *fw_json_encode(Frame *frame, Object *box, int64_t indent) {
    return nativeBoundary([&] {
        std::string out;
        std::set<Object *> active;
        encodeSlot(box->slots.at(0), out, static_cast<int>(std::clamp<int64_t>(indent, 0, 16)), 0,
                   active);
        return string(frame, out);
    });
}

// os -------------------------------------------------------------------------------------------
void fw_set_args(int64_t count, char **values) {
    auto &arguments = programArguments();
    arguments.clear();
    for (int64_t i = 1; i < count; ++i)
        arguments.emplace_back(values[i]);
}
Object *fw_os_args(Frame *frame) {
    return nativeBoundary([&] { return stringArray(frame, programArguments()); });
}
void fw_os_exit(int64_t code) {
    std::cout.flush();
    std::cerr.flush();
    std::exit(static_cast<int>(code));
}
Object *fw_os_run(Frame *frame, Object *process) {
    return nativeBoundary([&] {
        auto request = processRequest(process);
        flushProcessOutput();
        return processResult(frame, runRequest(request));
    });
}
// Runs commands connected stdout-to-stdin; the result has the last stage's status and stdout.
Object *fw_os_pipeline(Frame *frame, Object *commands, Object *input) {
    return nativeBoundary([&] {
        ProcessRequest request;
        for (auto &slot : commands->slots) {
            std::vector<std::string> stage;
            for (auto &arg : slot.object->slots)
                stage.push_back(arg.object ? arg.object->text : "");
            request.stages.push_back(std::move(stage));
        }
        if (request.stages.empty())
            fail("os.pipeline requires at least one command", "ProcessError");
        request.input = requireString(input)->text;
        flushProcessOutput();
        return processResult(frame, runRequest(request));
    });
}

// net ------------------------------------------------------------------------------------------
Object *fw_net_resolve(Frame *frame, Object *host) {
    return nativeBoundary([&] {
        AddressList list;
        resolveAddresses(list, requireString(host)->text, 0, false);
        std::vector<std::string> addresses;
        for (auto a = list.head; a; a = a->ai_next) {
            auto text = addressText(a->ai_addr);
            text = text.substr(0, text.rfind(':'));
            if (!text.empty() && text.front() == '[')
                text = text.substr(1, text.size() - 2);
            if (std::find(addresses.begin(), addresses.end(), text) == addresses.end())
                addresses.push_back(text);
        }
        return stringArray(frame, addresses);
    });
}
Object *fw_net_connect(Frame *frame, Object *host, int64_t port, double timeout) {
    return nativeBoundary([&] {
        AddressList list;
        resolveAddresses(list, requireString(host)->text, port, false);
        int code = 0;
        for (auto a = list.head; a; a = a->ai_next) {
            Socket s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s == invalidSocket) {
                code = socketError();
                continue;
            }
            if (timeout > 0)
                setTimeout(s, timeout);
            if (connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0)
                return socketObject(frame, s, false);
            code = socketError();
            closeSocket(s);
        }
        fail("cannot connect to " + host->text + ":" + std::to_string(port) + ": " +
                 socketMessage(code),
             "NetworkError");
    });
}
Object *fw_net_listen(Frame *frame, Object *host, int64_t port) {
    return nativeBoundary([&] {
        AddressList list;
        resolveAddresses(list, requireString(host)->text, port, true);
        int code = 0;
        for (auto a = list.head; a; a = a->ai_next) {
            Socket s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s == invalidSocket) {
                code = socketError();
                continue;
            }
            int reuse = 1;
#ifdef _WIN32
            setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&reuse),
                       sizeof(reuse));
#else
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse),
                       sizeof(reuse));
#endif
            if (bind(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0 && listen(s, 64) == 0)
                return socketObject(frame, s, true);
            code = socketError();
            closeSocket(s);
        }
        fail("cannot listen on " + host->text + ":" + std::to_string(port) + ": " +
                 socketMessage(code),
             "NetworkError");
    });
}
Object *fw_net_accept(Frame *frame, Object *listener) {
    return nativeBoundary([&] {
        auto &resource = socketOf(listener, true);
        Socket client;
        do
            client = accept(resource.socket, nullptr, nullptr);
        while (client == invalidSocket && interrupted());
        if (client == invalidSocket)
            fail("accept failed: " + socketMessage(socketError()), "NetworkError");
        return socketObject(frame, client, false);
    });
}
// 0 local address, 1 peer address ("host:port"); for a listener, 2 local port
Object *fw_net_address(Frame *frame, Object *object, int64_t peer) {
    return nativeBoundary([&] {
        auto resource = object && object->kind == Object::Resource
                            ? dynamic_cast<SocketResource *>(object->nativeResource())
                            : nullptr;
        if (!resource || resource->socket == invalidSocket)
            fail("socket is closed", "NetworkError");
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        int status = peer ? getpeername(resource->socket, reinterpret_cast<sockaddr *>(&address),
                                        &length)
                          : getsockname(resource->socket, reinterpret_cast<sockaddr *>(&address),
                                        &length);
        if (status != 0)
            fail("cannot read socket address", "NetworkError");
        return string(frame, addressText(reinterpret_cast<sockaddr *>(&address)));
    });
}
int64_t fw_net_port(Object *object) {
    return nativeBoundary([&]() -> int64_t {
        auto resource = object && object->kind == Object::Resource
                            ? dynamic_cast<SocketResource *>(object->nativeResource())
                            : nullptr;
        if (!resource || resource->socket == invalidSocket)
            fail("socket is closed", "NetworkError");
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        if (getsockname(resource->socket, reinterpret_cast<sockaddr *>(&address), &length) != 0)
            fail("cannot read socket address", "NetworkError");
        return address.ss_family == AF_INET6
                   ? ntohs(reinterpret_cast<sockaddr_in6 *>(&address)->sin6_port)
                   : ntohs(reinterpret_cast<sockaddr_in *>(&address)->sin_port);
    });
}
void fw_net_send(Object *socketValue, Object *text) {
    nativeBoundary([&] {
        auto &resource = socketOf(socketValue, false);
        auto &data = requireString(text)->text;
        size_t sent = 0;
        while (sent < data.size()) {
#ifdef MSG_NOSIGNAL
            constexpr int flags = MSG_NOSIGNAL;
#else
            constexpr int flags = 0;
#endif
            int n = static_cast<int>(::send(resource.socket, data.data() + sent,
                                            static_cast<int>(std::min<size_t>(data.size() - sent,
                                                                              65536)),
                                            flags));
            if (n < 0 && interrupted())
                continue;
            if (n <= 0)
                fail("send failed: " + socketMessage(socketError()), "NetworkError");
            sent += static_cast<size_t>(n);
        }
    });
}
// Receives up to `limit` bytes; returns "" at end of stream. line != 0 reads one line instead
// and returns null at end of stream.
Object *fw_net_receive(Frame *frame, Object *socketValue, int64_t limit, int64_t line) {
    return nativeBoundary([&]() -> Object * {
        auto &resource = socketOf(socketValue, false);
        auto &buffer = resource.buffered;
        char chunk[4096];
        auto fill = [&]() {
            while (true) {
                int n = static_cast<int>(recv(resource.socket, chunk, sizeof(chunk), 0));
                if (n < 0 && interrupted())
                    continue;
                if (n < 0)
                    fail("receive failed: " + socketMessage(socketError()), "NetworkError");
                buffer.append(chunk, static_cast<size_t>(n));
                return n > 0;
            }
        };
        if (line) {
            size_t end;
            while ((end = buffer.find('\n')) == std::string::npos)
                if (!fill()) {
                    if (buffer.empty())
                        return nullptr;
                    auto rest = std::move(buffer);
                    buffer.clear();
                    return string(frame, rest);
                }
            auto text = buffer.substr(0, end);
            buffer.erase(0, end + 1);
            if (!text.empty() && text.back() == '\r')
                text.pop_back();
            return string(frame, text);
        }
        if (limit <= 0)
            fail("receive limit must be positive", "NetworkError");
        if (buffer.empty())
            fill();
        auto count = std::min<size_t>(buffer.size(), static_cast<size_t>(limit));
        auto text = buffer.substr(0, count);
        buffer.erase(0, count);
        return string(frame, text);
    });
}
void fw_net_timeout(Object *socketValue, double seconds) {
    nativeBoundary([&] { setTimeout(socketOf(socketValue, false).socket, seconds); });
}
void fw_net_close(Object *object) {
    nativeBoundary([&] {
        auto resource = object && object->kind == Object::Resource
                            ? dynamic_cast<SocketResource *>(object->nativeResource())
                            : nullptr;
        if (!resource)
            fail("expected a socket", "NetworkError");
        if (resource->socket != invalidSocket)
            closeSocket(resource->socket);
        resource->socket = invalidSocket;
    });
}
}
