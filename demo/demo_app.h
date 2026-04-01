#ifndef EMBCLI_DEMO_APP_H
#define EMBCLI_DEMO_APP_H

#include "embcli/embcli.h"
#include "embcli/embcli_telnet.h"

#ifdef __cplusplus
extern "C" {
#endif

void build_demo_cli(embcli_t *cli, embcli_telnet_server_t *server);

#ifdef __cplusplus
}
#endif

#endif
