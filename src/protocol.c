#include "protocol.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static void space(const char **s) {
    while (isspace((unsigned char)**s))
        ++*s;
}

static int string(const char **s, char *out, size_t cap) {
    size_t n = 0;
    if (*(*s)++ != '"')
        return 0;
    while (**s && **s != '"') {
        unsigned char c = (unsigned char)*(*s)++;
        if (c < 32)
            return 0;
        if (c == '\\') {
            c = (unsigned char)*(*s)++;
            switch (c) {
            case '"':
            case '\\':
            case '/':
                break;
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case 'b':
                c = '\b';
                break;
            case 'f':
                c = '\f';
                break;
            case 'u': { /* Accept escaped ASCII; UTF-8 can be sent literally. */
                unsigned v = 0;
                for (int i = 0; i < 4; ++i) {
                    unsigned char h = (unsigned char)**s;
                    if (!isxdigit(h))
                        return 0;
                    ++*s;
                    v = v * 16 +
                        (isdigit(h) ? (unsigned)(h - '0') : (unsigned)(tolower(h) - 'a' + 10));
                }
                if (!v || v > 127)
                    return 0;
                c = (unsigned char)v;
                break;
            }
            default:
                return 0;
            }
        }
        if (n + 1 >= cap)
            return 0;
        out[n++] = (char)c;
    }
    if (**s != '"')
        return 0;
    ++*s;
    out[n] = 0;
    return 1;
}

int request_parse(const char *s, char *command, size_t nc, char *argument, size_t na) {
    int seen = 0;
    char key[32];
    if (!nc || !na)
        return 0;
    command[0] = argument[0] = 0;
    space(&s);
    if (*s++ != '{')
        return 0;
    for (;;) {
        space(&s);
        if (!string(&s, key, sizeof key))
            return 0;
        space(&s);
        if (*s++ != ':')
            return 0;
        space(&s);
        if (!strcmp(key, "command")) {
            if ((seen & 1) || !string(&s, command, nc))
                return 0;
            seen |= 1;
        } else if (!strcmp(key, "argument")) {
            if ((seen & 2) || !string(&s, argument, na))
                return 0;
            seen |= 2;
        } else
            return 0;
        space(&s);
        if (*s == '}') {
            ++s;
            break;
        }
        if (*s++ != ',')
            return 0;
    }
    space(&s);
    return !*s && (seen & 1) && command[0];
}

void json_string(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; ++p) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', out);
            fputc(*p, out);
        } else if (*p < 32)
            fprintf(out, "\\u%04x", *p);
        else
            fputc(*p, out);
    }
    fputc('"', out);
}

int socket_path(char *out, size_t size) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir)
        return 0;
    int n = snprintf(out, size, "%s/mori.sock", dir);
    return n > 0 && (size_t)n < size;
}
