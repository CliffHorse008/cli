#include "embcli/embcli.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EMBCLI_MAX_TOKENS 24
#define EMBCLI_MAX_USAGE 256
#define EMBCLI_MAX_PATH_DEPTH 16
#define EMBCLI_MAX_PROMPT 128
#define EMBCLI_MAX_LINE 512

typedef struct embcli_token {
    char *text;
} embcli_token_t;

/*
 * 当前版本支持多线程动态注册，但不支持运行时删除节点。
 * 因此这里只需要用读写锁保护树结构遍历和尾插注册即可。
 */
static void embcli_read_lock(embcli_t *cli) {
    if (cli != NULL) {
        pthread_rwlock_rdlock(&cli->tree_lock);
    }
}

static void embcli_write_lock(embcli_t *cli) {
    if (cli != NULL) {
        pthread_rwlock_wrlock(&cli->tree_lock);
    }
}

static void embcli_unlock(embcli_t *cli) {
    if (cli != NULL) {
        pthread_rwlock_unlock(&cli->tree_lock);
    }
}

/*
 * 当一个子树被挂入某个 CLI 实例后，需要把 owner 递归写回整棵子树。
 * 这样后续任意层级的动态注册都能回到同一个 tree_lock。
 */
static void embcli_menu_assign_owner_recursive(embcli_menu_t *menu, embcli_t *owner) {
    while (menu != NULL) {
        embcli_menu_t *child = menu->children;
        menu->owner = owner;
        if (child != NULL) {
            embcli_menu_assign_owner_recursive(child, owner);
        }
        menu = menu->next;
    }
}

/*
 * 供 bool/enum 解析使用的不区分大小写比较。
 * 命令关键字匹配则故意保持大小写敏感，便于嵌入式产品约束稳定的命名风格。
 */
static int embcli_stricmp(const char *lhs, const char *rhs) {
    while (*lhs != '\0' && *rhs != '\0') {
        int left = tolower((unsigned char)*lhs);
        int right = tolower((unsigned char)*rhs);
        if (left != right) {
            return left - right;
        }
        ++lhs;
        ++rhs;
    }
    return tolower((unsigned char)*lhs) - tolower((unsigned char)*rhs);
}

static const embcli_menu_t *embcli_find_child_menu(const embcli_menu_t *menu, const char *name) {
    const embcli_menu_t *child = menu->children;
    while (child != NULL) {
        if (strcmp(child->name, name) == 0) {
            return child;
        }
        child = child->next;
    }
    return NULL;
}

static const embcli_command_t *embcli_find_command(const embcli_menu_t *menu, const char *name) {
    const embcli_command_t *command = menu->commands;
    while (command != NULL) {
        if (strcmp(command->name, name) == 0) {
            return command;
        }
        command = command->next;
    }
    return NULL;
}

static bool embcli_name_equals_segment(const char *name, const char *segment, size_t segment_len) {
    return strncmp(name, segment, segment_len) == 0 && name[segment_len] == '\0';
}

static const embcli_menu_t *embcli_find_child_menu_segment(
    const embcli_menu_t *menu,
    const char *segment,
    size_t segment_len) {
    const embcli_menu_t *child = menu->children;
    while (child != NULL) {
        if (embcli_name_equals_segment(child->name, segment, segment_len)) {
            return child;
        }
        child = child->next;
    }
    return NULL;
}

static const embcli_command_t *embcli_find_command_segment(
    const embcli_menu_t *menu,
    const char *segment,
    size_t segment_len) {
    const embcli_command_t *command = menu->commands;
    while (command != NULL) {
        if (embcli_name_equals_segment(command->name, segment, segment_len)) {
            return command;
        }
        command = command->next;
    }
    return NULL;
}

static bool embcli_resolve_path(
    const embcli_session_t *session,
    const char *path,
    const embcli_menu_t **menu_out,
    const embcli_command_t **command_out) {
    const embcli_menu_t *menu;
    const char *cursor;

    if (session == NULL || path == NULL || *path == '\0') {
        return false;
    }

    menu = session->current_menu;
    cursor = path;

    if (*cursor == '/') {
        menu = &session->cli->root_menu;
        while (*cursor == '/') {
            ++cursor;
        }
    }

    if (*cursor == '\0') {
        *menu_out = menu;
        *command_out = NULL;
        return true;
    }

    while (*cursor != '\0') {
        const char *slash = strchr(cursor, '/');
        size_t segment_len = slash != NULL ? (size_t)(slash - cursor) : strlen(cursor);

        if (segment_len == 0) {
            return false;
        }

        if (slash == NULL) {
            const embcli_menu_t *child_menu = embcli_find_child_menu_segment(menu, cursor, segment_len);
            if (child_menu != NULL) {
                *menu_out = child_menu;
                *command_out = NULL;
                return true;
            }

            *menu_out = menu;
            *command_out = embcli_find_command_segment(menu, cursor, segment_len);
            return *command_out != NULL;
        }

        menu = embcli_find_child_menu_segment(menu, cursor, segment_len);
        if (menu == NULL) {
            return false;
        }

        cursor = slash + 1;
        while (*cursor == '/') {
            ++cursor;
        }
        if (*cursor == '\0') {
            *menu_out = menu;
            *command_out = NULL;
            return true;
        }
    }

    return false;
}

static void embcli_append_text(char *buffer, size_t buffer_size, size_t *offset, const char *text) {
    if (*offset >= buffer_size) {
        return;
    }

    int written = snprintf(buffer + *offset, buffer_size - *offset, "%s", text);
    if (written < 0) {
        return;
    }

    size_t advance = (size_t)written;
    if (advance >= buffer_size - *offset) {
        *offset = buffer_size - 1;
        return;
    }
    *offset += advance;
}

static void embcli_append_prompt_path(const embcli_session_t *session, char *buffer, size_t buffer_size) {
    const embcli_menu_t *stack[EMBCLI_MAX_PATH_DEPTH];
    size_t depth = 0;
    const embcli_menu_t *menu = session->current_menu;
    size_t offset = 0;

    if (session->cli->name != NULL && session->cli->name[0] != '\0') {
        embcli_append_text(buffer, buffer_size, &offset, session->cli->name);
    } else {
        embcli_append_text(buffer, buffer_size, &offset, "cli");
    }

    while (menu != NULL && menu->parent != NULL && depth < EMBCLI_MAX_PATH_DEPTH) {
        stack[depth++] = menu;
        menu = menu->parent;
    }

    for (size_t index = depth; index > 0; --index) {
        embcli_append_text(buffer, buffer_size, &offset, "/");
        embcli_append_text(buffer, buffer_size, &offset, stack[index - 1]->name);
    }
}

/* 渲染 `command <required> [optional] ...` 形式的 usage，供菜单/帮助/补全展示。 */
static void embcli_build_command_usage(
    const embcli_command_t *command,
    char *buffer,
    size_t buffer_size) {
    size_t offset = 0;
    embcli_append_text(buffer, buffer_size, &offset, command->name);

    for (size_t index = 0; index < command->arg_count; ++index) {
        const embcli_arg_spec_t *spec = &command->args[index];
        embcli_append_text(buffer, buffer_size, &offset, " ");
        embcli_append_text(buffer, buffer_size, &offset, spec->optional ? "[" : "<");
        embcli_append_text(buffer, buffer_size, &offset, spec->name);
        embcli_append_text(buffer, buffer_size, &offset, spec->optional ? "]" : ">");
    }
}

static void embcli_print_command_detail(embcli_session_t *session, const embcli_command_t *command) {
    char usage[EMBCLI_MAX_USAGE];
    embcli_build_command_usage(command, usage, sizeof(usage));

    embcli_session_printf(session, "command : %s\r\n", command->name);
    embcli_session_printf(session, "summary : %s\r\n", command->summary != NULL ? command->summary : "-");
    embcli_session_printf(session, "usage   : %s\r\n", usage);

    for (size_t index = 0; index < command->arg_count; ++index) {
        const embcli_arg_spec_t *spec = &command->args[index];
        embcli_session_printf(
            session,
            "  %-10s %-7s %s\r\n",
            spec->name,
            spec->optional ? "optional" : "required",
            spec->help != NULL ? spec->help : "-");
    }
}

static void embcli_print_submenu_detail(embcli_session_t *session, const embcli_menu_t *menu) {
    embcli_session_printf(session, "menu    : %s\r\n", menu->name);
    embcli_session_printf(session, "summary : %s\r\n", menu->summary != NULL ? menu->summary : "-");
    embcli_session_printf(session, "enter   : %s\r\n", menu->name);
}

static bool embcli_tokenize(
    char *buffer,
    embcli_token_t *tokens,
    size_t max_tokens,
    size_t *token_count,
    const char **error_text) {
    char *cursor = buffer;
    size_t count = 0;

    /*
     * 这里刻意采用“类 shell 但足够小”的分词规则：
     * - 空白字符分隔 token
     * - 单引号/双引号可保留空格
     * - 反斜杠转义下一个字符
     * 这样既能保持实现轻量，也足够覆盖 telnet CLI 的常见输入场景。
     */
    while (*cursor != '\0') {
        while (isspace((unsigned char)*cursor)) {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        if (count >= max_tokens) {
            *error_text = "too many tokens";
            return false;
        }

        char *start = cursor;
        char *write = cursor;
        bool escape = false;
        char quote = '\0';

        while (*cursor != '\0') {
            char ch = *cursor++;
            if (escape) {
                *write++ = ch;
                escape = false;
                continue;
            }
            if (ch == '\\') {
                escape = true;
                continue;
            }
            if (quote != '\0') {
                if (ch == quote) {
                    quote = '\0';
                    continue;
                }
                *write++ = ch;
                continue;
            }
            if (ch == '"' || ch == '\'') {
                quote = ch;
                continue;
            }
            if (isspace((unsigned char)ch)) {
                break;
            }
            *write++ = ch;
        }

        if (escape) {
            *error_text = "dangling escape";
            return false;
        }
        if (quote != '\0') {
            *error_text = "unterminated quote";
            return false;
        }

        *write = '\0';
        tokens[count++].text = start;
        cursor = (*cursor == '\0') ? cursor : cursor;
    }

    *token_count = count;
    return true;
}

static bool embcli_parse_bool(const char *text, bool *value) {
    static const char *truthy[] = { "1", "true", "on", "yes", "enable", "enabled" };
    static const char *falsy[] = { "0", "false", "off", "no", "disable", "disabled" };

    for (size_t index = 0; index < EMBCLI_ARRAY_SIZE(truthy); ++index) {
        if (embcli_stricmp(text, truthy[index]) == 0) {
            *value = true;
            return true;
        }
    }
    for (size_t index = 0; index < EMBCLI_ARRAY_SIZE(falsy); ++index) {
        if (embcli_stricmp(text, falsy[index]) == 0) {
            *value = false;
            return true;
        }
    }
    return false;
}

static bool embcli_join_rest(
    char *buffer,
    size_t buffer_size,
    const embcli_token_t *tokens,
    size_t begin,
    size_t token_count) {
    size_t offset = 0;

    /* REST 参数会把剩余 token 重新拼接起来，并保留单词间空格。 */
    if (begin >= token_count) {
        if (buffer_size > 0) {
            buffer[0] = '\0';
        }
        return true;
    }

    for (size_t index = begin; index < token_count; ++index) {
        const char *token = tokens[index].text;
        size_t length = strlen(token);
        if (offset != 0) {
            if (offset + 1 >= buffer_size) {
                return false;
            }
            buffer[offset++] = ' ';
        }
        if (offset + length >= buffer_size) {
            return false;
        }
        memcpy(buffer + offset, token, length);
        offset += length;
    }

    buffer[offset] = '\0';
    return true;
}

static bool embcli_parse_values(
    embcli_session_t *session,
    const embcli_command_t *command,
    const embcli_token_t *tokens,
    size_t token_count,
    embcli_value_t *values,
    char *rest_buffer,
    size_t rest_buffer_size) {
    size_t token_index = 1;

    /*
     * token 0 是命令字，后续 token 按参数表从左到右依次匹配。
     */
    for (size_t index = 0; index < command->arg_count; ++index) {
        const embcli_arg_spec_t *spec = &command->args[index];
        embcli_value_t *value = &values[index];
        memset(value, 0, sizeof(*value));
        value->name = spec->name;
        value->type = spec->type;

        if (spec->type == EMBCLI_ARG_REST) {
            /* REST 是唯一可变长参数类型，一旦命中就结束后续参数解析。 */
            if (token_index >= token_count) {
                if (!spec->optional) {
                    embcli_session_printf(session, "missing argument: %s\r\n", spec->name);
                    return false;
                }
                continue;
            }
            if (!embcli_join_rest(rest_buffer, rest_buffer_size, tokens, token_index, token_count)) {
                embcli_session_printf(session, "argument too long: %s\r\n", spec->name);
                return false;
            }
            value->present = true;
            value->text = rest_buffer;
            value->as.str = rest_buffer;
            token_index = token_count;
            continue;
        }

        if (token_index >= token_count) {
            if (!spec->optional) {
                embcli_session_printf(session, "missing argument: %s\r\n", spec->name);
                return false;
            }
            continue;
        }

        const char *text = tokens[token_index++].text;
        value->present = true;
        value->text = text;

        switch (spec->type) {
        case EMBCLI_ARG_STRING:
            value->as.str = text;
            break;
        case EMBCLI_ARG_INT: {
            errno = 0;
            char *end = NULL;
            long long parsed = strtoll(text, &end, 0);
            if (*text == '\0' || end == NULL || *end != '\0') {
                embcli_session_printf(session, "invalid integer for %s: %s\r\n", spec->name, text);
                return false;
            }
            if (errno == ERANGE || (int64_t)parsed < spec->int_min || (int64_t)parsed > spec->int_max) {
                embcli_session_printf(
                    session,
                    "out of range for %s: %" PRId64 " .. %" PRId64 "\r\n",
                    spec->name,
                    spec->int_min,
                    spec->int_max);
                return false;
            }
            value->as.i64 = (int64_t)parsed;
            break;
        }
        case EMBCLI_ARG_UINT: {
            errno = 0;
            char *end = NULL;
            if (text[0] == '-') {
                embcli_session_printf(session, "invalid unsigned integer for %s: %s\r\n", spec->name, text);
                return false;
            }
            unsigned long long parsed = strtoull(text, &end, 0);
            if (*text == '\0' || end == NULL || *end != '\0') {
                embcli_session_printf(session, "invalid unsigned integer for %s: %s\r\n", spec->name, text);
                return false;
            }
            if (errno == ERANGE || (uint64_t)parsed < spec->uint_min || (uint64_t)parsed > spec->uint_max) {
                embcli_session_printf(
                    session,
                    "out of range for %s: %" PRIu64 " .. %" PRIu64 "\r\n",
                    spec->name,
                    spec->uint_min,
                    spec->uint_max);
                return false;
            }
            value->as.u64 = (uint64_t)parsed;
            break;
        }
        case EMBCLI_ARG_BOOL:
            if (!embcli_parse_bool(text, &value->as.boolean)) {
                embcli_session_printf(session, "invalid boolean for %s: %s\r\n", spec->name, text);
                return false;
            }
            break;
        case EMBCLI_ARG_ENUM: {
            bool matched = false;
            for (size_t enum_index = 0; enum_index < spec->enum_value_count; ++enum_index) {
                if (embcli_stricmp(text, spec->enum_values[enum_index]) == 0) {
                    value->as.enum_index = enum_index;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                embcli_session_printf(session, "invalid enum for %s: %s\r\n", spec->name, text);
                return false;
            }
            break;
        }
        case EMBCLI_ARG_REST:
            break;
        }
    }

    if (token_index != token_count) {
        embcli_session_printf(session, "too many arguments\r\n");
        return false;
    }

    return true;
}

static void embcli_show_help(embcli_session_t *session, const char *target_name) {
    embcli_t *cli = session->cli;

    if (target_name == NULL || *target_name == '\0') {
        embcli_session_show_current_menu(session);
        return;
    }

    embcli_read_lock(cli);
    if (strchr(target_name, '/') != NULL) {
        const embcli_menu_t *menu = NULL;
        const embcli_command_t *command = NULL;

        if (embcli_resolve_path(session, target_name, &menu, &command)) {
            if (command != NULL) {
                embcli_print_command_detail(session, command);
            } else {
                embcli_print_submenu_detail(session, menu);
            }
            embcli_unlock(cli);
            return;
        }
    }

    const embcli_menu_t *menu = embcli_find_child_menu(session->current_menu, target_name);
    if (menu != NULL) {
        embcli_print_submenu_detail(session, menu);
        embcli_unlock(cli);
        return;
    }

    const embcli_command_t *command = embcli_find_command(session->current_menu, target_name);
    if (command != NULL) {
        embcli_print_command_detail(session, command);
        embcli_unlock(cli);
        return;
    }
    embcli_unlock(cli);

    embcli_session_printf(session, "no such item: %s\r\n", target_name);
}

void embcli_init(embcli_t *cli, const char *name, const char *banner) {
    memset(cli, 0, sizeof(*cli));
    cli->name = name;
    cli->banner = banner;
    pthread_rwlock_init(&cli->tree_lock, NULL);
    embcli_menu_init(&cli->root_menu, "", "root");
    cli->root_menu.owner = cli;
}

void embcli_deinit(embcli_t *cli) {
    if (cli == NULL) {
        return;
    }
    pthread_rwlock_destroy(&cli->tree_lock);
}

embcli_menu_t *embcli_root_menu(embcli_t *cli) {
    return &cli->root_menu;
}

void embcli_menu_init(embcli_menu_t *menu, const char *name, const char *summary) {
    memset(menu, 0, sizeof(*menu));
    menu->name = name;
    menu->summary = summary;
}

void embcli_command_init(
    embcli_command_t *command,
    const char *name,
    const char *summary,
    const embcli_arg_spec_t *args,
    size_t arg_count,
    embcli_handler_t handler,
    void *user_data) {
    memset(command, 0, sizeof(*command));
    command->name = name;
    command->summary = summary;
    command->args = args;
    command->arg_count = arg_count;
    command->handler = handler;
    command->user_data = user_data;
}

void embcli_menu_add_menu(embcli_menu_t *parent, embcli_menu_t *child) {
    embcli_t *owner = parent->owner;
    embcli_menu_t **tail = &parent->children;

    if (owner != NULL) {
        embcli_write_lock(owner);
    }
    child->parent = parent;
    child->next = NULL;
    embcli_menu_assign_owner_recursive(child, owner);
    while (*tail != NULL) {
        tail = &(*tail)->next;
    }
    *tail = child;
    if (owner != NULL) {
        embcli_unlock(owner);
    }
}

void embcli_menu_add_command(embcli_menu_t *menu, embcli_command_t *command) {
    embcli_t *owner = menu->owner;
    embcli_command_t **tail = &menu->commands;

    if (owner != NULL) {
        embcli_write_lock(owner);
    }
    command->next = NULL;
    while (*tail != NULL) {
        tail = &(*tail)->next;
    }
    *tail = command;
    if (owner != NULL) {
        embcli_unlock(owner);
    }
}

void embcli_session_init(
    embcli_session_t *session,
    embcli_t *cli,
    embcli_write_fn writer,
    void *writer_ctx) {
    memset(session, 0, sizeof(*session));
    session->cli = cli;
    session->current_menu = &cli->root_menu;
    session->writer = writer;
    session->writer_ctx = writer_ctx;
}

void embcli_session_write(embcli_session_t *session, const char *text) {
    if (session->writer != NULL && text != NULL) {
        session->writer(session->writer_ctx, text, strlen(text));
    }
}

void embcli_session_printf(embcli_session_t *session, const char *fmt, ...) {
    char buffer[512];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    embcli_session_write(session, buffer);
}

void embcli_session_request_close(embcli_session_t *session) {
    session->close_requested = true;
}

const char *embcli_value_string(const embcli_value_t *value) {
    if (value == NULL || !value->present) {
        return NULL;
    }
    if (value->type == EMBCLI_ARG_STRING || value->type == EMBCLI_ARG_REST) {
        return value->as.str;
    }
    return value->text;
}

void embcli_session_show_current_menu(embcli_session_t *session) {
    char prompt_path[EMBCLI_MAX_PROMPT];
    embcli_t *cli = session->cli;

    embcli_read_lock(cli);
    embcli_append_prompt_path(session, prompt_path, sizeof(prompt_path));

    embcli_session_printf(session, "\r\n[%s]\r\n", prompt_path);

    /* 先列出子菜单，让界面阅读顺序更接近目录浏览。 */
    const embcli_menu_t *menu = session->current_menu->children;
    while (menu != NULL) {
        embcli_session_printf(
            session,
            "  %-16s %s\r\n",
            menu->name,
            menu->summary != NULL ? menu->summary : "(menu)");
        menu = menu->next;
    }

    /* 再列出命令，每条命令都带 usage 签名。 */
    const embcli_command_t *command = session->current_menu->commands;
    while (command != NULL) {
        char usage[EMBCLI_MAX_USAGE];
        embcli_build_command_usage(command, usage, sizeof(usage));
        embcli_session_printf(
            session,
            "  %-16s %s\r\n",
            usage,
            command->summary != NULL ? command->summary : "-");
        command = command->next;
    }

    embcli_session_write(session, "  help [name]      show current menu or item detail\r\n");
    if (session->current_menu->parent != NULL) {
        embcli_session_write(session, "  back             leave current menu\r\n");
    }
    embcli_session_write(session, "  exit             close current session\r\n");
    embcli_unlock(cli);
}

void embcli_session_format_prompt(
    embcli_session_t *session,
    char *buffer,
    size_t buffer_size) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    embcli_read_lock(session->cli);
    embcli_append_prompt_path(session, buffer, buffer_size);
    embcli_unlock(session->cli);
}

void embcli_session_prompt(embcli_session_t *session) {
    char prompt[EMBCLI_MAX_PROMPT];
    embcli_session_format_prompt(session, prompt, sizeof(prompt));
    embcli_session_printf(session, "%s> ", prompt);
}

void embcli_session_start(embcli_session_t *session) {
    if (session->cli->banner != NULL && session->cli->banner[0] != '\0') {
        embcli_session_printf(session, "%s\r\n", session->cli->banner);
    }
    embcli_session_show_current_menu(session);
    embcli_session_prompt(session);
}

void embcli_session_process_line(embcli_session_t *session, const char *line) {
    char line_buffer[EMBCLI_MAX_LINE];
    embcli_token_t tokens[EMBCLI_MAX_TOKENS];
    embcli_value_t values[EMBCLI_MAX_TOKENS];
    char rest_buffer[EMBCLI_MAX_LINE];
    embcli_t *cli = session->cli;
    size_t token_count = 0;
    const char *token_error = NULL;

    if (line == NULL) {
        return;
    }

    if (strlen(line) >= sizeof(line_buffer)) {
        embcli_session_write(session, "input too long\r\n");
        embcli_session_prompt(session);
        return;
    }

    snprintf(line_buffer, sizeof(line_buffer), "%s", line);
    if (!embcli_tokenize(line_buffer, tokens, EMBCLI_MAX_TOKENS, &token_count, &token_error)) {
        embcli_session_printf(session, "parse error: %s\r\n", token_error);
        embcli_session_prompt(session);
        return;
    }

    if (token_count == 0) {
        embcli_session_prompt(session);
        return;
    }

    /* 内建命令优先于用户注册项解析。 */
    const char *verb = tokens[0].text;

    if (strcmp(verb, "help") == 0) {
        embcli_show_help(session, token_count > 1 ? tokens[1].text : NULL);
        embcli_session_prompt(session);
        return;
    }

    if (strcmp(verb, "back") == 0) {
        if (session->current_menu->parent == NULL) {
            embcli_session_write(session, "already at root menu\r\n");
        } else {
            embcli_session_printf(session, "leave menu: %s\r\n", session->current_menu->name);
            session->current_menu = session->current_menu->parent;
            embcli_session_show_current_menu(session);
        }
        embcli_session_prompt(session);
        return;
    }

    if (strcmp(verb, "exit") == 0 || strcmp(verb, "quit") == 0) {
        embcli_session_write(session, "session closed\r\n");
        embcli_session_request_close(session);
        return;
    }

    embcli_read_lock(cli);
    if (strchr(verb, '/') != NULL) {
        const embcli_menu_t *resolved_menu = NULL;
        const embcli_command_t *resolved_command = NULL;

        if (embcli_resolve_path(session, verb, &resolved_menu, &resolved_command)) {
            embcli_unlock(cli);

            if (resolved_command != NULL) {
                memset(values, 0, sizeof(values));
                if (!embcli_parse_values(
                        session,
                        resolved_command,
                        tokens,
                        token_count,
                        values,
                        rest_buffer,
                        sizeof(rest_buffer))) {
                    embcli_print_command_detail(session, resolved_command);
                    embcli_session_prompt(session);
                    return;
                }

                if (resolved_command->handler != NULL) {
                    resolved_command->handler(
                        session,
                        values,
                        resolved_command->arg_count,
                        resolved_command->user_data);
                }

                if (!session->close_requested) {
                    embcli_session_prompt(session);
                }
                return;
            }

            if (token_count == 1) {
                session->current_menu = (embcli_menu_t *)resolved_menu;
                embcli_session_printf(session, "enter menu: %s\r\n", verb);
                embcli_session_show_current_menu(session);
                embcli_session_prompt(session);
                return;
            }

            embcli_session_printf(session, "unknown command: %s\r\n", verb);
            embcli_session_prompt(session);
            return;
        }
    }

    const embcli_menu_t *child_menu = embcli_find_child_menu(session->current_menu, verb);
    if (child_menu != NULL && token_count == 1) {
        /* 进入菜单的行为被建模为“输入菜单名并回车”。 */
        embcli_unlock(cli);
        session->current_menu = (embcli_menu_t *)child_menu;
        embcli_session_printf(session, "enter menu: %s\r\n", child_menu->name);
        embcli_session_show_current_menu(session);
        embcli_session_prompt(session);
        return;
    }

    const embcli_command_t *command = embcli_find_command(session->current_menu, verb);
    embcli_unlock(cli);
    if (command == NULL) {
        embcli_session_printf(session, "unknown command: %s\r\n", verb);
        embcli_session_prompt(session);
        return;
    }

    memset(values, 0, sizeof(values));
    if (!embcli_parse_values(
            session,
            command,
            tokens,
            token_count,
            values,
            rest_buffer,
            sizeof(rest_buffer))) {
        embcli_print_command_detail(session, command);
        embcli_session_prompt(session);
        return;
    }

    /* 只有当整组参数全部通过校验后，才会调用业务处理函数。 */
    if (command->handler != NULL) {
        command->handler(session, values, command->arg_count, command->user_data);
    }

    if (!session->close_requested) {
        embcli_session_prompt(session);
    }
}
