#ifndef EMBCLI_EMBCLI_H
#define EMBCLI_EMBCLI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 供下方注册宏使用的小工具。
 * 这个库主要面向静态表注册场景，因此这里通常传入编译期数组。
 */
#define EMBCLI_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

/*
 * 内置解析器支持的参数类型。
 * - STRING: 经过类 shell 分词后的单个 token
 * - INT/UINT: 数值转换并附带范围检查
 * - BOOL: 支持 on/off、true/false 等多种别名
 * - ENUM: 固定字符串表中的一个值
 * - REST: 吞掉剩余所有 token，组合成一个逻辑字符串
 */
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

/* CLI 会话输出字节流时使用的传输层抽象。 */
typedef void (*embcli_write_fn)(void *ctx, const char *data, size_t len);
/* 参数解析成功后触发的业务回调。 */
typedef void (*embcli_handler_t)(
    embcli_session_t *session,
    const embcli_value_t *values,
    size_t value_count,
    void *user_data);

/*
 * 单个参数的元数据，供帮助信息、校验和补全逻辑使用。
 * 解析器只会读取与当前类型对应的字段。
 */
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

/*
 * 传给回调的已解析参数值。
 * `text` 保留原始 token，`as.*` 提供类型化视图。
 */
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

/* 菜单命令链表中的侵入式节点。 */
struct embcli_command {
    const char *name;
    const char *summary;
    const embcli_arg_spec_t *args;
    size_t arg_count;
    embcli_handler_t handler;
    void *user_data;
    embcli_command_t *next;
};

/* 菜单树中的侵入式节点。菜单构成树，命令则挂在各自菜单下。 */
struct embcli_menu {
    const char *name;
    const char *summary;
    embcli_t *owner;
    embcli_menu_t *parent;
    embcli_menu_t *children;
    embcli_menu_t *next;
    embcli_command_t *commands;
};

/*
 * 顶层 CLI 对象。内嵌 root 菜单以避免额外分配。
 * tree_lock 保护整棵菜单/命令树，支持多线程动态注册与多线程查询并发。
 */
struct embcli {
    const char *name;
    const char *banner;
    pthread_rwlock_t tree_lock;
    embcli_menu_t root_menu;
};

/* 每个连接对应的执行上下文。 */
struct embcli_session {
    embcli_t *cli;
    embcli_menu_t *current_menu;
    embcli_write_fn writer;
    void *writer_ctx;
    bool close_requested;
};

/* 面向静态注册风格的便捷宏。 */
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
    { (menu_name), (menu_summary), NULL, NULL, NULL, NULL, NULL }

/* 初始化 CLI 根对象以及隐含的 root 菜单。 */
void embcli_init(embcli_t *cli, const char *name, const char *banner);
/* 释放 CLI 内部资源。若对象会反复 init/deinit，建议显式调用。 */
void embcli_deinit(embcli_t *cli);
/* 单独提供 accessor，避免调用方依赖 `struct embcli` 的布局。 */
embcli_menu_t *embcli_root_menu(embcli_t *cli);

/* 与上方静态便捷宏等价的运行时初始化接口。 */
void embcli_menu_init(embcli_menu_t *menu, const char *name, const char *summary);
void embcli_command_init(
    embcli_command_t *command,
    const char *name,
    const char *summary,
    const embcli_arg_spec_t *args,
    size_t arg_count,
    embcli_handler_t handler,
    void *user_data);

/*
 * 将子菜单或命令追加到对应链表尾部。
 * 这两个接口支持多线程动态注册，内部会对整棵树加写锁。
 * 约束如下：
 * - 仅支持运行时追加，不支持删除已注册节点
 * - 同一个 menu/command 节点不应重复挂接到多处
 * - 注册对象及其引用的字符串/参数表生命周期需由调用方保证
 */
void embcli_menu_add_menu(embcli_menu_t *parent, embcli_menu_t *child);
void embcli_menu_add_command(embcli_menu_t *menu, embcli_command_t *command);

/*
 * 会话生命周期接口。一个会话通常对应一个 telnet/socket 客户端。
 * 菜单树查询可与其他线程的动态注册并发执行；
 * 但同一个 session 对象仍然只允许单线程串行使用。
 */
void embcli_session_init(
    embcli_session_t *session,
    embcli_t *cli,
    embcli_write_fn writer,
    void *writer_ctx);
void embcli_session_start(embcli_session_t *session);
void embcli_session_process_line(embcli_session_t *session, const char *line);
void embcli_session_show_current_menu(embcli_session_t *session);
/* 仅格式化提示符的路径部分，不带结尾的 "> "。 */
void embcli_session_format_prompt(
    embcli_session_t *session,
    char *buffer,
    size_t buffer_size);
void embcli_session_prompt(embcli_session_t *session);

/* 供回调和传输层使用的输出辅助接口。 */
void embcli_session_write(embcli_session_t *session, const char *text);
void embcli_session_printf(embcli_session_t *session, const char *fmt, ...);
/* 请求外层传输循环关闭当前会话。 */
void embcli_session_request_close(embcli_session_t *session);

/* 当调用方只关心字符串形式时使用的便捷 accessor。 */
const char *embcli_value_string(const embcli_value_t *value);

#ifdef __cplusplus
}
#endif

#endif
