#ifndef EMBCLI_EMBCLI_H
#define EMBCLI_EMBCLI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMBCLI_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

typedef enum embcli_arg_type {
    EMBCLI_ARG_STRING = 0,
    EMBCLI_ARG_INT,
    EMBCLI_ARG_UINT,
    EMBCLI_ARG_BOOL,
    EMBCLI_ARG_ENUM,
    EMBCLI_ARG_REST
} embcli_arg_type_t;

typedef struct embcli embcli_t;
typedef struct embcli_menu embcli_menu_t;
typedef struct embcli_command embcli_command_t;
typedef struct embcli_session embcli_session_t;
typedef struct embcli_value embcli_value_t;

typedef void (*embcli_write_fn)(void *ctx, const char *data, size_t len);
typedef void (*embcli_handler_t)(
    embcli_session_t *session,
    const embcli_value_t *values,
    size_t value_count,
    void *user_data);

typedef struct embcli_arg_spec {
    const char *name;
    embcli_arg_type_t type;
    bool optional;
    const char *help;
    int64_t int_min;
    int64_t int_max;
    uint64_t uint_min;
    uint64_t uint_max;
    const char *const *enum_values;
    size_t enum_value_count;
} embcli_arg_spec_t;

struct embcli_value {
    const char *name;
    embcli_arg_type_t type;
    bool present;
    const char *text;
    union {
        int64_t i64;
        uint64_t u64;
        bool boolean;
        const char *str;
        size_t enum_index;
    } as;
};

struct embcli_command {
    const char *name;
    const char *summary;
    const embcli_arg_spec_t *args;
    size_t arg_count;
    embcli_handler_t handler;
    void *user_data;
    embcli_command_t *next;
};

struct embcli_menu {
    const char *name;
    const char *summary;
    embcli_menu_t *parent;
    embcli_menu_t *children;
    embcli_menu_t *next;
    embcli_command_t *commands;
};

struct embcli {
    const char *name;
    const char *banner;
    embcli_menu_t root_menu;
};

struct embcli_session {
    embcli_t *cli;
    embcli_menu_t *current_menu;
    embcli_write_fn writer;
    void *writer_ctx;
    bool close_requested;
};

#define EMBCLI_ARG_STRING_REQ(name, help_text) \
    { (name), EMBCLI_ARG_STRING, false, (help_text), 0, 0, 0, 0, NULL, 0 }
#define EMBCLI_ARG_STRING_OPT(name, help_text) \
    { (name), EMBCLI_ARG_STRING, true, (help_text), 0, 0, 0, 0, NULL, 0 }

#define EMBCLI_ARG_INT_REQ(name, help_text, min_value, max_value) \
    { (name), EMBCLI_ARG_INT, false, (help_text), (min_value), (max_value), 0, 0, NULL, 0 }
#define EMBCLI_ARG_INT_OPT(name, help_text, min_value, max_value) \
    { (name), EMBCLI_ARG_INT, true, (help_text), (min_value), (max_value), 0, 0, NULL, 0 }

#define EMBCLI_ARG_UINT_REQ(name, help_text, min_value, max_value) \
    { (name), EMBCLI_ARG_UINT, false, (help_text), 0, 0, (min_value), (max_value), NULL, 0 }
#define EMBCLI_ARG_UINT_OPT(name, help_text, min_value, max_value) \
    { (name), EMBCLI_ARG_UINT, true, (help_text), 0, 0, (min_value), (max_value), NULL, 0 }

#define EMBCLI_ARG_BOOL_REQ(name, help_text) \
    { (name), EMBCLI_ARG_BOOL, false, (help_text), 0, 0, 0, 0, NULL, 0 }
#define EMBCLI_ARG_BOOL_OPT(name, help_text) \
    { (name), EMBCLI_ARG_BOOL, true, (help_text), 0, 0, 0, 0, NULL, 0 }

#define EMBCLI_ARG_ENUM_REQ(name, help_text, enum_table) \
    { (name), EMBCLI_ARG_ENUM, false, (help_text), 0, 0, 0, 0, (enum_table), EMBCLI_ARRAY_SIZE(enum_table) }
#define EMBCLI_ARG_ENUM_OPT(name, help_text, enum_table) \
    { (name), EMBCLI_ARG_ENUM, true, (help_text), 0, 0, 0, 0, (enum_table), EMBCLI_ARRAY_SIZE(enum_table) }

#define EMBCLI_ARG_REST_REQ(name, help_text) \
    { (name), EMBCLI_ARG_REST, false, (help_text), 0, 0, 0, 0, NULL, 0 }
#define EMBCLI_ARG_REST_OPT(name, help_text) \
    { (name), EMBCLI_ARG_REST, true, (help_text), 0, 0, 0, 0, NULL, 0 }

#define EMBCLI_COMMAND_DEF(cmd_name, cmd_summary, arg_table, cmd_handler, user_ctx) \
    { (cmd_name), (cmd_summary), (arg_table), EMBCLI_ARRAY_SIZE(arg_table), (cmd_handler), (user_ctx), NULL }

#define EMBCLI_COMMAND0_DEF(cmd_name, cmd_summary, cmd_handler, user_ctx) \
    { (cmd_name), (cmd_summary), NULL, 0, (cmd_handler), (user_ctx), NULL }

#define EMBCLI_MENU_DEF(menu_name, menu_summary) \
    { (menu_name), (menu_summary), NULL, NULL, NULL, NULL }

void embcli_init(embcli_t *cli, const char *name, const char *banner);
embcli_menu_t *embcli_root_menu(embcli_t *cli);

void embcli_menu_init(embcli_menu_t *menu, const char *name, const char *summary);
void embcli_command_init(
    embcli_command_t *command,
    const char *name,
    const char *summary,
    const embcli_arg_spec_t *args,
    size_t arg_count,
    embcli_handler_t handler,
    void *user_data);

void embcli_menu_add_menu(embcli_menu_t *parent, embcli_menu_t *child);
void embcli_menu_add_command(embcli_menu_t *menu, embcli_command_t *command);

void embcli_session_init(
    embcli_session_t *session,
    embcli_t *cli,
    embcli_write_fn writer,
    void *writer_ctx);
void embcli_session_start(embcli_session_t *session);
void embcli_session_process_line(embcli_session_t *session, const char *line);
void embcli_session_show_current_menu(embcli_session_t *session);
void embcli_session_format_prompt(
    embcli_session_t *session,
    char *buffer,
    size_t buffer_size);
void embcli_session_prompt(embcli_session_t *session);

void embcli_session_write(embcli_session_t *session, const char *text);
void embcli_session_printf(embcli_session_t *session, const char *fmt, ...);
void embcli_session_request_close(embcli_session_t *session);

const char *embcli_value_string(const embcli_value_t *value);

#ifdef __cplusplus
}
#endif

#endif
