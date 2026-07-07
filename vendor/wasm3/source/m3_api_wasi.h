//
//  m3_api_wasi.h
//
//  Created by Volodymyr Shymanskyy on 11/20/19.
//  Copyright © 2019 Volodymyr Shymanskyy. All rights reserved.
//

#ifndef m3_api_wasi_h
#define m3_api_wasi_h

#include <stddef.h>
#include <stdint.h>

#include "m3_core.h"

d_m3BeginExternC

typedef uint16_t (*m3_wasi_socket_send_fn)(void *user_data,
                                           uint32_t handle,
                                           const uint8_t *data,
                                           uint32_t data_len,
                                           uint16_t si_flags,
                                           uint32_t *so_datalen);

typedef uint16_t (*m3_wasi_socket_recv_fn)(void *user_data,
                                           uint32_t handle,
                                           uint8_t *data,
                                           uint32_t data_len,
                                           uint16_t ri_flags,
                                           uint32_t *ro_datalen,
                                           uint16_t *ro_flags);

typedef uint16_t (*m3_wasi_socket_shutdown_fn)(void *user_data,
                                               uint32_t handle,
                                               uint8_t how);

typedef struct m3_wasi_preopen_t
{
    int                     fd;
    const char*             path;
    const char*             real_path;
} m3_wasi_preopen_t;

typedef struct m3_wasi_socket_ops_t
{
    m3_wasi_socket_send_fn         send;
    m3_wasi_socket_recv_fn         recv;
    m3_wasi_socket_shutdown_fn     shutdown;
} m3_wasi_socket_ops_t;

typedef struct m3_wasi_context_t
{
    i32                     exit_code;
    u32                     argc;
    ccstr_t *               argv;
    m3_wasi_preopen_t *     preopen;
    u32                     preopen_count;
    u32                     preopen_capacity;
    const m3_wasi_socket_ops_t *socket_ops;
    void *                  socket_user_data;
} m3_wasi_context_t;

static inline
void m3_wasi_context_set_socket_ops(m3_wasi_context_t *context,
                                    const m3_wasi_socket_ops_t *ops,
                                    void *user_data)
{
    if (!context)
        return;

    context->socket_ops = ops;
    context->socket_user_data = user_data;
}

static inline
const m3_wasi_socket_ops_t *m3_wasi_context_get_socket_ops(const m3_wasi_context_t *context,
                                                           void **user_data)
{
    if (!context) {
        if (user_data)
            *user_data = NULL;
        return NULL;
    }

    if (user_data)
        *user_data = context->socket_user_data;
    return context->socket_ops;
}

m3_wasi_context_t* m3_NewWasiContext(void);
void m3_FreeWasiContext(m3_wasi_context_t* context);
void m3_wasi_context_reset_preopens(m3_wasi_context_t* context);
int m3_wasi_context_set_preopen(m3_wasi_context_t* context,
                                uint32_t fd,
                                const char* guest_path,
                                const char* host_path);
int m3_wasi_context_remove_preopen(m3_wasi_context_t* context, uint32_t fd);

M3Result    m3_LinkWASI             (IM3Module io_module);
M3Result    m3_LinkWASIWithContext  (IM3Module io_module, m3_wasi_context_t* context);

m3_wasi_context_t* m3_GetWasiContext();
m3_wasi_context_t* m3_GetWasiContextForRuntime(IM3Runtime runtime);
m3_wasi_context_t* m3_GetWasiContextForModule(IM3Module module);

d_m3EndExternC

#endif // m3_api_wasi_h
