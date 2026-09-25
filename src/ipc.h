#ifndef MORI_IPC_H
#define MORI_IPC_H

#include <poll.h>
#include <stdbool.h>

#define IPC_MAX_PEERS 32
#define IPC_POLL_COUNT (IPC_MAX_PEERS + 1)
struct Client;

bool ipc_start(void);
void ipc_stop(void);
void ipc_prepare(struct pollfd fds[IPC_POLL_COUNT]);
void ipc_dispatch(const struct pollfd fds[IPC_POLL_COUNT]);
void ipc_emit(const char *event, const struct Client *client);

#endif
