#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <termios.h>
#include <sys/ioctl.h>
#define MAX_HISTORY 2000
#define MAX_LINE    1200

static pthread_mutex_t hist_lock = PTHREAD_MUTEX_INITIALIZER;
static char history[MAX_HISTORY][MAX_LINE];
static int  hist_count = 0;     // number of stored lines (<= MAX_HISTORY)
static int  hist_start = 0;     // ring buffer start index

static volatile sig_atomic_t tui_dirty = 0;
static int tui_scroll = 0;      // 0 = bottom, >0 = scrolled up


static void hist_add_line(const char *s) {
    pthread_mutex_lock(&hist_lock);

    int idx = (hist_start + hist_count) % MAX_HISTORY;
    strncpy(history[idx], s, MAX_LINE - 1);
    history[idx][MAX_LINE - 1] = '\0';

    if (hist_count < MAX_HISTORY) hist_count++;
    else hist_start = (hist_start + 1) % MAX_HISTORY;

    pthread_mutex_unlock(&hist_lock);
}

static int hist_get_line(int i, char out[MAX_LINE]) {
    pthread_mutex_lock(&hist_lock);
    if (i < 0 || i >= hist_count) { pthread_mutex_unlock(&hist_lock); return -1; }
    int idx = (hist_start + i) % MAX_HISTORY;
    strncpy(out, history[idx], MAX_LINE);
    pthread_mutex_unlock(&hist_lock);
    return 0;
}

// ---------------- Protocol ----------------

typedef enum MessageType {
    MSG_LOGIN        = 0,   // OUTBOUND
    MSG_LOGOUT       = 1,   // OUTBOUND
    MSG_MESSAGE_SEND = 2,   // OUTBOUND
    MSG_MESSAGE_RECV = 10,  // INBOUND
    MSG_DISCONNECT   = 12,  // INBOUND
    MSG_SYSTEM       = 13   // INBOUND
} message_type_t;

typedef struct __attribute__((packed)) Message {
    uint32_t type;       // network order on the wire
    uint32_t timestamp;  // network order on the wire
    char username[32];   // null-terminated
    char message[1024];  // null-terminated
} message_t;

// ---------------- UI ----------------

static const char *COLOR_RED   = "\033[31m";
static const char *COLOR_GRAY  = "\033[90m";
static const char *COLOR_RESET = "\033[0m";

// ---------------- Global state ----------------

typedef struct ClientConfig {
    bool quiet;
    bool tui;
    char host[256];  // ip or domain string
    char port[16];   // string for getaddrinfo
} client_config_t;

typedef struct ClientState {
    int fd;
    pthread_t recv_thread;
    bool recv_thread_started;

    volatile sig_atomic_t stop_requested;
    volatile sig_atomic_t connected;

    char username[32];
    client_config_t cfg;

    pthread_mutex_t io_lock; // serialize stdout if desired
} client_state_t;

static client_state_t g = {
    .fd = -1,
    .recv_thread_started = false,
    .stop_requested = 0,
    .connected = 0
};

// ---------------- Helpers ----------------
static struct termios orig_termios;
static bool validate_line(const char *s, size_t *out_len);
static int  send_chat(int fd, const char *line);
static void term_raw_on(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);   // no line buffering, no echo
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;              // 0.1s read timeout
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void term_raw_off(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}
static void term_get_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        *rows = 24; *cols = 80;
    }
}
static void tui_redraw(const char *input) {
    int R, C;
    term_get_size(&R, &C);
    int msg_rows = R - 1; // last row is input

    // clear screen + home
    write(STDOUT_FILENO, "\033[2J\033[H", 7);

    // how many messages exist
    pthread_mutex_lock(&hist_lock);
    int count = hist_count;
    pthread_mutex_unlock(&hist_lock);

    // show bottom by default; tui_scroll moves view up
    int start = count - msg_rows - tui_scroll;
    if (start < 0) start = 0;

    for (int row = 0; row < msg_rows; row++) {
        int i = start + row;
        if (i >= count) break;

        char line[MAX_LINE];
        if (hist_get_line(i, line) == 0) {
            // clip to screen width
            line[C] = '\0';
            write(STDOUT_FILENO, line, strlen(line));
        }
        write(STDOUT_FILENO, "\n", 1);
    }

    // draw input line at bottom
    // move cursor to last row, col 1
    char buf[64];
    snprintf(buf, sizeof(buf), "\033[%d;1H", R);
    write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, "\033[2K", 4);     // clear line
    write(STDOUT_FILENO, "> ", 2);

    // show tail if too long
    int max_in = C - 3;
    int L = (int)strlen(input);
    const char *p = input;
    if (L > max_in) p = input + (L - max_in);
    write(STDOUT_FILENO, p, strlen(p));
}
static void tui_loop(void) {
    term_raw_on();
    atexit(term_raw_off);

    char input[1024] = {0};
    int len = 0;

    tui_dirty = 1;

    while (!g.stop_requested) {
        if (tui_dirty) {
            tui_dirty = 0;
            tui_redraw(input);
        }

        unsigned char ch;
        ssize_t n = read(STDIN_FILENO, &ch, 1);
        if (n <= 0) continue;

        if (ch == 27) { // ESC
            unsigned char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) <= 0) continue;
            if (read(STDIN_FILENO, &seq[1], 1) <= 0) continue;

            if (seq[0] == '[' && seq[1] == 'A') {       // UP
                tui_scroll++;
                tui_dirty = 1;
            } else if (seq[0] == '[' && seq[1] == 'B') { // DOWN
                if (tui_scroll > 0) tui_scroll--;
                tui_dirty = 1;
            }
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            input[len] = '\0';

            size_t outL = 0;
            if (len > 0 && validate_line(input, &outL)) {
                if (send_chat(g.fd, input) != 0) {
                    g.stop_requested = 1;
                }
            }
            len = 0;
            input[0] = '\0';
            tui_scroll = 0;     // jump back to bottom after sending (nice UX)
            tui_dirty = 1;
            continue;
        }

        if (ch == 127 || ch == 8) { // backspace
            if (len > 0) input[--len] = '\0';
            tui_dirty = 1;
            continue;
        }

        if (isprint(ch) && len < 1023) {
            input[len++] = (char)ch;
            input[len] = '\0';
            tui_dirty = 1;
        }
    }

    term_raw_off();
    // clear screen once exiting
    write(STDOUT_FILENO, "\033[2J\033[H", 7);
}
static void usage(FILE *out) {
    fprintf(out,
        "usage: ./client [--port PORT] [--ip IP | --domain DOMAIN] [--quiet] [--tui] [-h]\n"

        "\n"
        "options:\n"
        "  -h, --help           show this help message\n"
        "      --port PORT      port to connect to (default: 8080)\n"
        "      --ip IP          IPv4 address to connect to (default: 127.0.0.1)\n"
        "      --domain DOMAIN  domain to connect to (mutually exclusive with --ip)\n"
        "      --quiet          disable highlight + beep\n"
        "      --tui            enable text UI mode\n" 
    );
}

static void die(const char *msg) {
    fprintf(stderr, "Error: %s\n", msg);
    exit(1);
}

static void die_errno(const char *msg) {
    fprintf(stderr, "Error: %s: %s\n", msg, strerror(errno));
    exit(1);
}

static bool is_valid_username(const char *s) {
    if (!s || !*s) return false;
    size_t n = strlen(s);
    if (n >= 32) return false;
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)s[i])) return false;
    }
    return true;
}

static int load_username(char out[32]) {
    // Prefer POSIX user info over popen("whoami")
    struct passwd *pw = getpwuid(getuid());
    if (!pw || !pw->pw_name) return -1;
    if (!is_valid_username(pw->pw_name)) return -1;

    strncpy(out, pw->pw_name, 31);
    out[31] = '\0';
    return 0;
}

static void on_signal(int signo) {
    (void)signo;
    g.stop_requested = 1;
}

static void install_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;   // just sets a flag (async-signal-safe)
    sigemptyset(&sa.sa_mask);
    // default flags = 0 (per assignment advice)
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static ssize_t full_read(int fd, void *buf, size_t n) {
    size_t off = 0;
    unsigned char *p = (unsigned char *)buf;

    while (off < n) {
        ssize_t r = read(fd, p + off, n - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return 0; // peer closed
        off += (size_t)r;
    }
    return (ssize_t)off;
}

static ssize_t full_write(int fd, const void *buf, size_t n) {
    size_t off = 0;
    const unsigned char *p = (const unsigned char *)buf;

    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return (ssize_t)off;
}
static int connect_to_server(const char *host, const char *port) {
    struct addrinfo hints, *res = NULL, *it = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;      // IPv4 as spec says TCP IPv4
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "Error: getaddrinfo(%s,%s): %s\n", host, port, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (it = res; it; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            break; // success
        }
        close(fd);
	fprintf(stderr, "connect(%s:%s) failed: %s\n", host, port, strerror(errno));
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

static int send_login(int fd, const char username[32]) {
    message_t m;
    memset(&m, 0, sizeof(m));
    m.type = htonl(MSG_LOGIN);
    m.timestamp = htonl((uint32_t)time(NULL));
    strncpy(m.username, username, sizeof(m.username) - 1);
    m.username[sizeof(m.username) - 1] = '\0';

    if (full_write(fd, &m, sizeof(m)) < 0) return -1;
    return 0;
}

static int send_logout(int fd, const char username[32]) {
    message_t m;
    memset(&m, 0, sizeof(m));
    m.type = htonl(MSG_LOGOUT);
    m.timestamp = htonl((uint32_t)time(NULL));
    strncpy(m.username, username, sizeof(m.username) - 1);
    m.username[sizeof(m.username) - 1] = '\0';

    if (full_write(fd, &m, sizeof(m)) < 0) return -1;
    return 0;
}

static int send_chat(int fd, const char *line) {
    message_t m;
    memset(&m, 0, sizeof(m));
    m.type = htonl(MSG_MESSAGE_SEND);
    m.timestamp = htonl((uint32_t)time(NULL));
    // server typically ignores username for send, but safe to include nothing or keep empty.
    strncpy(m.message, line, sizeof(m.message) - 1);
    m.message[sizeof(m.message) - 1] = '\0';

    if (full_write(fd, &m, sizeof(m)) < 0) return -1;
    return 0;
}

static void print_timestamp(uint32_t ts_net, char out[64]) {
    uint32_t ts = ntohl(ts_net);
    time_t t = (time_t)ts;
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, 64, "%Y-%m-%d %H:%M:%S", &tmv);
}

static void print_highlighted(const char *msg, const char *mention) {
    // Highlight all occurrences of mention in msg
    size_t L = strlen(mention);
    const char *cur = msg;
    const char *hit;

    while ((hit = strstr(cur, mention)) != NULL) {
        fwrite(cur, 1, (size_t)(hit - cur), stdout);
        printf("\a%s%s%s", COLOR_RED, mention, COLOR_RESET);
        cur = hit + L;
    }
    fputs(cur, stdout);
}

static void *receiver_main(void *arg) {
    (void)arg;

    while (!g.stop_requested) {
        message_t m;
        ssize_t r = full_read(g.fd, &m, sizeof(m));
        if (r == 0) {
            g.stop_requested = 1;
            break;
        }
        if (r < 0) {
            fprintf(stderr, "Error: read failed: %s\n", strerror(errno));
            g.stop_requested = 1;
            break;
        }

        // safety: ensure termination
        m.username[31] = '\0';
        m.message[1023] = '\0';

        uint32_t type = ntohl(m.type);

        // ---------- TUI mode: store only ----------
        if (g.cfg.tui) {
            if (type == MSG_MESSAGE_RECV) {
                char tbuf[64];
                print_timestamp(m.timestamp, tbuf);

                char line[MAX_LINE];
                snprintf(line, sizeof(line), "[%s] %s: %s", tbuf, m.username, m.message);
                hist_add_line(line);
                tui_dirty = 1;

            } else if (type == MSG_SYSTEM) {
                char line[MAX_LINE];
                snprintf(line, sizeof(line), "[SYSTEM] %s", m.message);
                hist_add_line(line);
                tui_dirty = 1;

            } else if (type == MSG_DISCONNECT) {
                char line[MAX_LINE];
                snprintf(line, sizeof(line), "[DISCONNECT] %s", m.message);
                hist_add_line(line);
                tui_dirty = 1;
                g.stop_requested = 1;
            }
            continue;
        }

        // ---------- non-TUI mode: print ----------
        pthread_mutex_lock(&g.io_lock);

        if (type == MSG_MESSAGE_RECV) {
            char tbuf[64];
            print_timestamp(m.timestamp, tbuf);

            printf("[%s] %s: ", tbuf, m.username);

            if (g.cfg.quiet) {
                printf("%s\n", m.message);
            } else {
                char mention[40];
                snprintf(mention, sizeof(mention), "@%s", g.username);
                print_highlighted(m.message, mention);
                printf("\n");
            }
            fflush(stdout);

        } else if (type == MSG_SYSTEM) {
            printf("%s[SYSTEM] %s%s\n", COLOR_GRAY, m.message, COLOR_RESET);
            fflush(stdout);

        } else if (type == MSG_DISCONNECT) {
            printf("%s[DISCONNECT] %s%s\n", COLOR_RED, m.message, COLOR_RESET);
            fflush(stdout);
            g.stop_requested = 1;

        } else {
            fprintf(stderr, "Error: Unknown inbound message type %u\n", type);
        }

        pthread_mutex_unlock(&g.io_lock);
    }

    return NULL;
}

// Validate outbound line per typical spec: 1..1023 printable
static bool validate_line(const char *s, size_t *out_len) {
    size_t L = strlen(s);
    if (L < 1 || L > 1023) return false;
    for (size_t i = 0; i < L; i++) {
        if (!isprint((unsigned char)s[i])) return false;
    }
    if (out_len) *out_len = L;
    return true;
}

// ---------------- Arg parsing ----------------

static void parse_args(int argc, char **argv) {
    // defaults
    memset(&g.cfg, 0, sizeof(g.cfg));
    strncpy(g.cfg.host, "127.0.0.1", sizeof(g.cfg.host) - 1);
    strncpy(g.cfg.port, "8080", sizeof(g.cfg.port) - 1);
    g.cfg.quiet = false;

    bool have_ip = false;
    bool have_domain = false;

    static struct option longopts[] = {
        {"help",   no_argument,       0, 'h'},
        {"port",   required_argument, 0,  1 },
        {"ip",     required_argument, 0,  2 },
        {"domain", required_argument, 0,  3 },
        {"quiet",  no_argument,       0,  4 },
        {"tui",    no_argument,       0,  5 },

        {0,0,0,0}
    };

    int opt;
    int idx = 0;
    while ((opt = getopt_long(argc, argv, "h", longopts, &idx)) != -1) {
        if (opt == 'h') {
            usage(stdout);
            exit(0);
        } else if (opt == 1) { // --port
            char *end = NULL;
            long v = strtol(optarg, &end, 10);
            if (!end || *end != '\0' || v < 1 || v > 65535) die("invalid port");
            snprintf(g.cfg.port, sizeof(g.cfg.port), "%ld", v);
        } else if (opt == 2) { // --ip
            if (have_domain) die("cannot specify both --ip and --domain");
            have_ip = true;
            // We still pass it through getaddrinfo, but validate format (optional)
            strncpy(g.cfg.host, optarg, sizeof(g.cfg.host) - 1);
            g.cfg.host[sizeof(g.cfg.host) - 1] = '\0';
        } else if (opt == 3) { // --domain
            if (have_ip) die("cannot specify both --ip and --domain");
            have_domain = true;
            strncpy(g.cfg.host, optarg, sizeof(g.cfg.host) - 1);
            g.cfg.host[sizeof(g.cfg.host) - 1] = '\0';
        } else if (opt == 4) { // --quiet
            g.cfg.quiet = true;
        } else if (opt == 5) { // --tui
            g.cfg.tui = true;
	} else {
            usage(stderr);
            exit(1);
        }
    }

    if (optind < argc) {
        fprintf(stderr, "Error: unexpected argument: %s\n", argv[optind]);
        usage(stderr);
        exit(1);
    }

    // If user explicitly provided --ip, validate it's IPv4
    if (have_ip) {
        struct in_addr a;
        if (inet_pton(AF_INET, g.cfg.host, &a) != 1) die("invalid IPv4 address for --ip");
    }
}

// ---------------- Main ----------------

static void cleanup(void) {
    if (g.fd >= 0) {
        close(g.fd);
        g.fd = -1;
    }
    pthread_mutex_destroy(&g.io_lock);
}

int main(int argc, char **argv) {
    if (pthread_mutex_init(&g.io_lock, NULL) != 0) {
        die("mutex init failed");
    }

    install_signals();
    parse_args(argc, argv);

    if (load_username(g.username) != 0) {
        die("failed to determine username (must be alnum and <32 chars)");
    }

    // connect
    g.fd = connect_to_server(g.cfg.host, g.cfg.port);
    if (g.fd < 0) {
        cleanup();
        die("connect failed");
    }
    g.connected = 1;

    // login
    if (send_login(g.fd, g.username) != 0) {
        cleanup();
        die("failed to send login");
    }

    // receiver thread
    if (pthread_create(&g.recv_thread, NULL, receiver_main, NULL) != 0) {
        cleanup();
        die("failed to create receiver thread");
    }
    g.recv_thread_started = true;
     if (g.cfg.tui) {
    tui_loop();   // <-- REAL TUI    
} else {
    // STDIN/STDOUT mode (your existing behavior)
    char *line = NULL;
    size_t cap = 0;

    while (!g.stop_requested) {
        errno = 0;
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) {
            if (errno == EINTR) {
                clearerr(stdin);
                continue;
            }
            g.stop_requested = 1;
            break;
        }

        if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';

        size_t L = 0;
        if (!validate_line(line, &L)) {
            fprintf(stderr, "Error: message must be 1..1023 printable characters\n");
            continue;
        }

        if (send_chat(g.fd, line) != 0) {
            fprintf(stderr, "Error: send failed: %s\n", strerror(errno));
            g.stop_requested = 1;
            break;
        }
    }

    free(line);
}}
