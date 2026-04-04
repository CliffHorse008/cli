# embcli

面向嵌入式场景的轻量 CLI 面板，内嵌 telnet server，对外只暴露少量注册接口和回调式命令处理函数。

## 已实现能力

- 标准 TCP/telnet 接入，客户端可直接使用 `telnet` 连接
- 菜单式 CLI，支持进入子菜单、退出子菜单、显示当前菜单内容
- 对外接口简单，支持静态宏定义或运行时函数注册
- 参数类型较丰富
- 命令帮助、用法说明、参数校验、布尔/枚举解析
- `Tab` 自动补全并显示候选的 `usage/summary`，命令参数支持 `ENUM/BOOL` 级补全，其它类型给出参数提示，支持 `↑/↓` 历史命令浏览和 `←/→` 行内光标移动

当前参数类型包括：

- `STRING`
- `INT`
- `UINT`
- `BOOL`
- `ENUM`
- `REST`，用于接收剩余整段文本

## 目录

- `include/embcli/embcli.h`：CLI 核心 API
- `include/embcli/embcli_telnet.h`：telnet server API
- `src/embcli.c`：菜单、命令、参数解析
- `src/embcli_telnet.c`：telnet 接入实现
- `demo/main.c`：telnet server 示例程序
- `demo/demo_app.c`：demo CLI 构建逻辑
- `demo/selftest.c`：功能遍历与压力测试程序

## 快速构建

```bash
cmake -S . -B build
cmake --build build
```

运行：

```bash
./build/embcli_demo
```

默认只监听 `127.0.0.1`，便于先本地调试、不直接暴露到外网。

指定端口运行：

```bash
./build/embcli_demo 2323 0.0.0.0
```

再用 telnet 连接：

```bash
telnet 127.0.0.1 2323
```

自动化演示和压力测试：

```bash
./build/embcli_demo_selftest
```

自定义压力参数：

```bash
./build/embcli_demo_selftest 2423 40 6 20
```

也可以直接走 `ctest`：

```bash
cd build
ctest --output-on-failure
```

## 最小接入方式

### 1. 定义参数

```c
static const char *levels[] = { "debug", "info", "warn", "error" };

static const embcli_arg_spec_t log_args[] = {
    EMBCLI_ARG_ENUM_REQ("level", "debug/info/warn/error", levels)
};
```

### 2. 定义回调

```c
static void cmd_log_level(
    embcli_session_t *session,
    const embcli_value_t *values,
    size_t value_count,
    void *user_data) {
    (void)value_count;
    (void)user_data;
    embcli_session_printf(session, "log level => %s\r\n", embcli_value_string(&values[0]));
}
```

### 3. 定义命令和菜单

```c
static embcli_menu_t system_menu = EMBCLI_MENU_DEF("system", "system control");
static embcli_command_t cmd_log =
    EMBCLI_COMMAND_DEF("log-level", "set log level", log_args, cmd_log_level, NULL);

embcli_menu_add_menu(embcli_root_menu(&cli), &system_menu);
embcli_menu_add_command(&system_menu, &cmd_log);
```

### 4. 启动 telnet server

```c
embcli_telnet_config_t config = {
    .cli = &cli,
    .bind_address = "0.0.0.0",
    .port = 2323,
    .backlog = 4,
    .max_clients = 4,
};

embcli_telnet_server_t server;
embcli_telnet_server_start(&server, &config);
```

运行中切换监听地址：

```c
embcli_telnet_server_rebind(&server, "0.0.0.0");   /* 允许外网访问 */
embcli_telnet_server_rebind(&server, "127.0.0.1"); /* 恢复仅本地访问 */
```

查询当前监听地址：

```c
const char *bind = embcli_telnet_server_bind_address(&server);
```

## 菜单行为

连接后会显示当前菜单项和命令列表。

进入子菜单：

```text
board> system
```

也支持路径式进入，适合脚本从任意位置直接跳转：

```text
board> /system
board/system> /network
```

退出子菜单：

```text
board/system> back
```

执行子菜单命令时，除了先进入菜单再输入命令，也支持一条路径命令直接执行：

```text
board> system/log-level warn
board> /network/config 192.168.10.2 255.255.255.0 192.168.10.1
board/system> /device/set 2 on true
```

路径式执行命令不会修改当前所在菜单，因此很适合外部自动化脚本调用。

demo 还提供了运行时切换 telnet 暴露范围的命令：

```text
board> system/telnet-access on
board> system/telnet-access off
```

`on` 会把监听地址切到 `0.0.0.0`，`off` 会切回 `127.0.0.1`。

查看帮助：

```text
help
help reboot
help system
help system/reboot
help /network/config
```

关闭会话：

```text
exit
```

自动补全和历史：

```text
bo<Tab>         -> board 当前菜单下补全命令或菜单
help re<Tab>    -> 补全 help 的目标项
<Tab>           -> 列出当前菜单下候选及说明
log-level d<Tab> -> 补全枚举参数，如 debug
set 1 o<Tab>    -> 补全布尔参数候选，如 on/off
set <Tab>       -> 对不可自动补全的参数显示类型/范围提示
↑ / ↓           -> 浏览本会话历史命令
```

## 脚本调用建议

如果外部脚本需要一步执行深层菜单命令，推荐直接发送路径式命令，而不是先切菜单再发送第二条命令：

```bash
printf 'system/log-level warn\r' | nc 127.0.0.1 2323
printf '/network/config 192.168.10.2 255.255.255.0 192.168.10.1\r' | nc 127.0.0.1 2323
```

如果脚本是在某个已进入的菜单下运行，使用以 `/` 开头的绝对路径更稳定，不受当前菜单位置影响。

## 设计说明

- CLI 核心与传输层解耦，`embcli_session_*` 只依赖输出回调
- telnet server 只是一个适配层，后续很容易换成串口、UART、WebSocket 或自定义 socket
- 菜单与命令均支持静态对象，适合嵌入式固件常见的静态内存使用习惯
- 参数定义放在 `embcli_arg_spec_t` 中，注册接口较少，便于封装成更上层宏

## Demo 覆盖与压测

`embcli_demo_selftest` 会自动覆盖以下内容：

- `embcli_session_process_line()` 直接接口解析，包括引用、转义、缺参、多参、过多 token、超长输入
- 首屏 banner、根菜单、提示符
- 菜单进入/退出、`help` 和命令明细
- `STRING/INT/UINT/BOOL/ENUM/REST` 参数解析
- 数值溢出、负数传给 `UINT`、非法布尔值、非法枚举值
- 非法参数、越界参数、未知命令
- telnet 行缓冲区上限与超长输入恢复
- `Tab` 命令补全、`help` 目标补全、参数级补全、参数提示
- `↑/↓` 历史命令
- server `is_running/active_clients` 状态接口
- 顺序循环压测
- 多客户端并发压测
- 多客户端并发异常流量压测（路径命令、非法参数、超长行混合）
- `max_clients` 连接上限与 `server busy` 行为

默认参数下会执行完整功能遍历，并跑一轮中等强度压力测试；命令行参数可进一步放大循环次数和并发数。

## 后续可扩展点

- 参数级自动补全，例如枚举值自动补齐
- 权限级别和登录认证
- 串口后端
- 更严格的 telnet 选项协商
