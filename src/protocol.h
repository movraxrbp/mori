#ifndef MORI_PROTOCOL_H
#define MORI_PROTOCOL_H
#include <stddef.h>
#include <stdio.h>
#define MORI_LINE 4096
/* Requests are objects with string-valued command and optional argument. */
int request_parse(const char *s, char *command, size_t nc, char *argument, size_t na);
void json_string(FILE *out, const char *s);
int socket_path(char *out, size_t size);
#endif
