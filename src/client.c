#include "protocol.h"
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: mori-ipc COMMAND [ARGUMENT]\n");
        return 2;
    }
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (!socket_path(addr.sun_path, sizeof addr.sun_path)) {
        fprintf(stderr, "mori-ipc: set XDG_RUNTIME_DIR to an existing private directory\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("mori-ipc: connect");
        if (fd >= 0)
            close(fd);
        return 1;
    }
    char *request = NULL;
    size_t size = 0;
    FILE *buf = open_memstream(&request, &size);
    if (!buf) {
        close(fd);
        return 1;
    }
    fputs("{\"command\":", buf);
    json_string(buf, argv[1]);
    fputs(",\"argument\":", buf);
    json_string(buf, argc == 3 ? argv[2] : "");
    fputs("}\n", buf);
    fclose(buf);
    if (size >= MORI_LINE) {
        fprintf(stderr, "mori-ipc: request too long\n");
        free(request);
        close(fd);
        return 2;
    }
    for (size_t sent = 0; sent < size;) {
        ssize_t n = write(fd, request + sent, size - sent);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            perror("mori-ipc: write");
            free(request);
            close(fd);
            return 1;
        }
        sent += (size_t)n;
    }
    free(request);
    FILE *in = fdopen(fd, "r");
    if (!in) {
        close(fd);
        return 1;
    }
    char *line = NULL;
    size_t cap = 0;
    int status = 0, received = 0;
    while (getline(&line, &cap, in) >= 0) {
        received = 1;
        if (strstr(line, "\"ok\":false"))
            status = 1;
        fputs(line, stdout);
        fflush(stdout);
        if (strcmp(argv[1], "subscribe") || status)
            break;
    }
    free(line);
    fclose(in);
    return received ? status : 1;
}
