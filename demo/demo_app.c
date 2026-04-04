#include "demo_app.h"

#include "embcli/embcli.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct led_state {
    uint64_t id;
    bool on;
    bool blink;
} led_state_t;

typedef struct telnet_access_ctx {
    embcli_telnet_server_t *server;
} telnet_access_ctx_t;

typedef struct thread_cpu_sample {
    pid_t tid;
    unsigned long long user_ticks;
    unsigned long long system_ticks;
    char name[32];
} thread_cpu_sample_t;

typedef struct thread_cpu_report {
    pid_t tid;
    double user_percent;
    double system_percent;
    double total_percent;
    unsigned long long user_delta_ticks;
    unsigned long long system_delta_ticks;
    unsigned long long total_delta_ticks;
    char name[32];
} thread_cpu_report_t;

typedef enum thread_cpu_sort_key {
    THREAD_CPU_SORT_TOTAL = 0,
    THREAD_CPU_SORT_USER,
    THREAD_CPU_SORT_SYSTEM,
    THREAD_CPU_SORT_TID
} thread_cpu_sort_key_t;

/*
 * demo 回调刻意只回显解析后的状态，
 * 这样自动化 selftest 就能在不依赖真实硬件的前提下断言各项功能。
 */
static void cmd_version(
    embcli_session_t *session,
    const embcli_value_t *values,
    size_t value_count,
    void *user_data) {
    (void)values;
    (void)value_count;
    (void)user_data;
    embcli_session_write(session, "demo-fw 1.0.0\r\n");
}

static void cmd_led_set(
    embcli_session_t *session,
    const embcli_value_t *values,
    size_t value_count,
    void *user_data) {
    (void)value_count;
    led_state_t *state = (led_state_t *)user_data;

    state->id = values[0].as.u64;
    state->on = values[1].as.boolean;
    state->blink = values[2].present ? values[2].as.boolean : false;

    embcli_session_printf(
        session,
        "led[%llu] => on=%s blink=%s\r\n",
        (unsigned long long)state->id,
        state->on ? "true" : "false",
        state->blink ? "true" : "false");
}

static void cmd_log_level(
    embcli_session_t *session,
    const embcli_value_t *values,
    size_t value_count,
    void *user_data) {
    (void)value_count;
    (void)user_data;
    embcli_session_printf(session, "log level => %s\r\n", embcli_value_string(&values[0]));
}

static void cmd_net_config(
    embcli_session_t *session,
    const embcli_value_t *values,
    size_t value_count,
    void *user_data) {
    (void)value_count;
    (void)user_data;
    embcli_session_printf(
        session,
        "network => ip=%s mask=%s gateway=%s\r\n",
        embcli_value_string(&values[0]),
        embcli_value_string(&values[1]),
        embcli_value_string(&values[2]));
}

static void cmd_reboot(
    embcli_session_t *session,
    const embcli_value_t *values,
    size_t value_count,
    void *user_data) {
    (void)value_count;
    (void)user_data;

    uint64_t delay_ms = values[0].present ? values[0].as.u64 : 0;
    const char *reason = values[1].present ? embcli_value_string(&values[1]) : "manual";

    embcli_session_printf(
        session,
        "reboot scheduled: delay=%llums reason=%s\r\n",
        (unsigned long long)delay_ms,
        reason);
}

static void cmd_telnet_access(
    embcli_session_t *session,
    const embcli_value_t *values,
    size_t value_count,
    void *user_data) {
    telnet_access_ctx_t *ctx = (telnet_access_ctx_t *)user_data;
    bool enabled;
    int rc;

    (void)value_count;
    if (ctx == NULL || ctx->server == NULL) {
        embcli_session_write(session, "telnet server unavailable\r\n");
        return;
    }

    enabled = values[0].as.boolean;
    rc = embcli_telnet_server_rebind(ctx->server, enabled ? "0.0.0.0" : "127.0.0.1");
    if (rc != 0) {
        embcli_session_write(session, "telnet access update failed\r\n");
        return;
    }

    embcli_session_printf(
        session,
        "telnet bind => %s\r\n",
        embcli_telnet_server_bind_address(ctx->server));
}

static int thread_cpu_sample_compare(const void *lhs, const void *rhs) {
    const thread_cpu_sample_t *left = (const thread_cpu_sample_t *)lhs;
    const thread_cpu_sample_t *right = (const thread_cpu_sample_t *)rhs;

    if (left->tid < right->tid) {
        return -1;
    }
    if (left->tid > right->tid) {
        return 1;
    }
    return 0;
}

static int thread_cpu_report_compare(const void *lhs, const void *rhs) {
    const thread_cpu_report_t *left = (const thread_cpu_report_t *)lhs;
    const thread_cpu_report_t *right = (const thread_cpu_report_t *)rhs;

    if (left->tid < right->tid) {
        return -1;
    }
    if (left->tid > right->tid) {
        return 1;
    }
    return 0;
}

static bool read_thread_name(pid_t tid, char *buffer, size_t buffer_size) {
    char path[64];
    FILE *file;

    snprintf(path, sizeof(path), "/proc/self/task/%ld/comm", (long)tid);
    file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    if (fgets(buffer, (int)buffer_size, file) == NULL) {
        fclose(file);
        return false;
    }
    fclose(file);

    buffer[strcspn(buffer, "\r\n")] = '\0';
    return true;
}

static const char *thread_cpu_sort_name(thread_cpu_sort_key_t sort_key) {
    switch (sort_key) {
    case THREAD_CPU_SORT_USER:
        return "usr";
    case THREAD_CPU_SORT_SYSTEM:
        return "sys";
    case THREAD_CPU_SORT_TID:
        return "tid";
    case THREAD_CPU_SORT_TOTAL:
    default:
        return "cpu";
    }
}

static void skip_proc_stat_spaces(const char **cursor) {
    while (**cursor == ' ') {
        ++(*cursor);
    }
}

static bool skip_proc_stat_field(const char **cursor) {
    skip_proc_stat_spaces(cursor);
    if (**cursor == '\0') {
        return false;
    }
    while (**cursor != '\0' && **cursor != ' ') {
        ++(*cursor);
    }
    return true;
}

static bool read_thread_ticks(
    pid_t tid,
    unsigned long long *user_ticks,
    unsigned long long *system_ticks) {
    char path[64];
    char line[512];
    const char *cursor;
    char *rparen;
    FILE *file;
    char *end = NULL;
    unsigned long long utime;
    unsigned long long stime;

    snprintf(path, sizeof(path), "/proc/self/task/%ld/stat", (long)tid);
    file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return false;
    }
    fclose(file);

    rparen = strrchr(line, ')');
    if (rparen == NULL) {
        return false;
    }

    cursor = rparen + 2;
    if (!skip_proc_stat_field(&cursor)) {
        return false;
    }
    for (int index = 0; index < 10; ++index) {
        if (!skip_proc_stat_field(&cursor)) {
            return false;
        }
    }

    skip_proc_stat_spaces(&cursor);
    utime = strtoull(cursor, &end, 10);
    if (end == cursor) {
        return false;
    }
    cursor = end;
    skip_proc_stat_spaces(&cursor);
    stime = strtoull(cursor, &end, 10);
    if (end == cursor) {
        return false;
    }

    *user_ticks = utime;
    *system_ticks = stime;
    return true;
}

static bool collect_thread_cpu_samples(
    thread_cpu_sample_t **samples_out,
    size_t *count_out) {
    DIR *dir;
    struct dirent *entry;
    thread_cpu_sample_t *samples = NULL;
    size_t count = 0;
    size_t capacity = 0;

    dir = opendir("/proc/self/task");
    if (dir == NULL) {
        return false;
    }

    while ((entry = readdir(dir)) != NULL) {
        char *end = NULL;
        long tid_long = strtol(entry->d_name, &end, 10);
        thread_cpu_sample_t sample;

        if (entry->d_name[0] == '.' || end == NULL || *end != '\0') {
            continue;
        }

        memset(&sample, 0, sizeof(sample));
        sample.tid = (pid_t)tid_long;
        if (!read_thread_ticks(sample.tid, &sample.user_ticks, &sample.system_ticks)) {
            continue;
        }
        if (!read_thread_name(sample.tid, sample.name, sizeof(sample.name))) {
            snprintf(sample.name, sizeof(sample.name), "tid-%ld", tid_long);
        }

        if (count == capacity) {
            thread_cpu_sample_t *grown;
            size_t next_capacity = capacity == 0 ? 8U : capacity * 2U;

            grown = (thread_cpu_sample_t *)realloc(samples, next_capacity * sizeof(*samples));
            if (grown == NULL) {
                free(samples);
                closedir(dir);
                return false;
            }
            samples = grown;
            capacity = next_capacity;
        }

        samples[count++] = sample;
    }

    closedir(dir);
    qsort(samples, count, sizeof(*samples), thread_cpu_sample_compare);

    *samples_out = samples;
    *count_out = count;
    return true;
}

static const thread_cpu_sample_t *find_thread_cpu_sample(
    const thread_cpu_sample_t *samples,
    size_t count,
    pid_t tid) {
    size_t left = 0;
    size_t right = count;

    while (left < right) {
        size_t mid = left + (right - left) / 2U;

        if (samples[mid].tid == tid) {
            return &samples[mid];
        }
        if (samples[mid].tid < tid) {
            left = mid + 1U;
        } else {
            right = mid;
        }
    }

    return NULL;
}

static double thread_cpu_report_value(
    const thread_cpu_report_t *report,
    thread_cpu_sort_key_t sort_key) {
    switch (sort_key) {
    case THREAD_CPU_SORT_USER:
        return report->user_percent;
    case THREAD_CPU_SORT_SYSTEM:
        return report->system_percent;
    case THREAD_CPU_SORT_TID:
        return (double)report->tid;
    case THREAD_CPU_SORT_TOTAL:
    default:
        return report->total_percent;
    }
}

static bool thread_cpu_report_before(
    const thread_cpu_report_t *left,
    const thread_cpu_report_t *right,
    thread_cpu_sort_key_t sort_key) {
    double left_value = thread_cpu_report_value(left, sort_key);
    double right_value = thread_cpu_report_value(right, sort_key);

    if (sort_key == THREAD_CPU_SORT_TID) {
        if (left_value < right_value) {
            return true;
        }
        if (left_value > right_value) {
            return false;
        }
    } else {
        if (left_value > right_value) {
            return true;
        }
        if (left_value < right_value) {
            return false;
        }
    }

    return thread_cpu_report_compare(left, right) < 0;
}

static void sort_thread_cpu_reports(
    thread_cpu_report_t *reports,
    size_t report_count,
    thread_cpu_sort_key_t sort_key) {
    for (size_t index = 1; index < report_count; ++index) {
        thread_cpu_report_t key = reports[index];
        size_t pos = index;

        while (pos > 0 && thread_cpu_report_before(&key, &reports[pos - 1], sort_key)) {
            reports[pos] = reports[pos - 1];
            --pos;
        }
        reports[pos] = key;
    }
}

static bool sleep_interval_ms(uint64_t interval_ms) {
    struct timespec req;

    req.tv_sec = (time_t)(interval_ms / 1000U);
    req.tv_nsec = (long)((interval_ms % 1000U) * 1000000U);

    while (nanosleep(&req, &req) != 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

static void cmd_thread_cpu(
    embcli_session_t *session,
    const embcli_value_t *values,
    size_t value_count,
    void *user_data) {
#if defined(__linux__)
    thread_cpu_sample_t *before = NULL;
    thread_cpu_sample_t *after = NULL;
    thread_cpu_report_t *reports = NULL;
    struct timespec started;
    struct timespec finished;
    double elapsed_sec;
    long hz;
    uint64_t interval_ms;
    uint64_t limit;
    const char *filter;
    thread_cpu_sort_key_t sort_key;
    size_t before_count = 0;
    size_t after_count = 0;
    size_t report_count = 0;
    size_t print_count;

    (void)value_count;
    (void)user_data;

    interval_ms = values[0].present ? values[0].as.u64 : 500U;
    limit = values[1].present ? values[1].as.u64 : 8U;
    sort_key = values[2].present ? (thread_cpu_sort_key_t)values[2].as.enum_index : THREAD_CPU_SORT_TOTAL;
    filter = values[3].present ? embcli_value_string(&values[3]) : NULL;
    hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) {
        embcli_session_write(session, "thread cpu sampling unavailable\r\n");
        return;
    }

    if (!collect_thread_cpu_samples(&before, &before_count)) {
        embcli_session_write(session, "thread cpu snapshot failed\r\n");
        goto cleanup;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &started) != 0) {
        embcli_session_write(session, "thread cpu timer start failed\r\n");
        goto cleanup;
    }
    if (!sleep_interval_ms(interval_ms)) {
        embcli_session_write(session, "thread cpu sleep interrupted\r\n");
        goto cleanup;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &finished) != 0) {
        embcli_session_write(session, "thread cpu timer stop failed\r\n");
        goto cleanup;
    }
    if (!collect_thread_cpu_samples(&after, &after_count)) {
        embcli_session_write(session, "thread cpu snapshot failed\r\n");
        goto cleanup;
    }

    reports = (thread_cpu_report_t *)calloc(after_count, sizeof(*reports));
    if (reports == NULL) {
        embcli_session_write(session, "thread cpu report allocation failed\r\n");
        goto cleanup;
    }

    elapsed_sec =
        (double)(finished.tv_sec - started.tv_sec) +
        (double)(finished.tv_nsec - started.tv_nsec) / 1000000000.0;
    if (elapsed_sec <= 0.0) {
        embcli_session_write(session, "thread cpu elapsed time invalid\r\n");
        goto cleanup;
    }

    for (size_t index = 0; index < after_count; ++index) {
        const thread_cpu_sample_t *previous = find_thread_cpu_sample(before, before_count, after[index].tid);
        unsigned long long user_delta_ticks = 0;
        unsigned long long system_delta_ticks = 0;

        if (filter != NULL && filter[0] != '\0' && strstr(after[index].name, filter) == NULL) {
            continue;
        }

        if (previous != NULL) {
            if (after[index].user_ticks >= previous->user_ticks) {
                user_delta_ticks = after[index].user_ticks - previous->user_ticks;
            }
            if (after[index].system_ticks >= previous->system_ticks) {
                system_delta_ticks = after[index].system_ticks - previous->system_ticks;
            }
        }

        reports[report_count].tid = after[index].tid;
        reports[report_count].user_delta_ticks = user_delta_ticks;
        reports[report_count].system_delta_ticks = system_delta_ticks;
        reports[report_count].total_delta_ticks = user_delta_ticks + system_delta_ticks;
        reports[report_count].user_percent = ((double)user_delta_ticks / (double)hz) / elapsed_sec * 100.0;
        reports[report_count].system_percent = ((double)system_delta_ticks / (double)hz) / elapsed_sec * 100.0;
        reports[report_count].total_percent = reports[report_count].user_percent + reports[report_count].system_percent;
        snprintf(reports[report_count].name, sizeof(reports[report_count].name), "%s", after[index].name);
        ++report_count;
    }

    sort_thread_cpu_reports(reports, report_count, sort_key);
    print_count = report_count;
    if (limit > 0U && (size_t)limit < print_count) {
        print_count = (size_t)limit;
    }

    embcli_session_printf(
        session,
        "thread cpu sample: interval=%llums thread-count=%zu sort=%s filter=%s\r\n",
        (unsigned long long)interval_ms,
        report_count,
        thread_cpu_sort_name(sort_key),
        (filter != NULL && filter[0] != '\0') ? filter : "-");
    embcli_session_write(session, "  TID       USR%   SYS%   CPU%    TICKS   NAME\r\n");
    for (size_t index = 0; index < print_count; ++index) {
        embcli_session_printf(
            session,
            "  %-8ld %6.2f %6.2f %6.2f %7llu %s\r\n",
            (long)reports[index].tid,
            reports[index].user_percent,
            reports[index].system_percent,
            reports[index].total_percent,
            reports[index].total_delta_ticks,
            reports[index].name);
    }
cleanup:
    free(before);
    free(after);
    free(reports);
#else
    (void)values;
    (void)value_count;
    (void)user_data;
    embcli_session_write(session, "thread cpu sampling unsupported on this platform\r\n");
#endif
}

void build_demo_cli(embcli_t *cli, embcli_telnet_server_t *server) {
    static led_state_t led_state = { 0, false, false };
    static telnet_access_ctx_t telnet_ctx;
    static const char *log_levels[] = { "debug", "info", "warn", "error" };
    static const char *thread_cpu_sort_values[] = { "cpu", "usr", "sys", "tid" };
    static const embcli_arg_spec_t led_args[] = {
        EMBCLI_ARG_UINT_REQ("id", "led id, range 0..7", 0, 7),
        EMBCLI_ARG_BOOL_REQ("state", "on/off, true/false, 1/0"),
        EMBCLI_ARG_BOOL_OPT("blink", "optional blink mode")
    };
    static const embcli_arg_spec_t log_args[] = {
        EMBCLI_ARG_ENUM_REQ("level", "debug/info/warn/error", log_levels)
    };
    static const embcli_arg_spec_t net_args[] = {
        EMBCLI_ARG_STRING_REQ("ip", "ipv4 address"),
        EMBCLI_ARG_STRING_REQ("mask", "subnet mask"),
        EMBCLI_ARG_STRING_REQ("gateway", "default gateway")
    };
    static const embcli_arg_spec_t reboot_args[] = {
        EMBCLI_ARG_UINT_OPT("delay_ms", "optional delay before reboot", 0, 60000),
        EMBCLI_ARG_REST_OPT("reason", "optional reason text")
    };
    static const embcli_arg_spec_t telnet_access_args[] = {
        EMBCLI_ARG_BOOL_REQ("enabled", "on/off, true/false, 1/0")
    };
    static const embcli_arg_spec_t thread_cpu_args[] = {
        EMBCLI_ARG_UINT_OPT("interval_ms", "sampling window in milliseconds", 50, 10000),
        EMBCLI_ARG_UINT_OPT("limit", "maximum rows to print", 1, 64),
        EMBCLI_ARG_ENUM_OPT("sort", "cpu/usr/sys/tid", thread_cpu_sort_values),
        EMBCLI_ARG_STRING_OPT("filter", "optional thread name substring")
    };
    static embcli_menu_t system_menu;
    static embcli_menu_t network_menu;
    static embcli_menu_t device_menu;
    static embcli_command_t cmd_ver;
    static embcli_command_t cmd_led;
    static embcli_command_t cmd_log;
    static embcli_command_t cmd_telnet_access_toggle;
    static embcli_command_t cmd_thread_cpu_sample;
    static embcli_command_t cmd_ip;
    static embcli_command_t cmd_reboot_now;

    /*
     * 这些对象保留为 static，以贴近嵌入式固件常见的内存布局。
     * 但每次 build 都会重新 init，确保 selftest 能在同一进程里多次启停 demo server。
     */
    embcli_init(cli, "board", "Embedded CLI demo over telnet");
    telnet_ctx.server = server;
    embcli_menu_init(&system_menu, "system", "system control and logging");
    embcli_menu_init(&network_menu, "network", "network configuration");
    embcli_menu_init(&device_menu, "device", "device control");
    embcli_command_init(&cmd_ver, "version", "show firmware version", NULL, 0, cmd_version, NULL);
    embcli_command_init(&cmd_led, "set", "set led state", led_args, EMBCLI_ARRAY_SIZE(led_args), cmd_led_set, &led_state);
    embcli_command_init(&cmd_log, "log-level", "set log level", log_args, EMBCLI_ARRAY_SIZE(log_args), cmd_log_level, NULL);
    embcli_command_init(
        &cmd_telnet_access_toggle,
        "telnet-access",
        "toggle remote telnet exposure",
        telnet_access_args,
        EMBCLI_ARRAY_SIZE(telnet_access_args),
        cmd_telnet_access,
        &telnet_ctx);
    embcli_command_init(
        &cmd_thread_cpu_sample,
        "thread-cpu",
        "sample per-thread cpu like pidstat",
        thread_cpu_args,
        EMBCLI_ARRAY_SIZE(thread_cpu_args),
        cmd_thread_cpu,
        NULL);
    embcli_command_init(&cmd_ip, "config", "configure network", net_args, EMBCLI_ARRAY_SIZE(net_args), cmd_net_config, NULL);
    embcli_command_init(
        &cmd_reboot_now,
        "reboot",
        "schedule reboot",
        reboot_args,
        EMBCLI_ARRAY_SIZE(reboot_args),
        cmd_reboot,
        NULL);

    embcli_menu_add_menu(embcli_root_menu(cli), &system_menu);
    embcli_menu_add_menu(embcli_root_menu(cli), &network_menu);
    embcli_menu_add_menu(embcli_root_menu(cli), &device_menu);
    embcli_menu_add_command(embcli_root_menu(cli), &cmd_ver);

    embcli_menu_add_command(&system_menu, &cmd_log);
    embcli_menu_add_command(&system_menu, &cmd_telnet_access_toggle);
    embcli_menu_add_command(&system_menu, &cmd_thread_cpu_sample);
    embcli_menu_add_command(&system_menu, &cmd_reboot_now);
    embcli_menu_add_command(&network_menu, &cmd_ip);
    embcli_menu_add_command(&device_menu, &cmd_led);
}
