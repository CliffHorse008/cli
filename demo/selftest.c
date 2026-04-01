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
#include <stdatomic.h>
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
#define DEMO_DYNAMIC_WRITERS 4
#define DEMO_DYNAMIC_READERS 4
#define DEMO_DYNAMIC_MENUS_PER_WRITER 16
#define DEMO_DYNAMIC_TOTAL_MENUS (DEMO_DYNAMIC_WRITERS * DEMO_DYNAMIC_MENUS_PER_WRITER)

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

/* 一个轻量测试桩对象，用于在进程内启动/停止 demo server。 */
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

typedef struct demo_mem_writer {
    char *buffer;
    size_t capacity;
    size_t length;
} demo_mem_writer_t;

typedef struct demo_thread_gate {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int total;
    int ready;
    bool open;
    bool aborted;
} demo_thread_gate_t;

typedef struct dynamic_entry {
    embcli_menu_t menu;
    embcli_command_t command;
    int command_id;
    char menu_name[32];
    char menu_summary[64];
    char command_summary[64];
} dynamic_entry_t;

typedef struct dynamic_test_ctx {
    embcli_t cli;
    demo_thread_gate_t gate;
    atomic_bool stop_readers;
    dynamic_entry_t entries[DEMO_DYNAMIC_TOTAL_MENUS];
} dynamic_test_ctx_t;

typedef struct dynamic_writer_args {
    dynamic_test_ctx_t *ctx;
    int writer_id;
    bool ok;
    char error[128];
} dynamic_writer_args_t;

typedef struct dynamic_reader_args {
    dynamic_test_ctx_t *ctx;
    int reader_id;
    bool ok;
    char error[128];
} dynamic_reader_args_t;

typedef struct parser_test_fixture {
    embcli_t cli;
    embcli_session_t session;
    demo_mem_writer_t writer;
    char output[8192];
} parser_test_fixture_t;

/*
 * selftest 会把 telnet 输出收敛成纯文本，
 * 这样断言就可以基于稳定字符串，而不是依赖终端控制序列。
 */
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
    /*
     * 过滤掉 telnet 协商字节和 ANSI 重绘噪声。
     * CLI 会频繁重画提示符，但测试只关心最终的语义文本。
     */
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

    /*
     * 持续读取，直到连接在一个短暂窗口内安静下来。
     * 这很适合 CLI 流量模型：每条命令都会产生一串输出，随后回到空闲 prompt。
     */
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

static ssize_t demo_recv_raw_quiet(
    int fd,
    char *buffer,
    size_t capacity,
    int total_timeout_ms,
    int quiet_timeout_ms) {
    size_t length = 0;
    int elapsed_ms = 0;
    int idle_ms = 0;
    bool seen_data = false;

    if (capacity > 0) {
        buffer[0] = '\0';
    }

    while (elapsed_ms < total_timeout_ms) {
        fd_set readfds;
        struct timeval tv;
        int ready;
        char raw[4096];
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

        if (buffer != NULL && capacity > 0 && length < capacity - 1U) {
            size_t writable = (size_t)received;
            if (writable > capacity - length - 1U) {
                writable = capacity - length - 1U;
            }
            memcpy(buffer + length, raw, writable);
            length += writable;
            buffer[length] = '\0';
        }
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

static bool demo_expect_not_contains(const char *output, const char *needle, const char *label) {
    if (strstr(output, needle) != NULL) {
        fprintf(stderr, "expectation failed: %s\nunexpected: %s\noutput:\n%s\n", label, needle, output);
        return false;
    }
    return true;
}

static void demo_mem_writer_reset(demo_mem_writer_t *writer) {
    writer->length = 0;
    if (writer->capacity > 0) {
        writer->buffer[0] = '\0';
    }
}

static void demo_mem_writer_write(void *ctx, const char *data, size_t len) {
    demo_mem_writer_t *writer = (demo_mem_writer_t *)ctx;
    size_t writable;

    if (writer == NULL || writer->buffer == NULL || writer->capacity == 0 || data == NULL || len == 0) {
        return;
    }
    if (writer->length >= writer->capacity - 1U) {
        return;
    }

    writable = len;
    if (writable > (writer->capacity - writer->length - 1U)) {
        writable = writer->capacity - writer->length - 1U;
    }

    memcpy(writer->buffer + writer->length, data, writable);
    writer->length += writable;
    writer->buffer[writer->length] = '\0';
}

static bool demo_thread_gate_init(demo_thread_gate_t *gate, int total) {
    if (pthread_mutex_init(&gate->mutex, NULL) != 0) {
        return false;
    }
    if (pthread_cond_init(&gate->cond, NULL) != 0) {
        pthread_mutex_destroy(&gate->mutex);
        return false;
    }
    gate->total = total;
    gate->ready = 0;
    gate->open = false;
    gate->aborted = false;
    return true;
}

static void demo_thread_gate_destroy(demo_thread_gate_t *gate) {
    pthread_cond_destroy(&gate->cond);
    pthread_mutex_destroy(&gate->mutex);
}

static void demo_thread_gate_abort(demo_thread_gate_t *gate) {
    pthread_mutex_lock(&gate->mutex);
    gate->aborted = true;
    gate->open = true;
    pthread_cond_broadcast(&gate->cond);
    pthread_mutex_unlock(&gate->mutex);
}

static bool demo_thread_gate_wait(demo_thread_gate_t *gate) {
    bool ok = true;

    pthread_mutex_lock(&gate->mutex);
    ++gate->ready;
    if (gate->ready >= gate->total) {
        gate->open = true;
        pthread_cond_broadcast(&gate->cond);
    }
    while (!gate->open) {
        pthread_cond_wait(&gate->cond, &gate->mutex);
    }
    if (gate->aborted) {
        ok = false;
    }
    pthread_mutex_unlock(&gate->mutex);
    return ok;
}

static void demo_dynamic_command(
    embcli_session_t *session,
    const embcli_value_t *values,
    size_t value_count,
    void *user_data) {
    int *command_id = (int *)user_data;

    (void)values;
    (void)value_count;
    embcli_session_printf(session, "dynamic command %d\r\n", *command_id);
}

static void parser_cmd_string(
    embcli_session_t *session,
    const embcli_value_t *values,
    size_t value_count,
    void *user_data) {
    (void)value_count;
    (void)user_data;
    embcli_session_printf(session, "string=%s\r\n", embcli_value_string(&values[0]));
}

static void parser_cmd_int(
    embcli_session_t *session,
    const embcli_value_t *values,
    size_t value_count,
    void *user_data) {
    (void)value_count;
    (void)user_data;
    embcli_session_printf(session, "int=%lld\r\n", (long long)values[0].as.i64);
}

static void parser_cmd_uint(
    embcli_session_t *session,
    const embcli_value_t *values,
    size_t value_count,
    void *user_data) {
    (void)value_count;
    (void)user_data;
    embcli_session_printf(session, "uint=%llu\r\n", (unsigned long long)values[0].as.u64);
}

static void parser_cmd_bool(
    embcli_session_t *session,
    const embcli_value_t *values,
    size_t value_count,
    void *user_data) {
    (void)value_count;
    (void)user_data;
    embcli_session_printf(session, "bool=%s\r\n", values[0].as.boolean ? "true" : "false");
}

static void parser_cmd_enum(
    embcli_session_t *session,
    const embcli_value_t *values,
    size_t value_count,
    void *user_data) {
    const char *const *table = (const char *const *)user_data;

    (void)value_count;
    embcli_session_printf(session, "enum=%s\r\n", table[values[0].as.enum_index]);
}

static void parser_cmd_rest(
    embcli_session_t *session,
    const embcli_value_t *values,
    size_t value_count,
    void *user_data) {
    (void)value_count;
    (void)user_data;
    embcli_session_printf(session, "rest=%s\r\n", embcli_value_string(&values[0]));
}

static void build_parser_test_cli(embcli_t *cli) {
    static const char *modes[] = { "alpha", "beta", "gamma" };
    static const embcli_arg_spec_t string_args[] = {
        EMBCLI_ARG_STRING_REQ("value", "single string token")
    };
    static const embcli_arg_spec_t int_args[] = {
        EMBCLI_ARG_INT_REQ("value", "signed integer", -10, 10)
    };
    static const embcli_arg_spec_t uint_args[] = {
        EMBCLI_ARG_UINT_REQ("value", "unsigned integer", 0, UINT64_MAX)
    };
    static const embcli_arg_spec_t bool_args[] = {
        EMBCLI_ARG_BOOL_REQ("value", "boolean")
    };
    static const embcli_arg_spec_t enum_args[] = {
        EMBCLI_ARG_ENUM_REQ("value", "alpha/beta/gamma", modes)
    };
    static const embcli_arg_spec_t rest_args[] = {
        EMBCLI_ARG_REST_REQ("value", "rest text")
    };
    static embcli_command_t cmd_string;
    static embcli_command_t cmd_int;
    static embcli_command_t cmd_uint;
    static embcli_command_t cmd_bool;
    static embcli_command_t cmd_enum;
    static embcli_command_t cmd_rest;

    embcli_init(cli, "parsertest", NULL);
    embcli_command_init(
        &cmd_string,
        "echo",
        "echo a single string",
        string_args,
        EMBCLI_ARRAY_SIZE(string_args),
        parser_cmd_string,
        NULL);
    embcli_command_init(
        &cmd_int,
        "signed",
        "parse signed integer",
        int_args,
        EMBCLI_ARRAY_SIZE(int_args),
        parser_cmd_int,
        NULL);
    embcli_command_init(
        &cmd_uint,
        "unsigned",
        "parse unsigned integer",
        uint_args,
        EMBCLI_ARRAY_SIZE(uint_args),
        parser_cmd_uint,
        NULL);
    embcli_command_init(
        &cmd_bool,
        "flag",
        "parse bool aliases",
        bool_args,
        EMBCLI_ARRAY_SIZE(bool_args),
        parser_cmd_bool,
        NULL);
    embcli_command_init(
        &cmd_enum,
        "mode",
        "parse enum values",
        enum_args,
        EMBCLI_ARRAY_SIZE(enum_args),
        parser_cmd_enum,
        (void *)modes);
    embcli_command_init(
        &cmd_rest,
        "note",
        "parse remaining text",
        rest_args,
        EMBCLI_ARRAY_SIZE(rest_args),
        parser_cmd_rest,
        NULL);

    embcli_menu_add_command(embcli_root_menu(cli), &cmd_string);
    embcli_menu_add_command(embcli_root_menu(cli), &cmd_int);
    embcli_menu_add_command(embcli_root_menu(cli), &cmd_uint);
    embcli_menu_add_command(embcli_root_menu(cli), &cmd_bool);
    embcli_menu_add_command(embcli_root_menu(cli), &cmd_enum);
    embcli_menu_add_command(embcli_root_menu(cli), &cmd_rest);
}

static void parser_test_fixture_init(parser_test_fixture_t *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    build_parser_test_cli(&fixture->cli);
    fixture->writer.buffer = fixture->output;
    fixture->writer.capacity = sizeof(fixture->output);
    demo_mem_writer_reset(&fixture->writer);
    embcli_session_init(&fixture->session, &fixture->cli, demo_mem_writer_write, &fixture->writer);
}

static void parser_test_fixture_deinit(parser_test_fixture_t *fixture) {
    embcli_deinit(&fixture->cli);
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

    /*
     * 某些压测路径只需要一个可用 socket，并不关心 banner 内容。
     * 用本地 fallback buffer 可以复用同一套建连逻辑。
     */
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
    build_demo_cli(&ctx->cli, &ctx->server);

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

static bool demo_wait_for_active_clients(
    embcli_telnet_server_t *server,
    int expected,
    int timeout_ms) {
    int elapsed_ms = 0;

    while (elapsed_ms <= timeout_ms) {
        if (embcli_telnet_server_active_clients(server) == expected) {
            return true;
        }

        {
            struct timeval delay;
            delay.tv_sec = 0;
            delay.tv_usec = 100000;
            select(0, NULL, NULL, NULL, &delay);
        }
        elapsed_ms += 100;
    }

    return false;
}

static void demo_stop_server(demo_server_ctx_t *ctx) {
    embcli_telnet_server_stop(&ctx->server);
    embcli_deinit(&ctx->cli);
}

static bool run_server_api_test(demo_server_ctx_t *ctx) {
    int fd = -1;
    char output[DEMO_FEATURE_MAX];
    bool ok = true;

    if (!embcli_telnet_server_is_running(&ctx->server)) {
        fprintf(stderr, "server api: expected running state after start\n");
        return false;
    }
    if (strcmp(embcli_telnet_server_bind_address(&ctx->server), DEMO_HOST) != 0) {
        fprintf(stderr, "server api: expected default bind address %s\n", DEMO_HOST);
        return false;
    }
    if (embcli_telnet_server_active_clients(&ctx->server) != 0) {
        fprintf(stderr, "server api: expected zero active clients before connect\n");
        return false;
    }

    if (!demo_open_session(ctx->port, &fd, output, sizeof(output))) {
        return false;
    }
    if (!demo_wait_for_active_clients(&ctx->server, 1, 2000)) {
        fprintf(stderr, "server api: active client count did not become 1\n");
        ok = false;
    }

    if (fd >= 0) {
        close(fd);
        fd = -1;
    }

    if (!demo_wait_for_active_clients(&ctx->server, 0, 2000)) {
        fprintf(stderr, "server api: active client count did not return to 0\n");
        ok = false;
    }

    if (embcli_telnet_server_rebind(&ctx->server, "0.0.0.0") != 0 ||
        strcmp(embcli_telnet_server_bind_address(&ctx->server), "0.0.0.0") != 0) {
        fprintf(stderr, "server api: failed to rebind to 0.0.0.0\n");
        ok = false;
    }
    if (ok &&
        (!demo_open_session(ctx->port, &fd, output, sizeof(output)) ||
         !demo_wait_for_active_clients(&ctx->server, 1, 2000))) {
        fprintf(stderr, "server api: failed to accept connection after public rebind\n");
        ok = false;
    }
    if (fd >= 0) {
        close(fd);
        fd = -1;
        (void)demo_wait_for_active_clients(&ctx->server, 0, 2000);
    }

    if (embcli_telnet_server_rebind(&ctx->server, DEMO_HOST) != 0 ||
        strcmp(embcli_telnet_server_bind_address(&ctx->server), DEMO_HOST) != 0) {
        fprintf(stderr, "server api: failed to rebind back to %s\n", DEMO_HOST);
        ok = false;
    }

    return ok;
}

static bool test_telnet_rebind_command(demo_server_ctx_t *ctx) {
    int fd;
    char output[DEMO_FEATURE_MAX];

    if (!demo_open_session(ctx->port, &fd, output, sizeof(output))) {
        return false;
    }

    if (!demo_send_line(fd, "system/telnet-access on") ||
        !demo_send_line(fd, "system/telnet-access off") ||
        !demo_send_line(fd, "exit")) {
        close(fd);
        return false;
    }

    if (demo_recv_quiet(fd, output, sizeof(output), 4000, 250) < 0) {
        close(fd);
        return false;
    }
    close(fd);

    return demo_expect_contains(output, "telnet bind => 0.0.0.0", "enable public telnet") &&
           demo_expect_contains(output, "telnet bind => 127.0.0.1", "disable public telnet") &&
           strcmp(embcli_telnet_server_bind_address(&ctx->server), "127.0.0.1") == 0;
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

    if (demo_recv_raw_quiet(fd, output, sizeof(output), 4000, 250) < 0) {
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

static bool test_path_execution(uint16_t port) {
    int fd;
    char output[DEMO_FEATURE_MAX];

    if (!demo_open_session(port, &fd, output, sizeof(output))) {
        return false;
    }

    if (!demo_send_line(fd, "system/log-level error") ||
        !demo_send_line(fd, "help reboot") ||
        !demo_send_line(fd, "/system") ||
        !demo_send_line(fd, "/network/config 10.1.2.3 255.255.255.0 10.1.2.1") ||
        !demo_send_line(fd, "help reboot") ||
        !demo_send_line(fd, "help /system/reboot") ||
        !demo_send_line(fd, "exit")) {
        close(fd);
        return false;
    }

    if (demo_recv_quiet(fd, output, sizeof(output), 4000, 250) < 0) {
        close(fd);
        return false;
    }
    close(fd);

    return demo_expect_contains(output, "log level => error", "path command execution") &&
           demo_expect_contains(output, "no such item: reboot", "path command keeps root menu") &&
           demo_expect_contains(output, "enter menu: /system", "absolute path menu enter") &&
           demo_expect_contains(output, "network => ip=10.1.2.3 mask=255.255.255.0 gateway=10.1.2.1", "absolute path command execution") &&
           demo_count_occurrence(output, "command : reboot") >= 2 &&
           demo_expect_not_contains(output, "enter menu: system/log-level", "path command should not enter menu");
}

static bool test_input_boundaries(uint16_t port) {
    int fd;
    char output[8192];
    char chunk[101];
    char tail[11];

    if (!demo_open_session(port, &fd, output, sizeof(output))) {
        return false;
    }

    memset(chunk, 'x', sizeof(chunk) - 1U);
    chunk[sizeof(chunk) - 1U] = '\0';
    memset(tail, 'x', sizeof(tail) - 1U);
    tail[sizeof(tail) - 1U] = '\0';

    for (int index = 0; index < 5; ++index) {
        if (!demo_send_all(fd, chunk, strlen(chunk)) ||
            demo_recv_raw_quiet(fd, NULL, 0, 2000, 150) < 0) {
            close(fd);
            return false;
        }
    }

    if (!demo_send_all(fd, tail, strlen(tail)) ||
        demo_recv_raw_quiet(fd, NULL, 0, 2000, 150) < 0 ||
        !demo_send_text(fd, "xx\r") ||
        !demo_send_line(fd, "version") ||
        !demo_send_line(fd, "exit")) {
        close(fd);
        return false;
    }

    if (demo_recv_raw_quiet(fd, output, sizeof(output), 4000, 250) < 0) {
        close(fd);
        return false;
    }
    close(fd);

    return demo_expect_contains(output, "input too long", "telnet line overflow") &&
           demo_expect_contains(output, "demo-fw 1.0.0", "session still usable after overflow");
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

static bool run_parser_interface_tests(void) {
    parser_test_fixture_t fixture;
    char long_line[700];
    char token_line[256];
    size_t offset = 0;
    bool ok = true;

    parser_test_fixture_init(&fixture);

    demo_mem_writer_reset(&fixture.writer);
    embcli_session_process_line(&fixture.session, "echo \"hello world\"");
    ok = ok && demo_expect_contains(fixture.output, "string=hello world", "quoted string");

    demo_mem_writer_reset(&fixture.writer);
    embcli_session_process_line(&fixture.session, "echo hello\\ world");
    ok = ok && demo_expect_contains(fixture.output, "string=hello world", "escaped space");

    demo_mem_writer_reset(&fixture.writer);
    embcli_session_process_line(&fixture.session, "signed -8");
    ok = ok && demo_expect_contains(fixture.output, "int=-8", "signed integer");

    demo_mem_writer_reset(&fixture.writer);
    embcli_session_process_line(&fixture.session, "signed");
    ok = ok && demo_expect_contains(fixture.output, "missing argument: value", "missing int argument");

    demo_mem_writer_reset(&fixture.writer);
    embcli_session_process_line(&fixture.session, "signed abc");
    ok = ok && demo_expect_contains(fixture.output, "invalid integer for value: abc", "invalid int text");

    demo_mem_writer_reset(&fixture.writer);
    embcli_session_process_line(&fixture.session, "signed 999999999999999999999999");
    ok = ok && demo_expect_contains(fixture.output, "out of range for value: -10 .. 10", "int overflow");

    demo_mem_writer_reset(&fixture.writer);
    embcli_session_process_line(&fixture.session, "unsigned 18446744073709551615");
    ok = ok && demo_expect_contains(fixture.output, "uint=18446744073709551615", "uint max value");

    demo_mem_writer_reset(&fixture.writer);
    embcli_session_process_line(&fixture.session, "unsigned -1");
    ok = ok && demo_expect_contains(fixture.output, "invalid unsigned integer for value: -1", "negative uint rejected");

    demo_mem_writer_reset(&fixture.writer);
    embcli_session_process_line(&fixture.session, "unsigned 18446744073709551616");
    ok = ok && demo_expect_contains(
        fixture.output,
        "out of range for value: 0 .. 18446744073709551615",
        "uint overflow");

    demo_mem_writer_reset(&fixture.writer);
    embcli_session_process_line(&fixture.session, "flag ENABLED");
    ok = ok && demo_expect_contains(fixture.output, "bool=true", "bool truthy alias");

    demo_mem_writer_reset(&fixture.writer);
    embcli_session_process_line(&fixture.session, "flag disable");
    ok = ok && demo_expect_contains(fixture.output, "bool=false", "bool falsy alias");

    demo_mem_writer_reset(&fixture.writer);
    embcli_session_process_line(&fixture.session, "flag maybe");
    ok = ok && demo_expect_contains(fixture.output, "invalid boolean for value: maybe", "invalid bool");

    demo_mem_writer_reset(&fixture.writer);
    embcli_session_process_line(&fixture.session, "mode BETA");
    ok = ok && demo_expect_contains(fixture.output, "enum=beta", "enum case insensitive");

    demo_mem_writer_reset(&fixture.writer);
    embcli_session_process_line(&fixture.session, "mode delta");
    ok = ok && demo_expect_contains(fixture.output, "invalid enum for value: delta", "invalid enum");

    demo_mem_writer_reset(&fixture.writer);
    embcli_session_process_line(&fixture.session, "note maintenance window tonight");
    ok = ok && demo_expect_contains(fixture.output, "rest=maintenance window tonight", "rest capture");

    demo_mem_writer_reset(&fixture.writer);
    embcli_session_process_line(&fixture.session, "echo one two");
    ok = ok && demo_expect_contains(fixture.output, "too many arguments", "extra arguments");

    demo_mem_writer_reset(&fixture.writer);
    embcli_session_process_line(&fixture.session, "echo \"unterminated");
    ok = ok && demo_expect_contains(fixture.output, "parse error: unterminated quote", "unterminated quote");

    demo_mem_writer_reset(&fixture.writer);
    embcli_session_process_line(&fixture.session, "echo hello\\");
    ok = ok && demo_expect_contains(fixture.output, "parse error: dangling escape", "dangling escape");

    memset(token_line, 0, sizeof(token_line));
    for (int index = 0; index < 25; ++index) {
        int written = snprintf(
            token_line + offset,
            sizeof(token_line) - offset,
            "%sarg%d",
            index == 0 ? "" : " ",
            index);
        if (written < 0 || (size_t)written >= sizeof(token_line) - offset) {
            ok = false;
            break;
        }
        offset += (size_t)written;
    }
    demo_mem_writer_reset(&fixture.writer);
    embcli_session_process_line(&fixture.session, token_line);
    ok = ok && demo_expect_contains(fixture.output, "parse error: too many tokens", "too many tokens");

    memset(long_line, 'L', sizeof(long_line));
    long_line[sizeof(long_line) - 1] = '\0';
    demo_mem_writer_reset(&fixture.writer);
    embcli_session_process_line(&fixture.session, long_line);
    ok = ok && demo_expect_contains(fixture.output, "input too long", "core api long input");

    parser_test_fixture_deinit(&fixture);
    return ok;
}

static bool test_completion_and_history(uint16_t port) {
    int fd;
    char output[DEMO_FEATURE_MAX];
    /*
     * 这段混合脚本覆盖了：
     * - 菜单补全
     * - help 目标补全
     * - 枚举值补全
     * - 参数提示展示
     * - 通过方向键回放历史
     * - bool 参数补全
     */
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
        { "parser-interface", NULL },
        { "banner-and-root", test_banner_and_root },
        { "navigation-and-help", test_navigation_and_help },
        { "path-execution", test_path_execution },
        { "input-boundaries", test_input_boundaries },
        { "parameters-and-errors", test_parameters_and_errors },
        { "completion-and-history", test_completion_and_history }
    };

    /* 固定顺序执行功能测试，便于在失败时直接定位阶段。 */
    for (size_t index = 0; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        bool ok = tests[index].fn != NULL ? tests[index].fn(port) : run_parser_interface_tests();
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

    /*
     * 顺序压测会维持一个长连接，并持续发送多轮命令突发流量。
     * 这样能在没有多线程噪声的情况下，抓出菜单导航和行处理里的状态泄漏问题。
     */
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

    /* 每个 worker 都像一个独立操作者，拥有自己的会话。 */
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
        if (demo_recv_raw_quiet(fd, output, sizeof(output), 5000, 250) < 0) {
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

    /* 并发 worker 主要用于验证 telnet 传输层的线程安全和隔离性。 */
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

static void *run_edge_stress_worker(void *arg) {
    stress_worker_args_t *worker = (stress_worker_args_t *)arg;
    int fd = -1;
    char banner[4096];
    char output[16384];
    char chunk[101];
    char tail[11];

    worker->ok = false;
    worker->error[0] = '\0';

    if (!demo_open_session(worker->port, &fd, banner, sizeof(banner))) {
        snprintf(worker->error, sizeof(worker->error), "connect/open failed");
        return NULL;
    }

    memset(chunk, 'a' + (worker->worker_id % 26), sizeof(chunk) - 1U);
    chunk[sizeof(chunk) - 1U] = '\0';
    memset(tail, 'a' + (worker->worker_id % 26), sizeof(tail) - 1U);
    tail[sizeof(tail) - 1U] = '\0';

    for (int index = 0; index < worker->loops; ++index) {
        if (!demo_send_line(fd, "system/log-level info") ||
            !demo_send_line(fd, "system/log-level verbose") ||
            !demo_send_line(fd, "help /system/reboot") ||
            !demo_send_line(fd, "network/config 10.0.0.2 255.255.255.0 10.0.0.1") ||
            !demo_send_line(fd, "device/set 0 enabled disabled")) {
            snprintf(worker->error, sizeof(worker->error), "send failed");
            goto cleanup;
        }

        if (demo_recv_quiet(fd, output, sizeof(output), 5000, 250) < 0) {
            snprintf(worker->error, sizeof(worker->error), "recv failed");
            goto cleanup;
        }

        if (strstr(output, "log level => info") == NULL ||
            strstr(output, "invalid enum for level: verbose") == NULL ||
            strstr(output, "command : reboot") == NULL ||
            strstr(output, "network => ip=10.0.0.2 mask=255.255.255.0 gateway=10.0.0.1") == NULL ||
            strstr(output, "led[0] => on=true blink=false") == NULL) {
            snprintf(worker->error, sizeof(worker->error), "unexpected functional output in loop %d", index);
            goto cleanup;
        }

        for (int chunk_index = 0; chunk_index < 5; ++chunk_index) {
            if (!demo_send_all(fd, chunk, strlen(chunk)) ||
                demo_recv_raw_quiet(fd, NULL, 0, 2000, 150) < 0) {
                snprintf(worker->error, sizeof(worker->error), "edge prefill failed");
                goto cleanup;
            }
        }

        if (!demo_send_all(fd, tail, strlen(tail)) ||
            demo_recv_raw_quiet(fd, NULL, 0, 2000, 150) < 0 ||
            !demo_send_text(fd, "aa\r") ||
            !demo_send_line(fd, "version")) {
            snprintf(worker->error, sizeof(worker->error), "edge send failed");
            goto cleanup;
        }

        if (demo_recv_raw_quiet(fd, output, sizeof(output), 5000, 250) < 0) {
            snprintf(worker->error, sizeof(worker->error), "edge recv failed");
            goto cleanup;
        }

        if (strstr(output, "input too long") == NULL ||
            strstr(output, "demo-fw 1.0.0") == NULL) {
            snprintf(worker->error, sizeof(worker->error), "unexpected edge output in loop %d", index);
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

static bool run_parallel_edge_stress(uint16_t port, int clients, int loops) {
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
        if (pthread_create(&threads[index], NULL, run_edge_stress_worker, &workers[index]) != 0) {
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
            fprintf(stderr, "parallel edge worker %d failed: %s\n", index, workers[index].error);
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

    /* 第三个会话应被拒绝，因为这里把 max_clients 固定成了 2。 */
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

static void *run_dynamic_writer(void *arg) {
    dynamic_writer_args_t *worker = (dynamic_writer_args_t *)arg;
    dynamic_test_ctx_t *ctx = worker->ctx;
    int begin = worker->writer_id * DEMO_DYNAMIC_MENUS_PER_WRITER;

    worker->ok = false;
    worker->error[0] = '\0';
    if (!demo_thread_gate_wait(&ctx->gate)) {
        snprintf(worker->error, sizeof(worker->error), "gate aborted");
        return NULL;
    }

    /*
     * 每个写线程负责一段互不重叠的菜单对象，避免对象级写冲突。
     * 真正共享的是 CLI 树本身，从而可以验证动态尾插时的加锁正确性。
     */
    for (int index = 0; index < DEMO_DYNAMIC_MENUS_PER_WRITER; ++index) {
        dynamic_entry_t *entry = &ctx->entries[begin + index];
        struct timespec pause;

        snprintf(entry->menu_name, sizeof(entry->menu_name), "dyn-%d-%02d", worker->writer_id, index);
        snprintf(
            entry->menu_summary,
            sizeof(entry->menu_summary),
            "dynamic menu from writer %d item %d",
            worker->writer_id,
            index);
        snprintf(
            entry->command_summary,
            sizeof(entry->command_summary),
            "dynamic ping from writer %d item %d",
            worker->writer_id,
            index);

        entry->command_id = begin + index;
        embcli_menu_init(&entry->menu, entry->menu_name, entry->menu_summary);
        embcli_command_init(
            &entry->command,
            "ping",
            entry->command_summary,
            NULL,
            0,
            demo_dynamic_command,
            &entry->command_id);

        embcli_menu_add_menu(embcli_root_menu(&ctx->cli), &entry->menu);
        embcli_menu_add_command(&entry->menu, &entry->command);

        /*
         * 人为插入极短让步，增加注册线程与查询线程交错的概率，
         * 让测试更容易覆盖“菜单已挂接、命令刚追加”的边界窗口。
         */
        pause.tv_sec = 0;
        pause.tv_nsec = 1000000L;
        nanosleep(&pause, NULL);
    }

    worker->ok = true;
    return NULL;
}

static void *run_dynamic_reader(void *arg) {
    dynamic_reader_args_t *reader = (dynamic_reader_args_t *)arg;
    dynamic_test_ctx_t *ctx = reader->ctx;
    embcli_session_t session;
    demo_mem_writer_t writer;
    char output[8192];
    char prompt[128];
    int rounds = 0;

    reader->ok = false;
    reader->error[0] = '\0';

    writer.buffer = output;
    writer.capacity = sizeof(output);
    demo_mem_writer_reset(&writer);
    embcli_session_init(&session, &ctx->cli, demo_mem_writer_write, &writer);

    if (!demo_thread_gate_wait(&ctx->gate)) {
        snprintf(reader->error, sizeof(reader->error), "gate aborted");
        return NULL;
    }

    while (!atomic_load(&ctx->stop_readers)) {
        demo_mem_writer_reset(&writer);
        embcli_session_show_current_menu(&session);
        embcli_session_prompt(&session);
        if (writer.length == 0 || strstr(output, "[dyncli]") == NULL) {
            snprintf(reader->error, sizeof(reader->error), "menu listing failed at round %d", rounds);
            return NULL;
        }

        demo_mem_writer_reset(&writer);
        embcli_session_process_line(&session, "help");
        if (writer.length == 0 || strstr(output, "help [name]") == NULL) {
            snprintf(reader->error, sizeof(reader->error), "help failed at round %d", rounds);
            return NULL;
        }

        memset(prompt, 0, sizeof(prompt));
        embcli_session_format_prompt(&session, prompt, sizeof(prompt));
        if (strcmp(prompt, "dyncli") != 0) {
            snprintf(reader->error, sizeof(reader->error), "prompt mismatch at round %d: %.64s", rounds, prompt);
            return NULL;
        }

        ++rounds;
    }

    reader->ok = true;
    return NULL;
}

static bool verify_dynamic_registration(dynamic_test_ctx_t *ctx) {
    embcli_session_t session;
    demo_mem_writer_t writer;
    char output[8192];
    bool ok = true;

    writer.buffer = output;
    writer.capacity = sizeof(output);

    for (int index = 0; index < DEMO_DYNAMIC_TOTAL_MENUS; ++index) {
        dynamic_entry_t *entry = &ctx->entries[index];

        demo_mem_writer_reset(&writer);
        embcli_session_init(&session, &ctx->cli, demo_mem_writer_write, &writer);
        embcli_session_process_line(&session, entry->menu_name);
        if (strstr(output, "enter menu:") == NULL) {
            fprintf(stderr, "dynamic verify failed when entering %s\noutput:\n%s\n", entry->menu_name, output);
            ok = false;
            break;
        }

        demo_mem_writer_reset(&writer);
        embcli_session_process_line(&session, "help ping");
        if (strstr(output, "command : ping") == NULL) {
            fprintf(stderr, "dynamic verify failed for help %s\noutput:\n%s\n", entry->menu_name, output);
            ok = false;
            break;
        }

        demo_mem_writer_reset(&writer);
        embcli_session_process_line(&session, "ping");
        if (strstr(output, "dynamic command") == NULL) {
            fprintf(stderr, "dynamic verify failed for ping %s\noutput:\n%s\n", entry->menu_name, output);
            ok = false;
            break;
        }
    }

    return ok;
}

static bool run_dynamic_registration_test(void) {
    dynamic_test_ctx_t ctx;
    pthread_t writers[DEMO_DYNAMIC_WRITERS];
    pthread_t readers[DEMO_DYNAMIC_READERS];
    dynamic_writer_args_t writer_args[DEMO_DYNAMIC_WRITERS];
    dynamic_reader_args_t reader_args[DEMO_DYNAMIC_READERS];
    bool ok = true;

    memset(&ctx, 0, sizeof(ctx));
    embcli_init(&ctx.cli, "dyncli", NULL);
    atomic_init(&ctx.stop_readers, false);

    if (!demo_thread_gate_init(&ctx.gate, DEMO_DYNAMIC_WRITERS + DEMO_DYNAMIC_READERS)) {
        embcli_deinit(&ctx.cli);
        return false;
    }

    for (int index = 0; index < DEMO_DYNAMIC_WRITERS; ++index) {
        writer_args[index].ctx = &ctx;
        writer_args[index].writer_id = index;
        writer_args[index].ok = false;
        writer_args[index].error[0] = '\0';
        if (pthread_create(&writers[index], NULL, run_dynamic_writer, &writer_args[index]) != 0) {
            fprintf(stderr, "dynamic writer thread create failed: %d\n", index);
            ok = false;
            demo_thread_gate_abort(&ctx.gate);
            for (int created = 0; created < index; ++created) {
                pthread_join(writers[created], NULL);
            }
            demo_thread_gate_destroy(&ctx.gate);
            embcli_deinit(&ctx.cli);
            return false;
        }
    }

    for (int index = 0; index < DEMO_DYNAMIC_READERS; ++index) {
        reader_args[index].ctx = &ctx;
        reader_args[index].reader_id = index;
        reader_args[index].ok = false;
        reader_args[index].error[0] = '\0';
        if (pthread_create(&readers[index], NULL, run_dynamic_reader, &reader_args[index]) != 0) {
            fprintf(stderr, "dynamic reader thread create failed: %d\n", index);
            ok = false;
            demo_thread_gate_abort(&ctx.gate);
            atomic_store(&ctx.stop_readers, true);
            for (int created = 0; created < DEMO_DYNAMIC_WRITERS; ++created) {
                pthread_join(writers[created], NULL);
            }
            for (int created = 0; created < index; ++created) {
                pthread_join(readers[created], NULL);
            }
            demo_thread_gate_destroy(&ctx.gate);
            embcli_deinit(&ctx.cli);
            return false;
        }
    }

    for (int index = 0; index < DEMO_DYNAMIC_WRITERS; ++index) {
        pthread_join(writers[index], NULL);
        if (!writer_args[index].ok) {
            ok = false;
            fprintf(stderr, "dynamic writer %d failed: %s\n", index, writer_args[index].error);
        }
    }

    atomic_store(&ctx.stop_readers, true);

    for (int index = 0; index < DEMO_DYNAMIC_READERS; ++index) {
        pthread_join(readers[index], NULL);
        if (!reader_args[index].ok) {
            ok = false;
            fprintf(stderr, "dynamic reader %d failed: %s\n", index, reader_args[index].error);
        }
    }

    if (ok) {
        ok = verify_dynamic_registration(&ctx);
    }

    demo_thread_gate_destroy(&ctx.gate);
    embcli_deinit(&ctx.cli);
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

    printf("[phase] server-api start\n");
    fflush(stdout);
    ok = run_server_api_test(&server);
    printf("[phase] server-api done: %s\n", ok ? "PASS" : "FAIL");
    fflush(stdout);

    printf("[phase] rebind-command start\n");
    fflush(stdout);
    if (ok) {
        ok = test_telnet_rebind_command(&server);
    }
    printf("[phase] rebind-command done: %s\n", ok ? "PASS" : "FAIL");
    fflush(stdout);

    printf("[phase] feature-demo start\n");
    fflush(stdout);
    if (ok) {
        ok = run_feature_demo(port);
    }
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
    if (ok) {
        bool parallel_edge_ok = run_parallel_edge_stress(port, parallel_clients, parallel_loops);
        printf("[stress] parallel-edge: %s\n", parallel_edge_ok ? "PASS" : "FAIL");
        fflush(stdout);
        ok = parallel_edge_ok;
    }
    if (ok) {
        bool dynamic_ok = run_dynamic_registration_test();
        printf("[stress] dynamic-registration: %s\n", dynamic_ok ? "PASS" : "FAIL");
        fflush(stdout);
        ok = dynamic_ok;
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
