#include "embcli/embcli.h"
#include "embcli/embcli_telnet.h"
#include "demo_app.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DEMO_HOST "127.0.0.1"
#define DEMO_PORT 2423
#define DEMO_FEATURE_MAX 32768
#define DEMO_STRESS_CAPACITY(bytes_per_loop, loops) (((bytes_per_loop) * (loops)) + 8192U)

typedef enum demo_telnet_state {
    DEMO_TELNET_DATA = 0,
    DEMO_TELNET_IAC,
    DEMO_TELNET_IAC_OPTION,
    DEMO_TELNET_SB,
    DEMO_TELNET_SB_IAC
} demo_telnet_state_t;

typedef enum demo_ansi_state {
    DEMO_ANSI_NONE = 0,
    DEMO_ANSI_ESC,
    DEMO_ANSI_CSI
} demo_ansi_state_t;

typedef struct demo_filter_state {
    demo_telnet_state_t telnet;
    demo_ansi_state_t ansi;
} demo_filter_state_t;

typedef struct demo_server_ctx {
    embcli_t cli;
    embcli_telnet_server_t server;
    uint16_t port;
    int max_clients;
} demo_server_ctx_t;

typedef struct stress_worker_args {
    uint16_t port;
    int worker_id;
    int loops;
    bool ok;
    char error[256];
} stress_worker_args_t;

static void demo_append_char(char *buffer, size_t capacity, size_t *length, char ch) {
    if (*length + 1 >= capacity) {
        return;
    }
    buffer[*length] = ch;
    ++(*length);
    buffer[*length] = '\0';
}

static void demo_filter_bytes(
    demo_filter_state_t *state,
    const unsigned char *input,
    size_t input_len,
    char *output,
    size_t output_capacity,
    size_t *output_len) {
    for (size_t index = 0; index < input_len; ++index) {
        unsigned char byte = input[index];

        switch (state->telnet) {
        case DEMO_TELNET_DATA:
            break;
        case DEMO_TELNET_IAC:
            if (byte == 250U) {
                state->telnet = DEMO_TELNET_SB;
            } else if (byte == 251U || byte == 252U || byte == 253U || byte == 254U) {
                state->telnet = DEMO_TELNET_IAC_OPTION;
            } else {
                state->telnet = DEMO_TELNET_DATA;
            }
            continue;
        case DEMO_TELNET_IAC_OPTION:
            state->telnet = DEMO_TELNET_DATA;
            continue;
        case DEMO_TELNET_SB:
            if (byte == 255U) {
                state->telnet = DEMO_TELNET_SB_IAC;
            }
            continue;
        case DEMO_TELNET_SB_IAC:
            state->telnet = (byte == 240U) ? DEMO_TELNET_DATA : DEMO_TELNET_SB;
            continue;
        }

        if (byte == 255U) {
            state->telnet = DEMO_TELNET_IAC;
            continue;
        }

        switch (state->ansi) {
        case DEMO_ANSI_NONE:
            break;
        case DEMO_ANSI_ESC:
            if (byte == '[') {
                state->ansi = DEMO_ANSI_CSI;
            } else {
                state->ansi = DEMO_ANSI_NONE;
            }
            continue;
        case DEMO_ANSI_CSI:
            if (byte >= 0x40U && byte <= 0x7eU) {
                state->ansi = DEMO_ANSI_NONE;
            }
            continue;
        }

        if (byte == 0x1bU) {
            state->ansi = DEMO_ANSI_ESC;
            continue;
        }
        if (byte == '\r' || byte == '\a') {
            continue;
        }
        if (byte == '\n' || byte == '\t' || (byte >= 32U && byte <= 126U)) {
            demo_append_char(output, output_capacity, output_len, (char)byte);
        }
    }
}

static bool demo_send_all(int fd, const void *data, size_t len) {
    const char *cursor = (const char *)data;
    while (len > 0) {
        ssize_t written = send(fd, cursor, len, 0);
        if (written <= 0) {
            return false;
        }
        cursor += (size_t)written;
        len -= (size_t)written;
    }
    return true;
}

static bool demo_send_text(int fd, const char *text) {
    return demo_send_all(fd, text, strlen(text));
}

static bool demo_send_line(int fd, const char *line) {
    return demo_send_all(fd, line, strlen(line)) && demo_send_all(fd, "\r", 1);
}

static int demo_connect_client(uint16_t port) {
    int fd;
    struct sockaddr_in address;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, DEMO_HOST, &address.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static ssize_t demo_recv_quiet(
    int fd,
    char *buffer,
    size_t capacity,
    int total_timeout_ms,
    int quiet_timeout_ms) {
    demo_filter_state_t state;
    size_t length = 0;
    int elapsed_ms = 0;
    int idle_ms = 0;
    bool seen_data = false;

    memset(&state, 0, sizeof(state));
    if (capacity > 0) {
        buffer[0] = '\0';
    }

    while (elapsed_ms < total_timeout_ms) {
        fd_set readfds;
        struct timeval tv;
        int ready;
        unsigned char raw[1024];
        ssize_t received;

        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;

        ready = select(fd + 1, &readfds, NULL, NULL, &tv);
        elapsed_ms += 100;

        if (ready < 0) {
            return -1;
        }
        if (ready == 0) {
            if (seen_data) {
                idle_ms += 100;
                if (idle_ms >= quiet_timeout_ms) {
                    break;
                }
            }
            continue;
        }

        received = recv(fd, raw, sizeof(raw), 0);
        if (received <= 0) {
            break;
        }

        seen_data = true;
        idle_ms = 0;
        demo_filter_bytes(&state, raw, (size_t)received, buffer, capacity, &length);
    }

    return (ssize_t)length;
}

static int demo_count_occurrence(const char *haystack, const char *needle) {
    int count = 0;
    const char *cursor = haystack;

    while ((cursor = strstr(cursor, needle)) != NULL) {
        ++count;
        cursor += strlen(needle);
    }
    return count;
}

static bool demo_expect_contains(const char *output, const char *needle, const char *label) {
    if (strstr(output, needle) == NULL) {
        fprintf(stderr, "expectation failed: %s\nmissing: %s\noutput:\n%s\n", label, needle, output);
        return false;
    }
    return true;
}

static bool demo_open_session(uint16_t port, int *fd_out, char *buffer, size_t capacity) {
    int fd = demo_connect_client(port);
    char fallback[4096];
    char *read_buffer = buffer;
    size_t read_capacity = capacity;

    if (fd < 0) {
        fprintf(stderr, "failed to connect demo session\n");
        return false;
    }

    if (read_buffer == NULL || read_capacity == 0U) {
        read_buffer = fallback;
        read_capacity = sizeof(fallback);
    }

    if (demo_recv_quiet(fd, read_buffer, read_capacity, 3000, 200) < 0) {
        close(fd);
        fprintf(stderr, "failed to read demo banner\n");
        return false;
    }
    if (strstr(read_buffer, "board> ") == NULL) {
        close(fd);
        fprintf(stderr, "missing initial prompt\noutput:\n%s\n", read_buffer);
        return false;
    }
    *fd_out = fd;
    return true;
}

static bool demo_start_server(demo_server_ctx_t *ctx, uint16_t port, int max_clients) {
    embcli_telnet_config_t config;

    memset(ctx, 0, sizeof(*ctx));
    build_demo_cli(&ctx->cli);

    ctx->port = port;
    ctx->max_clients = max_clients;

    config.cli = &ctx->cli;
    config.bind_address = DEMO_HOST;
    config.port = port;
    config.backlog = max_clients > 4 ? max_clients : 4;
    config.max_clients = max_clients;

    if (embcli_telnet_server_start(&ctx->server, &config) != 0) {
        fprintf(stderr, "failed to start demo server on %u\n", port);
        return false;
    }

    {
        struct timeval delay;
        delay.tv_sec = 0;
        delay.tv_usec = 200000;
        select(0, NULL, NULL, NULL, &delay);
    }
    return true;
}

static void demo_stop_server(demo_server_ctx_t *ctx) {
    embcli_telnet_server_stop(&ctx->server);
}

static bool test_banner_and_root(uint16_t port) {
    int fd;
    char output[DEMO_FEATURE_MAX];

    if (!demo_open_session(port, &fd, output, sizeof(output))) {
        return false;
    }

    close(fd);
    return demo_expect_contains(output, "Embedded CLI demo over telnet", "banner") &&
           demo_expect_contains(output, "[board]", "root menu") &&
           demo_expect_contains(output, "system", "system menu listed") &&
           demo_expect_contains(output, "network", "network menu listed") &&
           demo_expect_contains(output, "device", "device menu listed") &&
           demo_expect_contains(output, "version", "version command listed");
}

static bool test_navigation_and_help(uint16_t port) {
    int fd;
    char output[DEMO_FEATURE_MAX];

    if (!demo_open_session(port, &fd, output, sizeof(output))) {
        return false;
    }

    if (!demo_send_line(fd, "help") ||
        !demo_send_line(fd, "system") ||
        !demo_send_line(fd, "help reboot") ||
        !demo_send_line(fd, "back") ||
        !demo_send_line(fd, "network") ||
        !demo_send_line(fd, "help config") ||
        !demo_send_line(fd, "exit")) {
        close(fd);
        return false;
    }

    if (demo_recv_quiet(fd, output, sizeof(output), 4000, 250) < 0) {
        close(fd);
        return false;
    }
    close(fd);

    return demo_expect_contains(output, "enter menu: system", "enter system") &&
           demo_expect_contains(output, "command : reboot", "help reboot") &&
           demo_expect_contains(output, "leave menu: system", "leave system") &&
           demo_expect_contains(output, "enter menu: network", "enter network") &&
           demo_expect_contains(output, "command : config", "help config");
}

static bool test_parameters_and_errors(uint16_t port) {
    int fd;
    char output[DEMO_FEATURE_MAX];

    if (!demo_open_session(port, &fd, output, sizeof(output))) {
        return false;
    }

    if (!demo_send_line(fd, "version") ||
        !demo_send_line(fd, "system") ||
        !demo_send_line(fd, "log-level warn") ||
        !demo_send_line(fd, "reboot 15 maintenance window") ||
        !demo_send_line(fd, "log-level verbose") ||
        !demo_send_line(fd, "back") ||
        !demo_send_line(fd, "network") ||
        !demo_send_line(fd, "config 192.168.10.2 255.255.255.0 192.168.10.1") ||
        !demo_send_line(fd, "back") ||
        !demo_send_line(fd, "device") ||
        !demo_send_line(fd, "set 2 on true") ||
        !demo_send_line(fd, "set 9 on") ||
        !demo_send_line(fd, "unknown-command") ||
        !demo_send_line(fd, "exit")) {
        close(fd);
        return false;
    }

    if (demo_recv_quiet(fd, output, sizeof(output), 5000, 300) < 0) {
        close(fd);
        return false;
    }
    close(fd);

    return demo_expect_contains(output, "demo-fw 1.0.0", "version output") &&
           demo_expect_contains(output, "log level => warn", "enum parsing") &&
           demo_expect_contains(output, "reboot scheduled: delay=15ms reason=maintenance window", "rest parsing") &&
           demo_expect_contains(output, "invalid enum for level: verbose", "invalid enum") &&
           demo_expect_contains(output, "network => ip=192.168.10.2 mask=255.255.255.0 gateway=192.168.10.1", "string params") &&
           demo_expect_contains(output, "led[2] => on=true blink=true", "bool params") &&
           demo_expect_contains(output, "out of range for id: 0 .. 7", "uint range") &&
           demo_expect_contains(output, "unknown command: unknown-command", "unknown command");
}

static bool test_completion_and_history(uint16_t port) {
    int fd;
    char output[DEMO_FEATURE_MAX];
    static const char script[] =
        "sy\t\r"
        "help \t\r"
        "help re\t\r"
        "log-level d\t\r"
        "reboot \t\r"
        "reboot 5 test auto completion\r"
        "back\r"
        "version\r"
        "\x1b[A\r"
        "device\r"
        "set \t\r"
        "set 1 of\t\r"
        "exit\r";

    if (!demo_open_session(port, &fd, output, sizeof(output))) {
        return false;
    }
    if (!demo_send_all(fd, script, sizeof(script) - 1U)) {
        close(fd);
        return false;
    }

    if (demo_recv_quiet(fd, output, sizeof(output), 6000, 300) < 0) {
        close(fd);
        return false;
    }
    close(fd);

    return demo_expect_contains(output, "log-level <level>", "help tab list") &&
           demo_expect_contains(output, "command : reboot", "help target completion") &&
           demo_expect_contains(output, "log level => debug", "enum completion execution") &&
           demo_expect_contains(output, "delay_ms", "uint hint") &&
           demo_expect_contains(output, "demo-fw 1.0.0", "history first execution") &&
           demo_count_occurrence(output, "demo-fw 1.0.0") >= 2 &&
           demo_expect_contains(output, "id", "parameter hint") &&
           demo_expect_contains(output, "led[1] => on=false blink=false", "bool completion execution");
}

static bool run_feature_demo(uint16_t port) {
    struct {
        const char *name;
        bool (*fn)(uint16_t port);
    } tests[] = {
        { "banner-and-root", test_banner_and_root },
        { "navigation-and-help", test_navigation_and_help },
        { "parameters-and-errors", test_parameters_and_errors },
        { "completion-and-history", test_completion_and_history }
    };

    for (size_t index = 0; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        bool ok = tests[index].fn(port);
        printf("[feature] %s: %s\n", tests[index].name, ok ? "PASS" : "FAIL");
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool run_sequential_stress(uint16_t port, int loops) {
    int fd;
    char script[4096];
    char output[16384];
    bool ok = false;

    if (!demo_open_session(port, &fd, NULL, 0)) {
        return false;
    }

    for (int index = 0; index < loops; ++index) {
        int led_id = index % 8;
        int host_octet = (index % 200) + 10;
        snprintf(
            script,
            sizeof(script),
            "version\r"
            "system\r"
            "log-level debug\r"
            "reboot %d stress-%d pass\r"
            "back\r"
            "network\r"
            "config 192.168.%d.%d 255.255.255.0 192.168.%d.1\r"
            "back\r"
            "device\r"
            "set %d on false\r"
            "back\r",
            (index % 30) + 1,
            index,
            index % 10,
            host_octet,
            index % 10,
            led_id);

        if (!demo_send_all(fd, script, strlen(script))) {
            goto cleanup;
        }
        if (demo_recv_quiet(fd, output, sizeof(output), 5000, 250) < 0) {
            goto cleanup;
        }

        if (!demo_expect_contains(output, "demo-fw 1.0.0", "sequential version") ||
            !demo_expect_contains(output, "log level => debug", "sequential log-level") ||
            !demo_expect_contains(output, "reboot scheduled:", "sequential reboot") ||
            !demo_expect_contains(output, "network =>", "sequential network") ||
            !demo_expect_contains(output, "led[", "sequential led")) {
            goto cleanup;
        }
    }

    if (!demo_send_line(fd, "exit")) {
        goto cleanup;
    }
    if (demo_recv_quiet(fd, output, sizeof(output), 2000, 200) < 0) {
        goto cleanup;
    }
    ok = true;

cleanup:
    if (!ok) {
        fprintf(stderr, "sequential stress failed\noutput excerpt:\n%.2048s\n", output);
    }
    if (fd >= 0) {
        close(fd);
    }
    return ok;
}

static void *run_stress_worker(void *arg) {
    stress_worker_args_t *worker = (stress_worker_args_t *)arg;
    int fd = -1;
    char banner[4096];
    char script[4096];
    char output[16384];

    worker->ok = false;
    worker->error[0] = '\0';

    if (!demo_open_session(worker->port, &fd, banner, sizeof(banner))) {
        snprintf(worker->error, sizeof(worker->error), "connect/open failed");
        return NULL;
    }

    for (int index = 0; index < worker->loops; ++index) {
        int led_id = (worker->worker_id + index) % 8;
        snprintf(
            script,
            sizeof(script),
            "system\r"
            "log-level info\r"
            "reboot %d worker-%d-step-%d\r"
            "back\r"
            "device\r"
            "set %d off true\r"
            "back\r"
            "network\r"
            "config 10.%d.%d.%d 255.255.0.0 10.%d.0.1\r"
            "back\r"
            "version\r",
            (index % 20) + 1,
            worker->worker_id,
            index,
            led_id,
            worker->worker_id,
            index % 250,
            led_id + 1,
            worker->worker_id);

        if (!demo_send_all(fd, script, strlen(script))) {
            snprintf(worker->error, sizeof(worker->error), "send failed");
            goto cleanup;
        }
        if (demo_recv_quiet(fd, output, sizeof(output), 5000, 250) < 0) {
            snprintf(worker->error, sizeof(worker->error), "recv failed");
            goto cleanup;
        }

        if (strstr(output, "log level => info") == NULL ||
            strstr(output, "reboot scheduled:") == NULL ||
            strstr(output, "network =>") == NULL ||
            strstr(output, "led[") == NULL ||
            strstr(output, "demo-fw 1.0.0") == NULL) {
            snprintf(worker->error, sizeof(worker->error), "unexpected output in loop %d", index);
            goto cleanup;
        }
    }

    if (!demo_send_line(fd, "exit")) {
        snprintf(worker->error, sizeof(worker->error), "exit failed");
        goto cleanup;
    }
    if (demo_recv_quiet(fd, output, sizeof(output), 2000, 200) < 0) {
        snprintf(worker->error, sizeof(worker->error), "exit recv failed");
        goto cleanup;
    }

    worker->ok = true;

cleanup:
    if (fd >= 0) {
        close(fd);
    }
    return NULL;
}

static bool run_parallel_stress(uint16_t port, int clients, int loops) {
    pthread_t *threads = NULL;
    stress_worker_args_t *workers = NULL;
    bool ok = true;

    threads = (pthread_t *)calloc((size_t)clients, sizeof(*threads));
    workers = (stress_worker_args_t *)calloc((size_t)clients, sizeof(*workers));
    if (threads == NULL || workers == NULL) {
        free(threads);
        free(workers);
        return false;
    }

    for (int index = 0; index < clients; ++index) {
        workers[index].port = port;
        workers[index].worker_id = index;
        workers[index].loops = loops;
        if (pthread_create(&threads[index], NULL, run_stress_worker, &workers[index]) != 0) {
            ok = false;
            workers[index].ok = false;
            snprintf(workers[index].error, sizeof(workers[index].error), "pthread_create failed");
            clients = index;
            break;
        }
    }

    for (int index = 0; index < clients; ++index) {
        pthread_join(threads[index], NULL);
        if (!workers[index].ok) {
            ok = false;
            fprintf(stderr, "parallel stress worker %d failed: %s\n", index, workers[index].error);
        }
    }

    free(threads);
    free(workers);
    return ok;
}

static bool run_saturation_test(uint16_t port) {
    int fd1 = -1;
    int fd2 = -1;
    int fd3 = -1;
    char output[8192];
    bool ok = false;

    printf("[saturation] open session #1\n");
    fflush(stdout);
    if (!demo_open_session(port, &fd1, output, sizeof(output))) {
        return false;
    }
    printf("[saturation] open session #2\n");
    fflush(stdout);
    if (!demo_open_session(port, &fd2, output, sizeof(output))) {
        close(fd1);
        return false;
    }

    printf("[saturation] open session #3 expecting busy\n");
    fflush(stdout);
    fd3 = demo_connect_client(port);
    if (fd3 < 0) {
        goto cleanup;
    }
    if (demo_recv_quiet(fd3, output, sizeof(output), 3000, 200) < 0) {
        goto cleanup;
    }

    ok = strstr(output, "server busy") != NULL;
cleanup:
    if (fd1 >= 0) {
        close(fd1);
    }
    if (fd2 >= 0) {
        close(fd2);
    }
    if (fd3 >= 0) {
        close(fd3);
    }
    if (!ok) {
        fprintf(stderr, "saturation test failed\n");
    }
    return ok;
}

int main(int argc, char **argv) {
    demo_server_ctx_t server;
    demo_server_ctx_t saturated_server;
    uint16_t port = DEMO_PORT;
    int sequential_loops = 40;
    int parallel_clients = 6;
    int parallel_loops = 20;
    bool ok = true;

    if (argc > 1) {
        port = (uint16_t)strtoul(argv[1], NULL, 10);
    }
    if (argc > 2) {
        sequential_loops = atoi(argv[2]);
    }
    if (argc > 3) {
        parallel_clients = atoi(argv[3]);
    }
    if (argc > 4) {
        parallel_loops = atoi(argv[4]);
    }

    printf("demo selftest start: port=%u sequential_loops=%d parallel_clients=%d parallel_loops=%d\n",
           port, sequential_loops, parallel_clients, parallel_loops);

    if (!demo_start_server(&server, port, parallel_clients + 2)) {
        return 1;
    }

    printf("[phase] feature-demo start\n");
    fflush(stdout);
    ok = run_feature_demo(port);
    printf("[phase] feature-demo done: %s\n", ok ? "PASS" : "FAIL");
    fflush(stdout);

    if (ok) {
        bool sequential_ok = run_sequential_stress(port, sequential_loops);
        printf("[stress] sequential: %s\n", sequential_ok ? "PASS" : "FAIL");
        fflush(stdout);
        ok = sequential_ok;
    }
    if (ok) {
        bool parallel_ok = run_parallel_stress(port, parallel_clients, parallel_loops);
        printf("[stress] parallel: %s\n", parallel_ok ? "PASS" : "FAIL");
        fflush(stdout);
        ok = parallel_ok;
    }

    printf("[phase] stop primary server\n");
    fflush(stdout);
    demo_stop_server(&server);
    printf("[phase] primary server stopped\n");
    fflush(stdout);

    printf("[phase] saturation start\n");
    fflush(stdout);
    if (ok && demo_start_server(&saturated_server, (uint16_t)(port + 1), 2)) {
        bool saturated_ok = run_saturation_test((uint16_t)(port + 1));
        printf("[stress] saturation: %s\n", saturated_ok ? "PASS" : "FAIL");
        fflush(stdout);
        ok = saturated_ok;
        demo_stop_server(&saturated_server);
        printf("[phase] saturation server stopped\n");
        fflush(stdout);
    } else if (ok) {
        ok = false;
    }

    printf("demo selftest result: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
