#include "embcli/embcli_telnet.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
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
#include <unistd.h>

#define TELNET_IAC 255
#define TELNET_DONT 254
#define TELNET_DO 253
#define TELNET_WONT 252
#define TELNET_WILL 251
#define TELNET_SB 250
#define TELNET_SE 240
#define TELNET_OPT_ECHO 1
#define TELNET_OPT_SUPPRESS_GA 3

#define EMBCLI_TELNET_LINE_MAX 512
#define EMBCLI_TELNET_HISTORY_MAX 16
#define EMBCLI_TELNET_MATCH_MAX 32
#define EMBCLI_TELNET_USAGE_MAX 256

typedef struct embcli_telnet_client {
    embcli_telnet_server_t *server;
    int socket_fd;
} embcli_telnet_client_t;

/*
 * Telnet 帧状态。
 * CLI 会把 telnet 控制字节和用户输入文本分开处理，避免选项协商和子协商内容
 * 混入行编辑器。
 */
typedef enum embcli_telnet_state {
    EMBCLI_TELNET_DATA = 0,
    EMBCLI_TELNET_IAC,
    EMBCLI_TELNET_IAC_OPTION,
    EMBCLI_TELNET_SB,
    EMBCLI_TELNET_SB_IAC
} embcli_telnet_state_t;

/* 仅跟踪最小范围的 ANSI 转义序列，用于方向键历史导航。 */
typedef enum embcli_ansi_state {
    EMBCLI_ANSI_NONE = 0,
    EMBCLI_ANSI_ESC,
    EMBCLI_ANSI_CSI
} embcli_ansi_state_t;

/*
 * 每个客户端各自拥有一份行编辑状态。
 * - `line`: 当前可见的命令行内容
 * - `scratch`: 浏览历史时暂存的未提交输入
 * - `history`: 用紧凑数组实现的有界历史记录
 */
typedef struct embcli_telnet_editor {
    char line[EMBCLI_TELNET_LINE_MAX];
    size_t line_len;
    char scratch[EMBCLI_TELNET_LINE_MAX];
    bool scratch_valid;
    char history[EMBCLI_TELNET_HISTORY_MAX][EMBCLI_TELNET_LINE_MAX];
    size_t history_count;
    ssize_t history_index;
    embcli_ansi_state_t ansi_state;
} embcli_telnet_editor_t;

/*
 * 补全结果会先收集到一个扁平数组里，之后再决定是：
 * - 作为可替换候选列表渲染
 * - 还是作为仅提示信息渲染
 *   例如 UINT/STRING 这类不适合自动猜值的参数
 */
typedef struct embcli_completion_result {
    struct {
        const char *name;
        const char *summary;
        const embcli_command_t *command;
        const embcli_menu_t *menu;
        char summary_storage[EMBCLI_TELNET_USAGE_MAX];
    } matches[EMBCLI_TELNET_MATCH_MAX];
    size_t match_count;
    bool replaceable;
} embcli_completion_result_t;

typedef struct embcli_telnet_builtin {
    const char *name;
    const char *usage;
    const char *summary;
} embcli_telnet_builtin_t;

static void embcli_telnet_session_write(void *ctx, const char *data, size_t len) {
    int socket_fd = *(int *)ctx;
    while (len > 0) {
#ifdef MSG_NOSIGNAL
        ssize_t written = send(socket_fd, data, len, MSG_NOSIGNAL);
#else
        ssize_t written = send(socket_fd, data, len, 0);
#endif
        if (written <= 0) {
            return;
        }
        data += (size_t)written;
        len -= (size_t)written;
    }
}

static void embcli_telnet_send_negotiation(int socket_fd) {
    static const unsigned char negotiation[] = {
        TELNET_IAC, TELNET_WILL, TELNET_OPT_ECHO,
        TELNET_IAC, TELNET_WILL, TELNET_OPT_SUPPRESS_GA
    };
#ifdef MSG_NOSIGNAL
    (void)send(socket_fd, negotiation, sizeof(negotiation), MSG_NOSIGNAL);
#else
    (void)send(socket_fd, negotiation, sizeof(negotiation), 0);
#endif
}

/* 用于在 telnet 连接上输出菜单/帮助/补全信息的便捷函数。 */
static void embcli_telnet_send_text(int socket_fd, const char *text) {
    if (text == NULL) {
        return;
    }

#ifdef MSG_NOSIGNAL
    (void)send(socket_fd, text, strlen(text), MSG_NOSIGNAL);
#else
    (void)send(socket_fd, text, strlen(text), 0);
#endif
}

static void embcli_telnet_send_bell(int socket_fd) {
    static const char bell[] = "\a";
    embcli_telnet_send_text(socket_fd, bell);
}

static const embcli_command_t *embcli_telnet_find_command(
    const embcli_menu_t *menu,
    const char *name) {
    for (const embcli_command_t *command = menu->commands; command != NULL; command = command->next) {
        if (strcmp(command->name, name) == 0) {
            return command;
        }
    }
    return NULL;
}

static void embcli_telnet_build_command_usage(
    const embcli_command_t *command,
    char *buffer,
    size_t buffer_size) {
    size_t offset = 0;

    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    buffer[0] = '\0';
    offset += (size_t)snprintf(buffer + offset, buffer_size - offset, "%s", command->name);
    if (offset >= buffer_size) {
        buffer[buffer_size - 1] = '\0';
        return;
    }

    for (size_t index = 0; index < command->arg_count; ++index) {
        const embcli_arg_spec_t *spec = &command->args[index];
        int written = snprintf(
            buffer + offset,
            buffer_size - offset,
            " %c%s%c",
            spec->optional ? '[' : '<',
            spec->name,
            spec->optional ? ']' : '>');
        if (written < 0) {
            return;
        }
        if ((size_t)written >= buffer_size - offset) {
            offset = buffer_size - 1;
            buffer[offset] = '\0';
            return;
        }
        offset += (size_t)written;
    }
}

static void embcli_telnet_redraw_line(
    int socket_fd,
    embcli_session_t *session,
    const embcli_telnet_editor_t *editor) {
    char prompt[128];
    char buffer[EMBCLI_TELNET_LINE_MAX + 192];

    /* 每次重绘都完整重写 prompt+line，并清掉尾部残留字符。 */
    embcli_session_format_prompt(session, prompt, sizeof(prompt));
    snprintf(buffer, sizeof(buffer), "\r%s> %s\033[K", prompt, editor->line);
    embcli_telnet_send_text(socket_fd, buffer);
}

static void embcli_telnet_editor_reset_navigation(embcli_telnet_editor_t *editor) {
    editor->history_index = -1;
    editor->scratch_valid = false;
}

static void embcli_telnet_editor_set_line(
    embcli_telnet_editor_t *editor,
    const char *text) {
    snprintf(editor->line, sizeof(editor->line), "%s", text != NULL ? text : "");
    editor->line_len = strlen(editor->line);
}

static void embcli_telnet_editor_add_history(
    embcli_telnet_editor_t *editor,
    const char *line) {
    if (line == NULL || line[0] == '\0') {
        return;
    }

    /* 跳过连续重复命令，避免方向键浏览历史时价值过低。 */
    if (editor->history_count > 0 &&
        strcmp(editor->history[editor->history_count - 1], line) == 0) {
        return;
    }

    if (editor->history_count == EMBCLI_TELNET_HISTORY_MAX) {
        memmove(
            &editor->history[0],
            &editor->history[1],
            sizeof(editor->history[0]) * (EMBCLI_TELNET_HISTORY_MAX - 1));
        --editor->history_count;
    }

    snprintf(
        editor->history[editor->history_count],
        sizeof(editor->history[editor->history_count]),
        "%s",
        line);
    ++editor->history_count;
}

static void embcli_telnet_editor_history_up(
    int socket_fd,
    embcli_session_t *session,
    embcli_telnet_editor_t *editor) {
    if (editor->history_count == 0) {
        embcli_telnet_send_bell(socket_fd);
        return;
    }

    if (editor->history_index < 0) {
        snprintf(editor->scratch, sizeof(editor->scratch), "%s", editor->line);
        editor->scratch_valid = true;
        editor->history_index = (ssize_t)editor->history_count - 1;
    } else if (editor->history_index > 0) {
        --editor->history_index;
    } else {
        embcli_telnet_send_bell(socket_fd);
        return;
    }

    embcli_telnet_editor_set_line(editor, editor->history[editor->history_index]);
    embcli_telnet_redraw_line(socket_fd, session, editor);
}

static void embcli_telnet_editor_history_down(
    int socket_fd,
    embcli_session_t *session,
    embcli_telnet_editor_t *editor) {
    if (editor->history_index < 0) {
        embcli_telnet_send_bell(socket_fd);
        return;
    }

    if ((size_t)editor->history_index + 1 < editor->history_count) {
        ++editor->history_index;
        embcli_telnet_editor_set_line(editor, editor->history[editor->history_index]);
    } else {
        editor->history_index = -1;
        embcli_telnet_editor_set_line(editor, editor->scratch_valid ? editor->scratch : "");
        editor->scratch_valid = false;
    }

    embcli_telnet_redraw_line(socket_fd, session, editor);
}

static void embcli_completion_add_builtin_match(
    embcli_completion_result_t *result,
    const embcli_telnet_builtin_t *builtin,
    const char *prefix,
    size_t prefix_len) {
    if (result->match_count >= EMBCLI_TELNET_MATCH_MAX) {
        return;
    }
    if (prefix_len == 0 || strncmp(builtin->name, prefix, prefix_len) == 0) {
        result->matches[result->match_count].name = builtin->name;
        snprintf(
            result->matches[result->match_count].summary_storage,
            sizeof(result->matches[result->match_count].summary_storage),
            "%s",
            builtin->summary != NULL ? builtin->summary : "-");
        result->matches[result->match_count].summary =
            result->matches[result->match_count].summary_storage;
        result->matches[result->match_count].command = NULL;
        result->matches[result->match_count].menu = NULL;
        ++result->match_count;
        result->replaceable = true;
    }
}

static void embcli_completion_add_menu_match(
    embcli_completion_result_t *result,
    const embcli_menu_t *menu,
    const char *prefix,
    size_t prefix_len) {
    if (result->match_count >= EMBCLI_TELNET_MATCH_MAX) {
        return;
    }
    if (prefix_len == 0 || strncmp(menu->name, prefix, prefix_len) == 0) {
        result->matches[result->match_count].name = menu->name;
        snprintf(
            result->matches[result->match_count].summary_storage,
            sizeof(result->matches[result->match_count].summary_storage),
            "%s",
            menu->summary != NULL ? menu->summary : "-");
        result->matches[result->match_count].summary =
            result->matches[result->match_count].summary_storage;
        result->matches[result->match_count].command = NULL;
        result->matches[result->match_count].menu = menu;
        ++result->match_count;
        result->replaceable = true;
    }
}

static void embcli_completion_add_command_match(
    embcli_completion_result_t *result,
    const embcli_command_t *command,
    const char *prefix,
    size_t prefix_len) {
    if (result->match_count >= EMBCLI_TELNET_MATCH_MAX) {
        return;
    }
    if (prefix_len == 0 || strncmp(command->name, prefix, prefix_len) == 0) {
        result->matches[result->match_count].name = command->name;
        snprintf(
            result->matches[result->match_count].summary_storage,
            sizeof(result->matches[result->match_count].summary_storage),
            "%s",
            command->summary != NULL ? command->summary : "-");
        result->matches[result->match_count].summary =
            result->matches[result->match_count].summary_storage;
        result->matches[result->match_count].command = command;
        result->matches[result->match_count].menu = NULL;
        ++result->match_count;
        result->replaceable = true;
    }
}

static void embcli_completion_add_value_match(
    embcli_completion_result_t *result,
    const char *name,
    const char *summary,
    const char *prefix,
    size_t prefix_len) {
    if (result->match_count >= EMBCLI_TELNET_MATCH_MAX) {
        return;
    }
    if (prefix_len == 0 || strncmp(name, prefix, prefix_len) == 0) {
        result->matches[result->match_count].name = name;
        snprintf(
            result->matches[result->match_count].summary_storage,
            sizeof(result->matches[result->match_count].summary_storage),
            "%s",
            summary != NULL ? summary : "-");
        result->matches[result->match_count].summary =
            result->matches[result->match_count].summary_storage;
        result->matches[result->match_count].command = NULL;
        result->matches[result->match_count].menu = NULL;
        ++result->match_count;
        result->replaceable = true;
    }
}

static void embcli_completion_add_hint(
    embcli_completion_result_t *result,
    const char *name,
    const char *summary) {
    if (result->match_count >= EMBCLI_TELNET_MATCH_MAX) {
        return;
    }
    result->matches[result->match_count].name = name;
    snprintf(
        result->matches[result->match_count].summary_storage,
        sizeof(result->matches[result->match_count].summary_storage),
        "%s",
        summary != NULL ? summary : "-");
    result->matches[result->match_count].summary =
        result->matches[result->match_count].summary_storage;
    result->matches[result->match_count].command = NULL;
    result->matches[result->match_count].menu = NULL;
    ++result->match_count;
    result->replaceable = false;
}

static void embcli_telnet_format_arg_hint(
    const embcli_arg_spec_t *spec,
    char *buffer,
    size_t buffer_size) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    switch (spec->type) {
    case EMBCLI_ARG_STRING:
        snprintf(buffer, buffer_size, "string, %s", spec->help != NULL ? spec->help : "free text");
        break;
    case EMBCLI_ARG_INT:
        snprintf(
            buffer,
            buffer_size,
            "int %" PRId64 "..%" PRId64 ", %s",
            spec->int_min,
            spec->int_max,
            spec->help != NULL ? spec->help : "-");
        break;
    case EMBCLI_ARG_UINT:
        snprintf(
            buffer,
            buffer_size,
            "uint %" PRIu64 "..%" PRIu64 ", %s",
            spec->uint_min,
            spec->uint_max,
            spec->help != NULL ? spec->help : "-");
        break;
    case EMBCLI_ARG_BOOL:
        snprintf(buffer, buffer_size, "bool, %s", spec->help != NULL ? spec->help : "-");
        break;
    case EMBCLI_ARG_ENUM:
        snprintf(buffer, buffer_size, "enum, %s", spec->help != NULL ? spec->help : "-");
        break;
    case EMBCLI_ARG_REST:
        snprintf(buffer, buffer_size, "text tail, %s", spec->help != NULL ? spec->help : "-");
        break;
    }
}

static const embcli_arg_spec_t *embcli_telnet_find_active_arg(
    const embcli_command_t *command,
    size_t arg_index) {
    if (command == NULL || command->arg_count == 0) {
        return NULL;
    }

    for (size_t index = 0; index < command->arg_count; ++index) {
        if (command->args[index].type == EMBCLI_ARG_REST && arg_index >= index) {
            return &command->args[index];
        }
    }

    if (arg_index >= command->arg_count) {
        return NULL;
    }

    return &command->args[arg_index];
}

static void embcli_telnet_collect_arg_matches(
    const embcli_command_t *command,
    size_t arg_index,
    const char *prefix,
    embcli_completion_result_t *result) {
    static const struct {
        const char *name;
        const char *summary;
    } bool_values[] = {
        { "true", "boolean true" },
        { "false", "boolean false" },
        { "on", "boolean true" },
        { "off", "boolean false" },
        { "1", "boolean true" },
        { "0", "boolean false" },
        { "yes", "boolean true" },
        { "no", "boolean false" }
    };
    char hint[EMBCLI_TELNET_USAGE_MAX];
    const embcli_arg_spec_t *spec = embcli_telnet_find_active_arg(command, arg_index);

    /*
     * 只对“可以安全猜测”的参数值做自动补全：
     * - 固定枚举值
     * - 预定义的 bool 别名表
     * 其它参数类型则退化为提示信息，仍然给用户提供输入指引。
     */
    if (spec == NULL) {
        return;
    }

    if (spec->type == EMBCLI_ARG_ENUM) {
        size_t prefix_len = strlen(prefix);
        for (size_t index = 0; index < spec->enum_value_count; ++index) {
            embcli_completion_add_value_match(
                result,
                spec->enum_values[index],
                spec->help != NULL ? spec->help : "enum value",
                prefix,
                prefix_len);
        }
        if (result->match_count == 0 && prefix_len != 0) {
            for (size_t index = 0; index < spec->enum_value_count; ++index) {
                embcli_completion_add_value_match(
                    result,
                    spec->enum_values[index],
                    spec->help != NULL ? spec->help : "enum value",
                    "",
                    0);
            }
        }
        return;
    }

    if (spec->type == EMBCLI_ARG_BOOL) {
        size_t prefix_len = strlen(prefix);
        for (size_t index = 0; index < EMBCLI_ARRAY_SIZE(bool_values); ++index) {
            embcli_completion_add_value_match(
                result,
                bool_values[index].name,
                bool_values[index].summary,
                prefix,
                prefix_len);
        }
        if (result->match_count == 0 && prefix_len != 0) {
            for (size_t index = 0; index < EMBCLI_ARRAY_SIZE(bool_values); ++index) {
                embcli_completion_add_value_match(
                    result,
                    bool_values[index].name,
                    bool_values[index].summary,
                    "",
                    0);
            }
        }
        return;
    }

    embcli_telnet_format_arg_hint(spec, hint, sizeof(hint));
    embcli_completion_add_hint(result, spec->name, hint);
}

static void embcli_telnet_collect_completion_matches(
    embcli_session_t *session,
    size_t token_index,
    const char *first_token,
    const char *prefix,
    embcli_completion_result_t *result) {
    static const embcli_telnet_builtin_t builtins_root[] = {
        { "help", "help [name]", "show current menu or item detail" },
        { "exit", "exit", "close current session" },
        { "quit", "quit", "close current session" },
        { "back", "back", "leave current menu" }
    };
    static const embcli_telnet_builtin_t builtins_no_back[] = {
        { "help", "help [name]", "show current menu or item detail" },
        { "exit", "exit", "close current session" },
        { "quit", "quit", "close current session" }
    };
    const embcli_telnet_builtin_t *builtins =
        session->current_menu->parent != NULL ? builtins_root : builtins_no_back;
    size_t builtin_count = session->current_menu->parent != NULL ? 4 : 3;
    size_t prefix_len = strlen(prefix);

    memset(result, 0, sizeof(*result));

    if (token_index == 0) {
        for (size_t index = 0; index < builtin_count; ++index) {
            embcli_completion_add_builtin_match(result, &builtins[index], prefix, prefix_len);
        }

        for (const embcli_menu_t *menu = session->current_menu->children; menu != NULL; menu = menu->next) {
            embcli_completion_add_menu_match(result, menu, prefix, prefix_len);
        }
        for (const embcli_command_t *command = session->current_menu->commands; command != NULL; command = command->next) {
            embcli_completion_add_command_match(result, command, prefix, prefix_len);
        }
        return;
    }

    if (token_index == 1 && first_token != NULL && strcmp(first_token, "help") == 0) {
        for (const embcli_menu_t *menu = session->current_menu->children; menu != NULL; menu = menu->next) {
            embcli_completion_add_menu_match(result, menu, prefix, prefix_len);
        }
        for (const embcli_command_t *command = session->current_menu->commands; command != NULL; command = command->next) {
            embcli_completion_add_command_match(result, command, prefix, prefix_len);
        }
        return;
    }

    if (first_token != NULL) {
        const embcli_command_t *command = embcli_telnet_find_command(session->current_menu, first_token);
        if (command != NULL && token_index >= 1) {
            embcli_telnet_collect_arg_matches(command, token_index - 1, prefix, result);
        }
    }
}

static size_t embcli_telnet_common_prefix_length(
    const embcli_completion_result_t *result) {
    if (result->match_count == 0) {
        return 0;
    }

    size_t length = strlen(result->matches[0].name);
    for (size_t index = 1; index < result->match_count; ++index) {
        size_t cursor = 0;
        while (cursor < length &&
               result->matches[0].name[cursor] != '\0' &&
               result->matches[index].name[cursor] != '\0' &&
               result->matches[0].name[cursor] == result->matches[index].name[cursor]) {
            ++cursor;
        }
        length = cursor;
    }

    return length;
}

static void embcli_telnet_replace_suffix(
    embcli_telnet_editor_t *editor,
    size_t token_begin,
    const char *replacement,
    bool append_space) {
    size_t base_len = token_begin;
    size_t replacement_len = strlen(replacement);
    size_t total_len = base_len + replacement_len + (append_space ? 1U : 0U);

    if (total_len >= sizeof(editor->line)) {
        return;
    }

    memcpy(editor->line + base_len, replacement, replacement_len);
    editor->line_len = base_len + replacement_len;
    if (append_space) {
        editor->line[editor->line_len++] = ' ';
    }
    editor->line[editor->line_len] = '\0';
}

static void embcli_telnet_print_completion_list(
    int socket_fd,
    embcli_session_t *session,
    embcli_telnet_editor_t *editor,
    const embcli_completion_result_t *result) {
    char usage[EMBCLI_TELNET_USAGE_MAX];
    char line[EMBCLI_TELNET_USAGE_MAX + 160];

    embcli_telnet_send_text(socket_fd, "\r\n");
    for (size_t index = 0; index < result->match_count; ++index) {
        if (result->matches[index].command != NULL) {
            embcli_telnet_build_command_usage(
                result->matches[index].command,
                usage,
                sizeof(usage));
        } else {
            snprintf(usage, sizeof(usage), "%s", result->matches[index].name);
        }

        snprintf(
            line,
            sizeof(line),
            "  %-24s %s\r\n",
            usage,
            result->matches[index].summary != NULL ? result->matches[index].summary : "-");
        embcli_telnet_send_text(socket_fd, line);
    }
    embcli_telnet_redraw_line(socket_fd, session, editor);
}

static void embcli_telnet_editor_autocomplete(
    int socket_fd,
    embcli_session_t *session,
    embcli_telnet_editor_t *editor) {
    embcli_completion_result_t result;
    char first_token[EMBCLI_TELNET_LINE_MAX];
    const char *first_token_view = NULL;
    size_t token_begin = 0;
    size_t token_index = 0;
    char prefix[EMBCLI_TELNET_LINE_MAX];
    size_t prefix_len = 0;
    bool in_token = false;

    /*
     * 补全逻辑以 token 为单位工作。
     * 先根据当前行尾判断光标正位于哪个 token，再按该 token 位置收集对应候选。
     */
    if (editor->line_len == 0) {
        embcli_telnet_collect_completion_matches(session, 0, NULL, "", &result);
        embcli_telnet_print_completion_list(socket_fd, session, editor, &result);
        return;
    }

    for (size_t index = 0; index < editor->line_len; ++index) {
        if (editor->line[index] != ' ') {
            if (!in_token) {
                if (token_index == 0) {
                    size_t first_len = 0;
                    while (index + first_len < editor->line_len &&
                           editor->line[index + first_len] != ' ') {
                        ++first_len;
                    }
                    snprintf(first_token, sizeof(first_token), "%.*s", (int)first_len, editor->line + index);
                    first_token_view = first_token;
                }
                in_token = true;
                token_begin = index;
            }
        } else {
            if (in_token) {
                in_token = false;
                ++token_index;
            }
        }
    }

    if (!in_token) {
        token_begin = editor->line_len;
    } else if (strchr(editor->line, ' ') == NULL) {
        token_begin = 0;
    }

    snprintf(prefix, sizeof(prefix), "%s", editor->line + token_begin);
    prefix_len = strlen(prefix);

    embcli_telnet_collect_completion_matches(session, token_index, first_token_view, prefix, &result);

    if (result.match_count == 0) {
        embcli_telnet_send_bell(socket_fd);
        return;
    }

    if (!result.replaceable) {
        embcli_telnet_print_completion_list(socket_fd, session, editor, &result);
        return;
    }

    if (result.match_count == 1) {
        embcli_telnet_replace_suffix(editor, token_begin, result.matches[0].name, true);
        embcli_telnet_redraw_line(socket_fd, session, editor);
        return;
    }

    size_t common_len = embcli_telnet_common_prefix_length(&result);
    if (common_len > prefix_len) {
        char partial[EMBCLI_TELNET_LINE_MAX];
        snprintf(partial, sizeof(partial), "%.*s", (int)common_len, result.matches[0].name);
        embcli_telnet_replace_suffix(editor, token_begin, partial, false);
        embcli_telnet_redraw_line(socket_fd, session, editor);
        return;
    }

    embcli_telnet_print_completion_list(socket_fd, session, editor, &result);
}

static bool embcli_telnet_try_acquire_slot(embcli_telnet_server_t *server) {
    bool accepted = false;

    pthread_mutex_lock(&server->lock);
    if (server->active_clients < server->config.max_clients) {
        ++server->active_clients;
        accepted = true;
    }
    pthread_mutex_unlock(&server->lock);

    return accepted;
}

static void embcli_telnet_release_slot(embcli_telnet_server_t *server) {
    pthread_mutex_lock(&server->lock);
    if (server->active_clients > 0) {
        --server->active_clients;
    }
    pthread_mutex_unlock(&server->lock);
}

static bool embcli_telnet_handle_char(
    int socket_fd,
    embcli_session_t *session,
    embcli_telnet_editor_t *editor,
    char *line,
    size_t *line_len,
    unsigned char byte) {
    (void)line;
    (void)line_len;

    if (byte == 0x08 || byte == 0x7f) {
        if (editor->line_len > 0) {
            --editor->line_len;
            editor->line[editor->line_len] = '\0';
            embcli_telnet_editor_reset_navigation(editor);
            embcli_telnet_redraw_line(socket_fd, session, editor);
        } else {
            embcli_telnet_send_bell(socket_fd);
        }
        return false;
    }

    if (byte == '\t') {
        embcli_telnet_editor_autocomplete(socket_fd, session, editor);
        return false;
    }

    if (byte < 32 || byte > 126) {
        return false;
    }

    if (editor->line_len + 1 >= EMBCLI_TELNET_LINE_MAX) {
        embcli_telnet_send_text(socket_fd, "\r\ninput too long\r\n");
        editor->line_len = 0;
        editor->line[0] = '\0';
        embcli_telnet_editor_reset_navigation(editor);
        embcli_telnet_redraw_line(socket_fd, session, editor);
        return true;
    }

    editor->line[editor->line_len++] = (char)byte;
    editor->line[editor->line_len] = '\0';
    embcli_telnet_editor_reset_navigation(editor);
    embcli_telnet_redraw_line(socket_fd, session, editor);
    return false;
}

static void *embcli_telnet_client_thread(void *arg) {
    embcli_telnet_client_t *client = (embcli_telnet_client_t *)arg;
    embcli_session_t session;
    embcli_telnet_editor_t editor;
    embcli_telnet_state_t state = EMBCLI_TELNET_DATA;
    bool saw_cr = false;

    memset(&editor, 0, sizeof(editor));
    editor.history_index = -1;

    embcli_telnet_send_negotiation(client->socket_fd);
    embcli_session_init(&session, client->server->config.cli, embcli_telnet_session_write, &client->socket_fd);
    embcli_session_start(&session);

    /*
     * 客户端主循环按“行”工作：
     * - 原始字节先经过 telnet/ANSI 状态机过滤
     * - 可打印字符进入行编辑器
     * - CR/LF 把当前逻辑行提交给 CLI 核心
     */
    while (!session.close_requested) {
        unsigned char buffer[128];
        ssize_t received = recv(client->socket_fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }

        for (ssize_t index = 0; index < received; ++index) {
            unsigned char byte = buffer[index];

            switch (state) {
            case EMBCLI_TELNET_DATA:
                if (byte == TELNET_IAC) {
                    state = EMBCLI_TELNET_IAC;
                    continue;
                }
                if (saw_cr) {
                    saw_cr = false;
                    if (byte == '\n') {
                        continue;
                    }
                }
                if (editor.ansi_state == EMBCLI_ANSI_ESC) {
                    if (byte == '[') {
                        editor.ansi_state = EMBCLI_ANSI_CSI;
                    } else {
                        editor.ansi_state = EMBCLI_ANSI_NONE;
                    }
                    continue;
                }
                if (editor.ansi_state == EMBCLI_ANSI_CSI) {
                    editor.ansi_state = EMBCLI_ANSI_NONE;
                    if (byte == 'A') {
                        embcli_telnet_editor_history_up(client->socket_fd, &session, &editor);
                    } else if (byte == 'B') {
                        embcli_telnet_editor_history_down(client->socket_fd, &session, &editor);
                    }
                    continue;
                }
                if (byte == 0x1b) {
                    editor.ansi_state = EMBCLI_ANSI_ESC;
                    continue;
                }
                if (byte == '\r' || byte == '\n') {
                    static const char newline[] = "\r\n";
#ifdef MSG_NOSIGNAL
                    (void)send(client->socket_fd, newline, sizeof(newline) - 1, MSG_NOSIGNAL);
#else
                    (void)send(client->socket_fd, newline, sizeof(newline) - 1, 0);
#endif
                    embcli_telnet_editor_add_history(&editor, editor.line);
                    embcli_session_process_line(&session, editor.line);
                    embcli_telnet_editor_set_line(&editor, "");
                    embcli_telnet_editor_reset_navigation(&editor);
                    if (byte == '\r') {
                        saw_cr = true;
                    }
                    if (session.close_requested) {
                        break;
                    }
                    continue;
                }
                (void)embcli_telnet_handle_char(
                    client->socket_fd,
                    &session,
                    &editor,
                    editor.line,
                    &editor.line_len,
                    byte);
                break;
            case EMBCLI_TELNET_IAC:
                if (byte == TELNET_SB) {
                    state = EMBCLI_TELNET_SB;
                } else if (byte == TELNET_WILL || byte == TELNET_WONT ||
                           byte == TELNET_DO || byte == TELNET_DONT) {
                    state = EMBCLI_TELNET_IAC_OPTION;
                } else {
                    state = EMBCLI_TELNET_DATA;
                }
                break;
            case EMBCLI_TELNET_IAC_OPTION:
                state = EMBCLI_TELNET_DATA;
                break;
            case EMBCLI_TELNET_SB:
                if (byte == TELNET_IAC) {
                    state = EMBCLI_TELNET_SB_IAC;
                }
                break;
            case EMBCLI_TELNET_SB_IAC:
                state = (byte == TELNET_SE) ? EMBCLI_TELNET_DATA : EMBCLI_TELNET_SB;
                break;
            }
        }
    }

    shutdown(client->socket_fd, SHUT_RDWR);
    close(client->socket_fd);
    embcli_telnet_release_slot(client->server);
    free(client);
    return NULL;
}

static int embcli_telnet_make_listener(const embcli_telnet_config_t *config) {
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return -1;
    }

    int enable = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) != 0) {
        close(socket_fd);
        return -1;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(config->port);

    if (config->bind_address == NULL || strcmp(config->bind_address, "0.0.0.0") == 0) {
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, config->bind_address, &address.sin_addr) != 1) {
        close(socket_fd);
        return -1;
    }

    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(socket_fd);
        return -1;
    }

    if (listen(socket_fd, config->backlog) != 0) {
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

static void *embcli_telnet_accept_thread(void *arg) {
    embcli_telnet_server_t *server = (embcli_telnet_server_t *)arg;

    while (server->running) {
        fd_set readfds;
        struct timeval timeout;
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd;

        FD_ZERO(&readfds);
        FD_SET(server->listen_fd, &readfds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;

        /*
         * 使用较短超时让停服响应更及时。
         * 如果只依赖阻塞式 accept，会让 selftest 中的 stop/restart 路径很难稳定覆盖。
         */
        int ready = select(server->listen_fd + 1, &readfds, NULL, NULL, &timeout);
        if (!server->running) {
            break;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            continue;
        }
        if (!FD_ISSET(server->listen_fd, &readfds)) {
            continue;
        }

        client_fd = accept(server->listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (!server->running || errno == EBADF || errno == EINVAL) {
                break;
            }
            continue;
        }

        if (!embcli_telnet_try_acquire_slot(server)) {
            static const char busy[] = "server busy\r\n";
#ifdef MSG_NOSIGNAL
            (void)send(client_fd, busy, sizeof(busy) - 1, MSG_NOSIGNAL);
#else
            (void)send(client_fd, busy, sizeof(busy) - 1, 0);
#endif
            close(client_fd);
            continue;
        }

        embcli_telnet_client_t *client = (embcli_telnet_client_t *)calloc(1, sizeof(*client));
        if (client == NULL) {
            embcli_telnet_release_slot(server);
            close(client_fd);
            continue;
        }

        client->server = server;
        client->socket_fd = client_fd;

        pthread_t thread;
        if (pthread_create(&thread, NULL, embcli_telnet_client_thread, client) != 0) {
            embcli_telnet_release_slot(server);
            close(client_fd);
            free(client);
            continue;
        }
        pthread_detach(thread);
    }

    return NULL;
}

int embcli_telnet_server_start(
    embcli_telnet_server_t *server,
    const embcli_telnet_config_t *config) {
    if (server == NULL || config == NULL || config->cli == NULL || config->port == 0) {
        return -1;
    }

    memset(server, 0, sizeof(*server));
    server->config = *config;
    if (server->config.backlog <= 0) {
        server->config.backlog = 4;
    }
    if (server->config.max_clients <= 0) {
        server->config.max_clients = 2;
    }

    if (pthread_mutex_init(&server->lock, NULL) != 0) {
        return -1;
    }

    server->listen_fd = embcli_telnet_make_listener(&server->config);
    if (server->listen_fd < 0) {
        return -1;
    }

    server->running = true;
    if (pthread_create(&server->accept_thread, NULL, embcli_telnet_accept_thread, server) != 0) {
        server->running = false;
        close(server->listen_fd);
        server->listen_fd = -1;
        return -1;
    }

    return 0;
}

static void embcli_telnet_wake_listener(embcli_telnet_server_t *server) {
    int socket_fd;
    struct sockaddr_in address;
    const char *host = "127.0.0.1";

    if (server == NULL || server->listen_fd < 0) {
        return;
    }

    if (server->config.bind_address != NULL &&
        strcmp(server->config.bind_address, "0.0.0.0") != 0) {
        host = server->config.bind_address;
    }

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(server->config.port);
    /*
     * 通过“连一下再立刻断开”即可唤醒 accept 线程，
     * 这样 stop() 在对方阻塞于 select() 时也能稳定 join。
     */
    if (inet_pton(AF_INET, host, &address.sin_addr) == 1) {
        (void)connect(socket_fd, (struct sockaddr *)&address, sizeof(address));
    }

    close(socket_fd);
}

void embcli_telnet_server_stop(embcli_telnet_server_t *server) {
    if (server == NULL || !server->running) {
        return;
    }

    server->running = false;
    embcli_telnet_wake_listener(server);
    pthread_join(server->accept_thread, NULL);
    shutdown(server->listen_fd, SHUT_RDWR);
    close(server->listen_fd);
    server->listen_fd = -1;
}

bool embcli_telnet_server_is_running(embcli_telnet_server_t *server) {
    bool running;
    if (server == NULL) {
        return false;
    }

    pthread_mutex_lock(&server->lock);
    running = server->running;
    pthread_mutex_unlock(&server->lock);
    return running;
}

int embcli_telnet_server_active_clients(embcli_telnet_server_t *server) {
    int active = 0;
    if (server == NULL) {
        return 0;
    }

    pthread_mutex_lock(&server->lock);
    active = server->active_clients;
    pthread_mutex_unlock(&server->lock);
    return active;
}
