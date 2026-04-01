#include "demo_app.h"

#include "embcli/embcli.h"

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

void build_demo_cli(embcli_t *cli, embcli_telnet_server_t *server) {
    static led_state_t led_state = { 0, false, false };
    static telnet_access_ctx_t telnet_ctx;
    static const char *log_levels[] = { "debug", "info", "warn", "error" };
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
    static embcli_menu_t system_menu;
    static embcli_menu_t network_menu;
    static embcli_menu_t device_menu;
    static embcli_command_t cmd_ver;
    static embcli_command_t cmd_led;
    static embcli_command_t cmd_log;
    static embcli_command_t cmd_telnet_access_toggle;
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
    embcli_menu_add_command(&system_menu, &cmd_reboot_now);
    embcli_menu_add_command(&network_menu, &cmd_ip);
    embcli_menu_add_command(&device_menu, &cmd_led);
}
