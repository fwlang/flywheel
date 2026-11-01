// TLS for HTTPS servers and the HTTP client, through the Mbed TLS copy compiled into the
// runtime (cmake/Bundled.cmake), so programs need no system TLS library. Included by
// runtime.cpp.
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/threading.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#ifdef _WIN32
#include <wincrypt.h>
#endif
namespace {
#ifdef _WIN32
// MBEDTLS_THREADING_ALT (runtime/bundled/threading_alt.h): SRW locks and condition variables.
static_assert(sizeof(mbedtls_platform_mutex_t) == sizeof(SRWLOCK));
static_assert(sizeof(mbedtls_platform_condition_variable_t) == sizeof(CONDITION_VARIABLE));
PSRWLOCK srwLock(mbedtls_platform_mutex_t *m) {
    return reinterpret_cast<PSRWLOCK>(&m->lock);
}
PCONDITION_VARIABLE conditionOf(mbedtls_platform_condition_variable_t *c) {
    return reinterpret_cast<PCONDITION_VARIABLE>(&c->condition);
}
void tlsThreading() {
    mbedtls_threading_set_alt(
        [](mbedtls_platform_mutex_t *m) {
            InitializeSRWLock(srwLock(m));
            return 0;
        },
        [](mbedtls_platform_mutex_t *) {},
        [](mbedtls_platform_mutex_t *m) {
            AcquireSRWLockExclusive(srwLock(m));
            return 0;
        },
        [](mbedtls_platform_mutex_t *m) {
            ReleaseSRWLockExclusive(srwLock(m));
            return 0;
        },
        [](mbedtls_platform_condition_variable_t *c) {
            InitializeConditionVariable(conditionOf(c));
            return 0;
        },
        [](mbedtls_platform_condition_variable_t *) {},
        [](mbedtls_platform_condition_variable_t *c) {
            WakeConditionVariable(conditionOf(c));
            return 0;
        },
        [](mbedtls_platform_condition_variable_t *c) {
            WakeAllConditionVariable(conditionOf(c));
            return 0;
        },
        [](mbedtls_platform_condition_variable_t *c, mbedtls_platform_mutex_t *m) {
            return SleepConditionVariableSRW(conditionOf(c), srwLock(m), INFINITE, 0)
                       ? 0
                       : MBEDTLS_ERR_THREADING_USAGE_ERROR;
        });
}
#endif
std::string tlsMessage(int code) {
    char text[200];
    mbedtls_strerror(code, text, sizeof text);
    return text;
}
void tlsInit(const char *errorType) {
    static std::once_flag once;
    static psa_status_t status = PSA_ERROR_BAD_STATE;
    std::call_once(once, [] {
#ifdef _WIN32
        tlsThreading();
#endif
        status = psa_crypto_init();
    });
    if (status != PSA_SUCCESS)
        fail("cannot initialize TLS cryptography (PSA error " + std::to_string(status) + ")",
             errorType);
}
bool tlsRetry(int code) {
    return code == MBEDTLS_ERR_SSL_WANT_READ || code == MBEDTLS_ERR_SSL_WANT_WRITE ||
           code == MBEDTLS_ERR_SSL_ASYNC_IN_PROGRESS ||
           code == MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS;
}
// A certificate chain and key for one HTTPS listener.
struct TlsServerConfig {
    mbedtls_ssl_config config;
    mbedtls_x509_crt chain;
    mbedtls_pk_context key;
    TlsServerConfig() {
        mbedtls_ssl_config_init(&config);
        mbedtls_x509_crt_init(&chain);
        mbedtls_pk_init(&key);
    }
    TlsServerConfig(const TlsServerConfig &) = delete;
    ~TlsServerConfig() {
        mbedtls_ssl_config_free(&config);
        mbedtls_x509_crt_free(&chain);
        mbedtls_pk_free(&key);
    }
};
// Whether a private key belongs to a certificate. (mbedtls_pk_check_pair cannot compare RSA
// keys, so the encoded public keys are compared.)
bool samePublicKey(const mbedtls_pk_context &certificate, const mbedtls_pk_context &key) {
    unsigned char a[4096], b[4096];
    // The DER is written at the end of each buffer; the result is its length.
    int n = mbedtls_pk_write_pubkey_der(&certificate, a, sizeof a);
    int m = mbedtls_pk_write_pubkey_der(&key, b, sizeof b);
    return n > 0 && n == m && std::memcmp(a + sizeof a - n, b + sizeof b - m, n) == 0;
}
std::unique_ptr<TlsServerConfig> tlsServerConfig(const std::string &certificate,
                                                 const std::string &key) {
    tlsInit("RuntimeError");
    auto c = std::make_unique<TlsServerConfig>();
    if (mbedtls_x509_crt_parse_file(&c->chain, certificate.c_str()) != 0)
        fail("cannot load TLS certificate '" + certificate + "'", "FileError");
    if (mbedtls_pk_parse_keyfile(&c->key, key.c_str(), nullptr) != 0 ||
        !samePublicKey(c->chain.pk, c->key))
        fail("cannot load TLS private key '" + key + "'", "FileError");
    int code = mbedtls_ssl_config_defaults(&c->config, MBEDTLS_SSL_IS_SERVER,
                                           MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT);
    if (!code)
        code = mbedtls_ssl_conf_own_cert(&c->config, &c->chain, &c->key);
    if (code)
        fail("cannot create TLS context: " + tlsMessage(code));
    return c;
}
constexpr int tlsAgain = -1;  // the socket would block; retry when it is ready
constexpr int tlsFailed = -2; // the connection failed; `error` holds the Mbed TLS code
// One TLS connection over a socket the caller owns. Non-blocking sessions report tlsAgain;
// blocking ones (the HTTP client, with socket timeouts) treat a timeout as a failure.
struct TlsSession {
    mbedtls_ssl_context ssl;
    Socket socket;
    bool nonBlocking;
    size_t pendingWrite = 0; // length of a write that must be retried with the same data
    int error = 0;
    TlsSession(Socket socket, bool nonBlocking) : socket(socket), nonBlocking(nonBlocking) {
        mbedtls_ssl_init(&ssl);
    }
    TlsSession(const TlsSession &) = delete;
    ~TlsSession() {
        if (mbedtls_ssl_is_handshake_over(&ssl))
            mbedtls_ssl_close_notify(&ssl); // best effort
        mbedtls_ssl_free(&ssl);
    }
    bool setup(const mbedtls_ssl_config *config) {
        if ((error = mbedtls_ssl_setup(&ssl, config)))
            return false;
        mbedtls_ssl_set_bio(&ssl, this, sendTo, receiveFrom, nullptr);
        return true;
    }
    bool socketWouldBlock() const {
        if (!nonBlocking)
            return false;
#ifdef _WIN32
        auto code = WSAGetLastError();
        return code == WSAEWOULDBLOCK || code == WSAEINTR;
#else
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
    }
    static int sendTo(void *context, const unsigned char *data, size_t size) {
        auto &s = *static_cast<TlsSession *>(context);
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        auto chunk = static_cast<int>(std::min<size_t>(size, 1 << 20));
        int n = static_cast<int>(
            ::send(s.socket, reinterpret_cast<const char *>(data), chunk, flags));
        if (n >= 0)
            return n;
        return s.socketWouldBlock() ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    }
    static int receiveFrom(void *context, unsigned char *data, size_t size) {
        auto &s = *static_cast<TlsSession *>(context);
        auto chunk = static_cast<int>(std::min<size_t>(size, 1 << 20));
        int n = static_cast<int>(::recv(s.socket, reinterpret_cast<char *>(data), chunk, 0));
        if (n >= 0)
            return n;
        return s.socketWouldBlock() ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    }
    // 1 once the handshake is complete, 0 to retry later, tlsFailed on error.
    int handshake() {
        int code = mbedtls_ssl_handshake(&ssl);
        if (!code)
            return 1;
        if (tlsRetry(code))
            return 0;
        error = code;
        return tlsFailed;
    }
    // Bytes read, 0 at the end of the stream, tlsAgain, or tlsFailed.
    int read(void *buffer, size_t size) {
        while (true) {
            int n = mbedtls_ssl_read(&ssl, static_cast<unsigned char *>(buffer), size);
            if (n > 0)
                return n;
            if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == MBEDTLS_ERR_SSL_CONN_EOF)
                return 0;
            // TLS 1.3 servers send session tickets after the handshake; keep reading.
            if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
                continue;
            // Mbed TLS also asks for a retry after records that carry no application data,
            // which on a blocking socket just means reading again.
            if (tlsRetry(n)) {
                if (nonBlocking)
                    return tlsAgain;
                continue;
            }
            error = n;
            return tlsFailed;
        }
    }
    // Bytes written (at least 1), tlsAgain, or tlsFailed. After tlsAgain the caller must
    // retry with the same leading bytes; the retried length is remembered here.
    int write(const void *data, size_t size) {
        if (pendingWrite)
            size = std::min(size, pendingWrite);
        while (true) {
            int n = mbedtls_ssl_write(&ssl, static_cast<const unsigned char *>(data), size);
            if (n > 0) {
                pendingWrite = 0;
                return n;
            }
            if (!tlsRetry(n)) {
                error = n;
                return tlsFailed;
            }
            if (nonBlocking) {
                pendingWrite = size;
                return tlsAgain;
            }
        }
    }
};
// Server session for an accepted, non-blocking socket; null when it cannot be set up.
std::unique_ptr<TlsSession> tlsAccept(const TlsServerConfig &config, Socket socket) {
    auto session = std::make_unique<TlsSession>(socket, true);
    if (!session->setup(&config.config))
        return nullptr;
    return session;
}
// Trusted roots for the HTTP client: SSL_CERT_FILE / SSL_CERT_DIR when set, otherwise the
// system store (Windows) or the distribution's CA bundle (Linux and BSDs).
void loadRoots(mbedtls_x509_crt &roots) {
    auto loaded = [&] { return roots.raw.len > 0; };
    if (auto file = std::getenv("SSL_CERT_FILE"); file && *file)
        mbedtls_x509_crt_parse_file(&roots, file);
    if (auto dir = std::getenv("SSL_CERT_DIR"); dir && *dir && !loaded())
        mbedtls_x509_crt_parse_path(&roots, dir);
    if (loaded())
        return;
#ifdef _WIN32
    if (auto store = CertOpenSystemStoreW(0, L"ROOT")) {
        for (auto c = CertEnumCertificatesInStore(store, nullptr); c;
             c = CertEnumCertificatesInStore(store, c))
            if (c->dwCertEncodingType & X509_ASN_ENCODING)
                mbedtls_x509_crt_parse_der(&roots, c->pbCertEncoded, c->cbCertEncoded);
        CertCloseStore(store, 0);
    }
#else
    for (auto file : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                      "/etc/ssl/ca-bundle.pem", "/etc/pki/tls/cacert.pem",
                      "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", "/etc/ssl/cert.pem",
                      "/usr/local/etc/ssl/cert.pem", "/usr/local/share/certs/ca-root-nss.crt"}) {
        mbedtls_x509_crt_parse_file(&roots, file);
        if (loaded())
            return;
    }
    mbedtls_x509_crt_parse_path(&roots, "/etc/ssl/certs");
#endif
}
struct TlsClientConfig {
    mbedtls_ssl_config config;
    mbedtls_x509_crt roots;
    TlsClientConfig() {
        mbedtls_ssl_config_init(&config);
        mbedtls_x509_crt_init(&roots);
    }
    TlsClientConfig(const TlsClientConfig &) = delete;
    ~TlsClientConfig() {
        mbedtls_ssl_config_free(&config);
        mbedtls_x509_crt_free(&roots);
    }
};
const TlsClientConfig &tlsClientConfig() {
    static std::mutex mutex;
    static std::unique_ptr<TlsClientConfig> shared;
    std::lock_guard<std::mutex> lock(mutex);
    if (shared)
        return *shared;
    tlsInit("HttpError");
    auto c = std::make_unique<TlsClientConfig>();
    loadRoots(c->roots);
    if (!c->roots.raw.len)
        fail("no trusted root certificates were found for HTTPS (set SSL_CERT_FILE to a CA "
             "bundle)",
             "HttpError");
    int code = mbedtls_ssl_config_defaults(&c->config, MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT);
    if (code)
        fail("cannot create a TLS client context: " + tlsMessage(code), "HttpError");
    mbedtls_ssl_conf_authmode(&c->config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&c->config, &c->roots, nullptr);
    shared = std::move(c);
    return *shared;
}
// Client handshake over a connected, blocking socket, verifying the certificate for `host`.
std::unique_ptr<TlsSession> tlsConnect(Socket socket, const std::string &host) {
    auto &config = tlsClientConfig();
    auto session = std::make_unique<TlsSession>(socket, false);
    if (!session->setup(&config.config) ||
        mbedtls_ssl_set_hostname(&session->ssl, host.c_str()) != 0)
        fail("cannot create a TLS session", "HttpError");
    int done;
    while ((done = session->handshake()) == 0) {
    }
    if (done == 1)
        return session;
    auto flags = mbedtls_ssl_get_verify_result(&session->ssl);
    std::string detail;
    if (session->error == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags && flags != uint32_t(-1)) {
        char text[512];
        mbedtls_x509_crt_verify_info(text, sizeof text, "", flags);
        detail = text;
        while (!detail.empty() && (detail.back() == '\n' || detail.back() == ' '))
            detail.pop_back();
        for (auto &c : detail)
            if (c == '\n')
                c = ';';
        detail = "certificate verification failed: " + detail;
    } else {
        detail = tlsMessage(session->error);
    }
    fail("TLS handshake with " + host + " failed (" + detail + ")", "HttpError");
}
} // namespace
