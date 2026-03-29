#ifndef EMBCLI_EMBCLI_TELNET_H
#define EMBCLI_EMBCLI_TELNET_H

#include <stdbool.h>
#include <pthread.h>
#include <stdint.h>

#include "embcli.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct embcli_telnet_config {
    embcli_t *cli;
    const char *bind_address;
    uint16_t port;
    int backlog;
    int max_clients;
} embcli_telnet_config_t;

typedef struct embcli_telnet_server {
    embcli_telnet_config_t config;
    int listen_fd;
    bool running;
    pthread_t accept_thread;
    pthread_mutex_t lock;
    int active_clients;
} embcli_telnet_server_t;

int embcli_telnet_server_start(
    embcli_telnet_server_t *server,
    const embcli_telnet_config_t *config);
void embcli_telnet_server_stop(embcli_telnet_server_t *server);
bool embcli_telnet_server_is_running(embcli_telnet_server_t *server);
int embcli_telnet_server_active_clients(embcli_telnet_server_t *server);

#ifdef __cplusplus
}
#endif

#endif
