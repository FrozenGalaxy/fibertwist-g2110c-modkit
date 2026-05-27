/*
 * ont_json_httpd.c - tiny cached HTTP wrapper for diag_to_json on embedded ONT.
 *
 * Endpoints:
 *   GET /status   return cached JSON; if stale, refresh after response
 *   GET /refresh  refresh cache now and return JSON
 *   GET /info     return server/cache metadata
 *   GET /         help text
 *
 * Build:
 *   $HOME/buildroot-ont/output/host/bin/mips-buildroot-linux-uclibc-gcc -mips1 -EB -msoft-float -mno-mips16 -O2 -Wall -Wextra -Wl,--dynamic-linker=/lib/ld-uClibc.so.0 -o ont_json_httpd ont_json_httpd.c
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef O_NONBLOCK
#define O_NONBLOCK 04000
#endif

#define DEFAULT_PORT 8090
#define DEFAULT_TTL_SECONDS 10
#define DEFAULT_COLLECTOR_TIMEOUT_SECONDS 8
#define DEFAULT_MAX_JSON_BYTES (512u * 1024u)
#define DEFAULT_COLLECTOR "/var/config/diag_to_json"
#define READ_REQ_MAX 2048
#define ERRBUF_SIZE 192

typedef struct {
    int port;
    int ttl_seconds;
    int collector_timeout_seconds;
    size_t max_json_bytes;
    const char *collector_path;
    const char *bind_addr;
} config_t;

typedef struct {
    char *data;
    size_t len;
    time_t updated_at;
    unsigned long refresh_ok_count;
    unsigned long refresh_fail_count;
    int last_rc;
    char last_error[ERRBUF_SIZE];
} cache_t;

static void set_error(char *err, size_t errn, const char *fmt, ...) {
    va_list ap;
    if (!err || errn == 0) return;
    va_start(ap, fmt);
    vsnprintf(err, errn, fmt, ap);
    va_end(ap);
    err[errn - 1] = '\0';
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [-p port] [-a bind_addr] [-t ttl_seconds] [-T collector_timeout_seconds] [-m max_json_bytes] [-c collector_path]\n"
        "\nDefaults:\n"
        "  -p %d\n"
        "  -a 0.0.0.0\n"
        "  -t %d\n"
        "  -T %d\n"
        "  -m %u\n"
        "  -c %s\n",
        argv0,
        DEFAULT_PORT,
        DEFAULT_TTL_SECONDS,
        DEFAULT_COLLECTOR_TIMEOUT_SECONDS,
        (unsigned)DEFAULT_MAX_JSON_BYTES,
        DEFAULT_COLLECTOR);
}

static int parse_int_arg(const char *s, int min_v, int max_v, int *out) {
    char *end = NULL;
    long v;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return -1;
    if (v < min_v || v > max_v) return -1;
    *out = (int)v;
    return 0;
}

static int parse_size_arg(const char *s, size_t min_v, size_t max_v, size_t *out) {
    char *end = NULL;
    unsigned long v;
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return -1;
    if ((size_t)v < min_v || (size_t)v > max_v) return -1;
    *out = (size_t)v;
    return 0;
}

static int parse_args(int argc, char **argv, config_t *cfg) {
    int i;
    cfg->port = DEFAULT_PORT;
    cfg->ttl_seconds = DEFAULT_TTL_SECONDS;
    cfg->collector_timeout_seconds = DEFAULT_COLLECTOR_TIMEOUT_SECONDS;
    cfg->max_json_bytes = DEFAULT_MAX_JSON_BYTES;
    cfg->collector_path = DEFAULT_COLLECTOR;
    cfg->bind_addr = "0.0.0.0";

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            if (parse_int_arg(argv[++i], 1, 65535, &cfg->port) != 0) return -1;
        } else if (!strcmp(argv[i], "-a") && i + 1 < argc) {
            cfg->bind_addr = argv[++i];
        } else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            if (parse_int_arg(argv[++i], 0, 86400, &cfg->ttl_seconds) != 0) return -1;
        } else if (!strcmp(argv[i], "-T") && i + 1 < argc) {
            if (parse_int_arg(argv[++i], 1, 120, &cfg->collector_timeout_seconds) != 0) return -1;
        } else if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            if (parse_size_arg(argv[++i], 1024u, 8u * 1024u * 1024u, &cfg->max_json_bytes) != 0) return -1;
        } else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            cfg->collector_path = argv[++i];
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            exit(0);
        } else {
            return -1;
        }
    }
    return 0;
}

static int set_fd_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int run_collector(const char *path, int timeout_sec, size_t max_bytes,
                         char **out, size_t *out_len, char *err, size_t errn) {
    int pfd[2];
    pid_t pid;
    char *buf = NULL;
    size_t cap = 0, len = 0;
    int child_status = 0;
    int child_reaped = 0;
    int eof_seen = 0;
    time_t start;

    *out = NULL;
    *out_len = 0;
    set_error(err, errn, "unknown error");

    if (pipe(pfd) != 0) {
        set_error(err, errn, "pipe failed errno=%d", errno);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        set_error(err, errn, "fork failed errno=%d", errno);
        close(pfd[0]); close(pfd[1]);
        return -1;
    }

    if (pid == 0) {
        int devnull;
        close(pfd[0]);
        if (dup2(pfd[1], STDOUT_FILENO) < 0) _exit(125);
        close(pfd[1]);
        devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execl(path, path, (char *)NULL);
        _exit(127);
    }

    close(pfd[1]);

    if (set_fd_nonblock(pfd[0]) != 0) {
        set_error(err, errn, "fcntl nonblock failed errno=%d", errno);
        kill(pid, SIGKILL);
        close(pfd[0]);
        waitpid(pid, &child_status, 0);
        return -1;
    }

    cap = 16384;
    if (cap > max_bytes) cap = max_bytes;
    buf = (char *)malloc(cap + 1);
    if (!buf) {
        set_error(err, errn, "malloc failed");
        kill(pid, SIGKILL);
        close(pfd[0]);
        waitpid(pid, &child_status, 0);
        return -1;
    }

    start = time(NULL);

    while (!eof_seen) {
        fd_set rfds;
        struct timeval tv;
        int sr;
        time_t now = time(NULL);

        if (!child_reaped) {
            pid_t wr = waitpid(pid, &child_status, WNOHANG);
            if (wr == pid) child_reaped = 1;
        }

        if (now >= start + timeout_sec) {
            set_error(err, errn, "collector timeout after %d seconds", timeout_sec);
            kill(pid, SIGKILL);
            close(pfd[0]);
            waitpid(pid, &child_status, 0);
            free(buf);
            return -1;
        }

        FD_ZERO(&rfds);
        FD_SET(pfd[0], &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        sr = select(pfd[0] + 1, &rfds, NULL, NULL, &tv);
        if (sr < 0) {
            if (errno == EINTR) continue;
            set_error(err, errn, "select failed errno=%d", errno);
            kill(pid, SIGKILL);
            close(pfd[0]);
            waitpid(pid, &child_status, 0);
            free(buf);
            return -1;
        }
        if (sr == 0) continue;

        for (;;) {
            char tmp[4096];
            ssize_t r = read(pfd[0], tmp, sizeof(tmp));
            if (r > 0) {
                if (len + (size_t)r > max_bytes) {
                    set_error(err, errn, "collector output exceeded max bytes");
                    kill(pid, SIGKILL);
                    close(pfd[0]);
                    waitpid(pid, &child_status, 0);
                    free(buf);
                    return -1;
                }
                if (len + (size_t)r > cap) {
                    size_t ncap = cap * 2;
                    char *nbuf;
                    while (ncap < len + (size_t)r) ncap *= 2;
                    if (ncap > max_bytes) ncap = max_bytes;
                    nbuf = (char *)realloc(buf, ncap + 1);
                    if (!nbuf) {
                        set_error(err, errn, "realloc failed");
                        kill(pid, SIGKILL);
                        close(pfd[0]);
                        waitpid(pid, &child_status, 0);
                        free(buf);
                        return -1;
                    }
                    buf = nbuf;
                    cap = ncap;
                }
                memcpy(buf + len, tmp, (size_t)r);
                len += (size_t)r;
            } else if (r == 0) {
                eof_seen = 1;
                break;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
                set_error(err, errn, "read failed errno=%d", errno);
                kill(pid, SIGKILL);
                close(pfd[0]);
                waitpid(pid, &child_status, 0);
                free(buf);
                return -1;
            }
        }
    }

    close(pfd[0]);

    if (!child_reaped) {
        if (waitpid(pid, &child_status, 0) != pid) {
            set_error(err, errn, "waitpid failed errno=%d", errno);
            free(buf);
            return -1;
        }
    }

    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        if (WIFEXITED(child_status)) set_error(err, errn, "collector exited rc=%d", WEXITSTATUS(child_status));
        else if (WIFSIGNALED(child_status)) set_error(err, errn, "collector killed by signal=%d", WTERMSIG(child_status));
        else set_error(err, errn, "collector ended abnormally");
        free(buf);
        return -1;
    }

    if (len == 0) {
        set_error(err, errn, "collector produced empty output");
        free(buf);
        return -1;
    }

    buf[len] = '\0';
    {
        size_t i = 0;
        while (i < len && (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t')) ++i;
        if (i == len || (buf[i] != '{' && buf[i] != '[')) {
            set_error(err, errn, "collector output does not look like JSON");
            free(buf);
            return -1;
        }
    }

    *out = buf;
    *out_len = len;
    set_error(err, errn, "ok");
    return 0;
}

static int refresh_cache(const config_t *cfg, cache_t *cache) {
    char *new_data = NULL;
    size_t new_len = 0;
    char err[ERRBUF_SIZE];
    int rc = run_collector(cfg->collector_path, cfg->collector_timeout_seconds,
                           cfg->max_json_bytes, &new_data, &new_len, err, sizeof(err));
    cache->last_rc = rc;
    if (rc == 0) {
        free(cache->data);
        cache->data = new_data;
        cache->len = new_len;
        cache->updated_at = time(NULL);
        cache->refresh_ok_count++;
        snprintf(cache->last_error, sizeof(cache->last_error), "ok");
        return 0;
    }
    cache->refresh_fail_count++;
    snprintf(cache->last_error, sizeof(cache->last_error), "%s", err);
    return rc ? rc : -1;
}

static int send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

static void http_date_now(char *buf, size_t bufn) {
    time_t now = time(NULL);
    struct tm *tmv = gmtime(&now);
    if (!tmv) snprintf(buf, bufn, "Thu, 01 Jan 1970 00:00:00 GMT");
    else strftime(buf, bufn, "%a, %d %b %Y %H:%M:%S GMT", tmv);
}

static void send_response(int fd, const char *status, const char *ctype,
                          const char *body, size_t body_len,
                          const char *extra_headers, int head_only) {
    char hdr[1024];
    char datebuf[64];
    int n;
    http_date_now(datebuf, sizeof(datebuf));
    n = snprintf(hdr, sizeof(hdr),
                 "HTTP/1.0 %s\r\n"
                 "Date: %s\r\n"
                 "Server: ont_json_httpd/1.0\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %lu\r\n"
                 "Connection: close\r\n"
                 "Cache-Control: no-store\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "%s"
                 "\r\n",
                 status, datebuf, ctype, (unsigned long)body_len,
                 extra_headers ? extra_headers : "");
    if (n < 0) return;
    if ((size_t)n > sizeof(hdr)) n = (int)sizeof(hdr);
    send_all(fd, hdr, (size_t)n);
    if (!head_only && body && body_len) send_all(fd, body, body_len);
}

static void json_escape_to_buf(const char *src, char *dst, size_t dstn) {
    size_t pos = 0;
#define PUTCH(ch) do { if (pos + 1 < dstn) dst[pos++] = (char)(ch); } while (0)
#define PUTS2(a,b) do { PUTCH(a); PUTCH(b); } while (0)
    if (dstn == 0) return;
    PUTCH('"');
    while (*src && pos + 2 < dstn) {
        unsigned char c = (unsigned char)*src++;
        if (c == '"' || c == '\\') { PUTCH('\\'); PUTCH(c); }
        else if (c == '\n') { PUTS2('\\','n'); }
        else if (c == '\r') { PUTS2('\\','r'); }
        else if (c == '\t') { PUTS2('\\','t'); }
        else if (c < 0x20) {
            char tmp[8];
            int n = snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)c);
            int i;
            for (i = 0; n > 0 && i < n && pos + 1 < dstn; ++i) PUTCH(tmp[i]);
        } else PUTCH(c);
    }
    PUTCH('"');
    dst[pos] = '\0';
#undef PUTCH
#undef PUTS2
}

static void send_info_json(int fd, const config_t *cfg, const cache_t *cache, int head_only) {
    char body[2048];
    char esc_collector[512];
    char esc_error[512];
    time_t now = time(NULL);
    long age = cache->data ? (long)(now - cache->updated_at) : -1;
    int n;
    json_escape_to_buf(cfg->collector_path, esc_collector, sizeof(esc_collector));
    json_escape_to_buf(cache->last_error, esc_error, sizeof(esc_error));
    n = snprintf(body, sizeof(body),
                 "{\n"
                 "  \"server\": \"ont_json_httpd\",\n"
                 "  \"cache_valid\": %s,\n"
                 "  \"cache_age_seconds\": %ld,\n"
                 "  \"cache_ttl_seconds\": %d,\n"
                 "  \"cache_bytes\": %lu,\n"
                 "  \"refresh_ok_count\": %lu,\n"
                 "  \"refresh_fail_count\": %lu,\n"
                 "  \"last_refresh_rc\": %d,\n"
                 "  \"collector_path\": %s,\n"
                 "  \"last_error\": %s\n"
                 "}\n",
                 cache->data ? "true" : "false",
                 age, cfg->ttl_seconds,
                 (unsigned long)(cache->data ? cache->len : 0),
                 cache->refresh_ok_count, cache->refresh_fail_count,
                 cache->last_rc, esc_collector, esc_error);
    if (n < 0) return;
    if ((size_t)n >= sizeof(body)) n = (int)sizeof(body) - 1;
    send_response(fd, "200 OK", "application/json", body, (size_t)n, NULL, head_only);
}

static void send_text(int fd, const char *status, const char *text, int head_only) {
    send_response(fd, status, "text/plain; charset=utf-8", text, strlen(text), NULL, head_only);
}

static int read_request_line(int fd, char *method, size_t methodn, char *path, size_t pathn) {
    char req[READ_REQ_MAX + 1];
    ssize_t r;
    size_t total = 0;
    char *line_end, *sp1, *sp2;
    memset(req, 0, sizeof(req));
    while (total < READ_REQ_MAX) {
        r = read(fd, req + total, READ_REQ_MAX - total);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) break;
        total += (size_t)r;
        req[total] = '\0';
        if (strstr(req, "\r\n") || strchr(req, '\n')) break;
    }
    if (total == 0) return -1;
    line_end = strstr(req, "\r\n");
    if (!line_end) line_end = strchr(req, '\n');
    if (line_end) *line_end = '\0';
    sp1 = strchr(req, ' ');
    if (!sp1) return -1;
    *sp1++ = '\0';
    sp2 = strchr(sp1, ' ');
    if (!sp2) return -1;
    *sp2 = '\0';
    snprintf(method, methodn, "%s", req);
    snprintf(path, pathn, "%s", sp1);
    {
        char *q = strchr(path, '?');
        if (q) *q = '\0';
    }
    return 0;
}

static int cache_is_stale(const config_t *cfg, const cache_t *cache) {
    time_t now;
    if (!cache->data) return 1;
    if (cfg->ttl_seconds == 0) return 1;
    now = time(NULL);
    return (now - cache->updated_at) >= cfg->ttl_seconds;
}

static void handle_status(int fd, const config_t *cfg, cache_t *cache, int head_only) {
    char extra[256];
    long age;
    int stale_after_send;
    if (!cache->data) refresh_cache(cfg, cache);
    if (!cache->data) {
        char body[512];
        char esc_error[256];
        int n;
        json_escape_to_buf(cache->last_error, esc_error, sizeof(esc_error));
        n = snprintf(body, sizeof(body), "{\"ok\": false, \"error\": \"no cache and refresh failed\", \"last_error\": %s}\n", esc_error);
        if (n < 0) return;
        if ((size_t)n >= sizeof(body)) n = (int)sizeof(body) - 1;
        send_response(fd, "503 Service Unavailable", "application/json", body, (size_t)n, NULL, head_only);
        return;
    }
    age = (long)(time(NULL) - cache->updated_at);
    stale_after_send = cache_is_stale(cfg, cache);
    snprintf(extra, sizeof(extra),
             "X-Cache: %s\r\nX-Cache-Age: %ld\r\nX-Cache-TTL: %d\r\n",
             stale_after_send ? "stale" : "hit", age, cfg->ttl_seconds);
    send_response(fd, "200 OK", "application/json", cache->data, cache->len, extra, head_only);
    if (stale_after_send) refresh_cache(cfg, cache);
}

static void handle_refresh(int fd, const config_t *cfg, cache_t *cache, int head_only) {
    int rc = refresh_cache(cfg, cache);
    char extra[384];
    if (rc == 0 && cache->data) {
        snprintf(extra, sizeof(extra), "X-Cache: refreshed\r\nX-Cache-Age: 0\r\nX-Refresh-Result: ok\r\n");
        send_response(fd, "200 OK", "application/json", cache->data, cache->len, extra, head_only);
        return;
    }
    if (cache->data) {
        long age = (long)(time(NULL) - cache->updated_at);
        snprintf(extra, sizeof(extra),
                 "X-Cache: stale\r\nX-Cache-Age: %ld\r\nX-Refresh-Result: failed\r\nX-Refresh-Error: %s\r\n",
                 age, cache->last_error);
        send_response(fd, "200 OK", "application/json", cache->data, cache->len, extra, head_only);
        return;
    }
    {
        char body[512];
        char esc_error[256];
        int n;
        json_escape_to_buf(cache->last_error, esc_error, sizeof(esc_error));
        n = snprintf(body, sizeof(body), "{\"ok\": false, \"error\": \"refresh failed\", \"last_error\": %s}\n", esc_error);
        if (n < 0) return;
        if ((size_t)n >= sizeof(body)) n = (int)sizeof(body) - 1;
        send_response(fd, "503 Service Unavailable", "application/json", body, (size_t)n, NULL, head_only);
    }
}

static void handle_client(int fd, const config_t *cfg, cache_t *cache) {
    char method[16], path[256];
    int head_only = 0;
    if (read_request_line(fd, method, sizeof(method), path, sizeof(path)) != 0) {
        send_text(fd, "400 Bad Request", "bad request\n", 0);
        return;
    }
    if (!strcmp(method, "HEAD")) head_only = 1;
    else if (strcmp(method, "GET")) {
        send_text(fd, "405 Method Not Allowed", "method not allowed\n", 0);
        return;
    }
    if (!strcmp(path, "/status")) handle_status(fd, cfg, cache, head_only);
    else if (!strcmp(path, "/refresh")) handle_refresh(fd, cfg, cache, head_only);
    else if (!strcmp(path, "/info")) send_info_json(fd, cfg, cache, head_only);
    else if (!strcmp(path, "/") || !strcmp(path, "")) {
        const char *help =
            "ont_json_httpd\n\n"
            "GET /status   return cached JSON; if stale, refresh after response\n"
            "GET /refresh  refresh cache now and return JSON\n"
            "GET /info     server/cache metadata\n\n";
        send_text(fd, "200 OK", help, head_only);
    } else send_text(fd, "404 Not Found", "not found\n", head_only);
}

static int create_listen_socket(const config_t *cfg) {
    int fd, one = 1;
    struct sockaddr_in addr;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)cfg->port);
    if (!strcmp(cfg->bind_addr, "0.0.0.0") || !strcmp(cfg->bind_addr, "*")) addr.sin_addr.s_addr = htonl(INADDR_ANY);
    else {
        addr.sin_addr.s_addr = inet_addr(cfg->bind_addr);
        if (addr.sin_addr.s_addr == (in_addr_t)-1) { close(fd); return -1; }
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    if (listen(fd, 8) != 0) { close(fd); return -1; }
    return fd;
}

int main(int argc, char **argv) {
    config_t cfg;
    cache_t cache;
    int lfd;
    memset(&cache, 0, sizeof(cache));
    cache.last_rc = 0;
    snprintf(cache.last_error, sizeof(cache.last_error), "no refresh yet");
    if (parse_args(argc, argv, &cfg) != 0) { usage(argv[0]); return 2; }
    signal(SIGPIPE, SIG_IGN);
    lfd = create_listen_socket(&cfg);
    if (lfd < 0) {
        fprintf(stderr, "failed to bind/listen on %s:%d errno=%d\n", cfg.bind_addr, cfg.port, errno);
        return 1;
    }
    fprintf(stderr, "ont_json_httpd listening on %s:%d collector=%s ttl=%d timeout=%d max=%lu\n",
            cfg.bind_addr, cfg.port, cfg.collector_path, cfg.ttl_seconds,
            cfg.collector_timeout_seconds, (unsigned long)cfg.max_json_bytes);
    for (;;) {
        int cfd;
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        cfd = accept(lfd, (struct sockaddr *)&peer, &peer_len);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            sleep(1);
            continue;
        }
        handle_client(cfd, &cfg, &cache);
        close(cfd);
    }
    return 0;
}
