#include "embcli/embcli_telnet.h"
#include "demo_app.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void demo_handle_signal(int signo) {
    (void)signo;
    g_stop = 1;
}

static uint16_t parse_port_or_default(const char *text, uint16_t fallback) {
    char *end = NULL;
    unsigned long value;

    if (text == NULL) {
        return fallback;
    }

    value = strtoul(text, &end, 10);
    if (end == NULL || *end != '\0' || value == 0 || value > 65535UL) {
        return fallback;
    }

    return (uint16_t)value;
}

int main(int argc, char **argv) {
    embcli_t cli;
    embcli_telnet_server_t server;
    embcli_telnet_config_t config;
    const char *bind_address = "0.0.0.0";
    uint16_t port = 2323;

    if (argc > 1) {
        port = parse_port_or_default(argv[1], 2323);
    }
    if (argc > 2 && argv[2][0] != '\0') {
        bind_address = argv[2];
    }

    build_demo_cli(&cli);

    config.cli = &cli;
    config.bind_address = bind_address;
    config.port = port;
    config.backlog = 4;
    config.max_clients = 4;

    signal(SIGINT, demo_handle_signal);
    signal(SIGTERM, demo_handle_signal);

    if (embcli_telnet_server_start(&server, &config) != 0) {
        fprintf(stderr, "failed to start telnet server on port %u\n", config.port);
        return 1;
    }

    printf("embcli demo listening on telnet %s:%u\n", config.bind_address, config.port);
    printf("try: telnet 127.0.0.1 %u\n", config.port);
    printf("press Ctrl+C to stop\n");

    while (!g_stop) {
        sleep(1);
    }

    embcli_telnet_server_stop(&server);
    embcli_deinit(&cli);
    return 0;
}
