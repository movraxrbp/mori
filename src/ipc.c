#include "mori.h"
#include "ipc.h"
#include "protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_PEER IPC_MAX_PEERS
#define OUT_SIZE 65536

typedef struct {
    int fd;
    bool subscribed;
    char in[MORI_LINE], out[OUT_SIZE];
    size_t used, queued, sent;
} Peer;

static Peer peers[MAX_PEER];
static int listener = -1;
static char ipc_path[108];
static bool initialized;

static void drop(Peer *p) {
    if (p->fd >= 0)
        close(p->fd);
    p->fd = -1;
    p->used = p->queued = p->sent = 0;
    p->subscribed = false;
}

static void queue(Peer *p, const char *data, size_t n) {
    if (p->fd < 0)
        return;
    if (p->sent) {
        memmove(p->out, p->out + p->sent, p->queued - p->sent);
        p->queued -= p->sent;
        p->sent = 0;
    }
    if (n > sizeof p->out - p->queued) {
        drop(p);
        return;
    }
    memcpy(p->out + p->queued, data, n);
    p->queued += n;
}

static void flush_peer(Peer *p) {
    while (p->fd >= 0 && p->sent < p->queued) {
        ssize_t n = write(p->fd, p->out + p->sent, p->queued - p->sent);
        if (n > 0)
            p->sent += (size_t)n;
        else if (n < 0 && errno == EINTR)
            continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        else {
            drop(p);
            break;
        }
    }
    if (p->sent == p->queued)
        p->sent = p->queued = 0;
}

void ipc_emit(const char *event, const Client *c) {
    char *data = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&data, &size);
    if (out) {
        fputs("{\"event\":", out);
        json_string(out, event);
        fprintf(out, ",\"workspace\":%d,\"monitor\":%d,\"window\":%lu}\n",
                monitors[selected_monitor].workspace + 1, selected_monitor + 1, c ? c->win : 0UL);
        fclose(out);
        for (int i = 0; i < MAX_PEER; ++i)
            if (peers[i].subscribed)
                queue(&peers[i], data, size);
        free(data);
    }
}

static void respond(Peer *p, const char *request) {
    char cmd[64], arg[2048];
    const char *error = NULL;
    char *data = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&data, &size);
    if (!out) {
        drop(p);
        return;
    }
    if (!request_parse(request, cmd, sizeof cmd, arg, sizeof arg))
        error = "invalid request: expected command and optional argument strings";
    else if (!strcmp(cmd, "workspaces")) {
        fputs("{\"ok\":true,\"workspaces\":[", out);
        for (int i = 0; i < config.count; ++i) {
            if (i)
                fputc(',', out);
            int mi = workspace_monitor(i);
            fprintf(out, "{\"id\":%d,\"name\":", i + 1);
            json_string(out, config.names[i]);
            fputs(",\"layout\":", out);
            json_string(out, config.layouts[i]);
            fprintf(out, ",\"visible\":%s,\"focused\":%s,\"monitor\":%d}",
                    mi >= 0 ? "true" : "false",
                    monitors[selected_monitor].workspace == i ? "true" : "false", mi + 1);
        }
        fputs("]}\n", out);
    } else if (!strcmp(cmd, "windows")) {
        fputs("{\"ok\":true,\"windows\":[", out);
        bool comma = false;
        for (Client *c = clients; c; c = c->next) {
            if (comma)
                fputc(',', out);
            comma = true;
            fprintf(out, "{\"id\":%lu,\"workspace\":%d,\"title\":", c->win, c->workspace + 1);
            json_string(out, c->title);
            fputs(",\"class\":", out);
            json_string(out, c->class);
            fprintf(out,
                    ",\"focused\":%s,\"floating\":%s,\"fullscreen\":%s,\"x\":%d,\"y\":%d,\"width\":"
                    "%d,\"height\":%d}",
                    c == focused ? "true" : "false", c->floating ? "true" : "false",
                    c->fullscreen ? "true" : "false", c->x, c->y, c->w, c->h);
        }
        fputs("]}\n", out);
    } else if (!strcmp(cmd, "monitors")) {
        fputs("{\"ok\":true,\"monitors\":[", out);
        for (int i = 0; i < nmonitors; ++i) {
            Monitor *m = &monitors[i];
            if (i)
                fputc(',', out);
            fprintf(out, "{\"id\":%d,\"name\":", i + 1);
            json_string(out, m->name);
            fprintf(
                out,
                ",\"workspace\":%d,\"focused\":%s,\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}",
                m->workspace + 1, i == selected_monitor ? "true" : "false", m->x, m->y, m->w, m->h);
        }
        fputs("]}\n", out);
    } else if (!strcmp(cmd, "subscribe")) {
        p->subscribed = true;
        fputs("{\"ok\":true}\n", out);
    } else {
        if (!strcmp(cmd, "reload")) {
            if (!wm_reload())
                error = "config reload failed; previous config retained";
        } else
            error = wm_command(cmd, arg);
        if (!error)
            fputs("{\"ok\":true}\n", out);
    }
    if (error) {
        fputs("{\"ok\":false,\"error\":", out);
        json_string(out, error);
        fputs("}\n", out);
    }
    fclose(out);
    queue(p, data, size);
    free(data);
}

static void read_peer(Peer *p) {
    /* Bound per-peer work to one read per event-loop iteration. */
    ssize_t n = read(p->fd, p->in + p->used, sizeof p->in - p->used - 1);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return;
    if (n <= 0) {
        drop(p);
        return;
    }
    p->used += (size_t)n;
    p->in[p->used] = 0;
    if (memchr(p->in, 0, p->used)) {
        drop(p);
        return;
    }
    char *end;
    while (p->fd >= 0 && (end = memchr(p->in, '\n', p->used))) {
        size_t consumed = (size_t)(end - p->in) + 1;
        *end = 0;
        respond(p, p->in);
        if (p->fd < 0)
            return;
        memmove(p->in, p->in + consumed, p->used - consumed);
        p->used -= consumed;
        p->in[p->used] = 0;
    }
    if (p->used == sizeof p->in - 1)
        drop(p);
}

static bool nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0 &&
           fcntl(fd, F_SETFD, FD_CLOEXEC) >= 0;
}

bool ipc_start(void) {
    for (int i = 0; i < MAX_PEER; ++i)
        peers[i].fd = -1;
    initialized = true;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (!socket_path(addr.sun_path, sizeof addr.sun_path)) {
        fprintf(stderr, "mori: XDG_RUNTIME_DIR must be set (socket path must fit 107 bytes)\n");
        return false;
    }
    const char *dir = getenv("XDG_RUNTIME_DIR");
    struct stat st;
    if (stat(dir, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 0077)) {
        fprintf(
            stderr,
            "mori: XDG_RUNTIME_DIR must be an existing directory owned by you with mode 0700\n");
        return false;
    }
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0 || !nonblock(listener)) {
        perror("mori: socket");
        return false;
    }
    mode_t mask = umask(0077);
    int rc = bind(listener, (struct sockaddr *)&addr, sizeof addr);
    umask(mask);
    if (rc < 0) {
        perror("mori: bind (an existing socket is never removed automatically)");
        return false;
    }
    copy(ipc_path, sizeof ipc_path, addr.sun_path);
    if (listen(listener, MAX_PEER) < 0) {
        perror("mori: listen");
        return false;
    }
    return true;
}

void ipc_stop(void) {
    if (!initialized)
        return;
    for (int i = 0; i < MAX_PEER; ++i) {
        flush_peer(&peers[i]);
        drop(&peers[i]);
    }
    if (listener >= 0)
        close(listener);
    if (ipc_path[0])
        unlink(ipc_path);
    listener = -1;
    ipc_path[0] = 0;
    initialized = false;
}

void ipc_prepare(struct pollfd fds[IPC_POLL_COUNT]) {
    fds[0] = (struct pollfd){.fd = listener, .events = POLLIN};
    for (int i = 0; i < MAX_PEER; ++i)
        fds[i + 1] =
            (struct pollfd){.fd = peers[i].fd, .events = POLLIN | (peers[i].queued ? POLLOUT : 0)};
}

void ipc_dispatch(const struct pollfd fds[IPC_POLL_COUNT]) {
    /* Process existing peers before accepting: poll results refer to old fds. */
    for (int i = 0; i < MAX_PEER; ++i) {
        Peer *p = &peers[i];
        short revents = fds[i + 1].revents;
        if (p->fd < 0 || p->fd != fds[i + 1].fd)
            continue;
        if (revents & POLLIN)
            read_peer(p);
        if (p->fd >= 0 && p->queued)
            flush_peer(p);
        if (p->fd >= 0 && (revents & (POLLERR | POLLHUP | POLLNVAL)))
            drop(p);
    }
    if (fds[0].revents & POLLIN) {
        for (int budget = MAX_PEER; budget; --budget) {
            int fd = accept(listener, NULL, NULL);
            if (fd < 0)
                break;
            if (!nonblock(fd)) {
                close(fd);
                continue;
            }
            int i = 0;
            while (i < MAX_PEER && peers[i].fd >= 0)
                ++i;
            if (i == MAX_PEER)
                close(fd);
            else
                peers[i].fd = fd;
        }
    }
}
