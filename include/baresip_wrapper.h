#ifndef BARESIP_WRAPPER_H
#define BARESIP_WRAPPER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bsw_wrapper bsw_wrapper_t;
typedef void (*bsw_event_cb)(
    int32_t event,
    uintptr_t ua_ptr,
    uintptr_t call_ptr,
    const char *text,
    void *user
);

int32_t bsw_wrapper_create(bsw_wrapper_t **out_handle, const char *lib_path);
void bsw_wrapper_destroy(bsw_wrapper_t *handle);

int32_t bsw_init(
    bsw_wrapper_t *handle,
    const char *conf_path,
    const char *software,
    int32_t enable_udp,
    int32_t enable_tcp,
    int32_t enable_tls
);

int32_t bsw_load_module(bsw_wrapper_t *handle, const char *module_path, const char *module_name);

int32_t bsw_run_async(bsw_wrapper_t *handle);
int32_t bsw_stop(bsw_wrapper_t *handle);
int32_t bsw_is_running(bsw_wrapper_t *handle);
int32_t bsw_last_loop_error(bsw_wrapper_t *handle);

int32_t bsw_set_event_callback(bsw_wrapper_t *handle, bsw_event_cb cb, void *user);

int32_t bsw_add_ua(
    bsw_wrapper_t *handle,
    const char *aor,
    int32_t do_register,
    uintptr_t *out_ua
);

int32_t bsw_connect_call(
    bsw_wrapper_t *handle,
    uintptr_t ua_ptr,
    const char *from_uri,
    const char *req_uri,
    uintptr_t *out_call
);

int32_t bsw_answer_call(
    bsw_wrapper_t *handle,
    uintptr_t ua_ptr,
    uintptr_t call_ptr
);

int32_t bsw_hangup_call(
    bsw_wrapper_t *handle,
    uintptr_t ua_ptr,
    uintptr_t call_ptr,
    uint16_t status_code,
    const char *reason
);

const char *bsw_version(bsw_wrapper_t *handle);
const char *bsw_last_error(bsw_wrapper_t *handle);

#ifdef __cplusplus
}
#endif

#endif
