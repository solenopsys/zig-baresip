#include "baresip_wrapper.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re.h"
#include "baresip.h"

#ifndef BSW_OK
#define BSW_OK 0
#endif

#ifndef BSW_INVALID
#define BSW_INVALID -1
#endif

#ifndef BSW_FAILURE
#define BSW_FAILURE -2
#endif

#ifndef BSW_NOT_AVAIL
#define BSW_NOT_AVAIL -3
#endif

struct bsw_wrapper {
    void *lib;
    pthread_mutex_t mu;
    pthread_t loop_thread;
    int loop_running;
    int loop_joinable;
    int initialized;
    int last_loop_err;
    int event_registered;
    char last_error[512];

    bsw_event_cb on_event;
    void *on_event_user;

    int (*libre_init_fn)(void);
    void (*libre_close_fn)(void);
    int (*re_main_fn)(re_signal_h *signalh);
    void (*re_cancel_fn)(void);

    int (*conf_path_set_fn)(const char *path);
    int (*conf_configure_fn)(void);
    struct config *(*conf_config_fn)(void);

    int (*baresip_init_fn)(struct config *cfg);
    void (*baresip_close_fn)(void);
    const char *(*baresip_version_fn)(void);

    int (*ua_init_fn)(const char *software, bool udp, bool tcp, bool tls);
    void (*ua_stop_all_fn)(bool forced);
    int (*ua_alloc_fn)(struct ua **uap, const char *aor);
    int (*ua_register_fn)(struct ua *ua);
    int (*ua_connect_fn)(
        struct ua *ua,
        struct call **callp,
        const char *from_uri,
        const char *req_uri,
        enum vidmode vmode
    );
    int (*ua_answer_fn)(struct ua *ua, struct call *call, enum vidmode vmode);
    void (*ua_hangup_fn)(struct ua *ua, struct call *call, uint16_t scode, const char *reason);

    int (*module_load_fn)(const char *path, const char *name);

    int (*bevent_register_fn)(bevent_h *eh, void *arg);
    void (*bevent_unregister_fn)(bevent_h *eh);
    struct call *(*bevent_get_call_fn)(const struct bevent *event);
    struct ua *(*bevent_get_ua_fn)(const struct bevent *event);
    const char *(*bevent_get_text_fn)(const struct bevent *event);
};

static void bsw_bevent_bridge(enum bevent_ev ev, struct bevent *event, void *arg);

static void bsw_set_last_error(bsw_wrapper_t *h, const char *msg) {
    if (!h) return;
    if (!msg) {
        h->last_error[0] = '\0';
        return;
    }
    snprintf(h->last_error, sizeof(h->last_error), "%s", msg);
}

static void *bsw_load_symbol(bsw_wrapper_t *h, const char *name, int required) {
    dlerror();
    void *sym = dlsym(h->lib, name);
    const char *err = dlerror();

    if (err != NULL) {
        if (required) {
            char buf[512];
            snprintf(buf, sizeof(buf), "missing symbol %s: %s", name, err);
            bsw_set_last_error(h, buf);
        }
        return NULL;
    }

    return sym;
}

static void bsw_unregister_events(bsw_wrapper_t *handle) {
    if (!handle->event_registered) return;
    if (!handle->bevent_unregister_fn) return;

    handle->bevent_unregister_fn(bsw_bevent_bridge);
    handle->event_registered = 0;
}

static int bsw_register_events(bsw_wrapper_t *handle) {
    if (!handle->on_event) return BSW_OK;
    if (!handle->bevent_register_fn || !handle->bevent_unregister_fn) return BSW_NOT_AVAIL;
    if (handle->event_registered) return BSW_OK;

    const int err = handle->bevent_register_fn(bsw_bevent_bridge, handle);
    if (err) {
        bsw_set_last_error(handle, "bevent_register failed");
        return err;
    }

    handle->event_registered = 1;
    return BSW_OK;
}

static void bsw_bevent_bridge(enum bevent_ev ev, struct bevent *event, void *arg) {
    bsw_wrapper_t *h = (bsw_wrapper_t *)arg;
    if (!h) return;

    pthread_mutex_lock(&h->mu);
    bsw_event_cb cb = h->on_event;
    void *user = h->on_event_user;
    pthread_mutex_unlock(&h->mu);

    if (!cb) return;

    uintptr_t ua_ptr = 0;
    uintptr_t call_ptr = 0;
    const char *text = NULL;

    if (event) {
        if (h->bevent_get_ua_fn) {
            ua_ptr = (uintptr_t)h->bevent_get_ua_fn(event);
        }
        if (h->bevent_get_call_fn) {
            call_ptr = (uintptr_t)h->bevent_get_call_fn(event);
        }
        if (h->bevent_get_text_fn) {
            text = h->bevent_get_text_fn(event);
        }
    }

    cb((int32_t)ev, ua_ptr, call_ptr, text, user);
}

static void *bsw_loop_thread_main(void *arg) {
    bsw_wrapper_t *h = (bsw_wrapper_t *)arg;
    const int err = h->re_main_fn(NULL);

    pthread_mutex_lock(&h->mu);
    h->last_loop_err = err;
    h->loop_running = 0;
    pthread_mutex_unlock(&h->mu);

    return NULL;
}

int32_t bsw_wrapper_create(bsw_wrapper_t **out_handle, const char *lib_path) {
    if (!out_handle) return BSW_INVALID;
    *out_handle = NULL;

    bsw_wrapper_t *h = (bsw_wrapper_t *)calloc(1, sizeof(*h));
    if (!h) return BSW_FAILURE;

    pthread_mutex_init(&h->mu, NULL);

    const char *path = (lib_path && lib_path[0] != '\0') ? lib_path : "libbaresip.so";
    h->lib = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!h->lib) {
        bsw_set_last_error(h, dlerror());
        pthread_mutex_destroy(&h->mu);
        free(h);
        return BSW_FAILURE;
    }

#define BSW_LOAD_REQUIRED(name, field)                                                                  \
    do {                                                                                                 \
        h->field = bsw_load_symbol(h, name, 1);                                                         \
        if (!h->field) {                                                                                 \
            dlclose(h->lib);                                                                             \
            pthread_mutex_destroy(&h->mu);                                                               \
            free(h);                                                                                     \
            return BSW_FAILURE;                                                                          \
        }                                                                                                \
    } while (0)

#define BSW_LOAD_OPTIONAL(name, field)                                                                   \
    do {                                                                                                 \
        h->field = bsw_load_symbol(h, name, 0);                                                         \
    } while (0)

    BSW_LOAD_REQUIRED("libre_init", libre_init_fn);
    BSW_LOAD_REQUIRED("libre_close", libre_close_fn);
    BSW_LOAD_REQUIRED("re_main", re_main_fn);
    BSW_LOAD_REQUIRED("re_cancel", re_cancel_fn);

    BSW_LOAD_REQUIRED("conf_path_set", conf_path_set_fn);
    BSW_LOAD_REQUIRED("conf_configure", conf_configure_fn);
    BSW_LOAD_REQUIRED("conf_config", conf_config_fn);

    BSW_LOAD_REQUIRED("baresip_init", baresip_init_fn);
    BSW_LOAD_REQUIRED("baresip_close", baresip_close_fn);
    BSW_LOAD_OPTIONAL("baresip_version", baresip_version_fn);

    BSW_LOAD_REQUIRED("ua_init", ua_init_fn);
    BSW_LOAD_REQUIRED("ua_stop_all", ua_stop_all_fn);
    BSW_LOAD_REQUIRED("ua_alloc", ua_alloc_fn);
    BSW_LOAD_REQUIRED("ua_register", ua_register_fn);
    BSW_LOAD_REQUIRED("ua_connect", ua_connect_fn);
    BSW_LOAD_REQUIRED("ua_answer", ua_answer_fn);
    BSW_LOAD_REQUIRED("ua_hangup", ua_hangup_fn);

    BSW_LOAD_OPTIONAL("module_load", module_load_fn);

    BSW_LOAD_OPTIONAL("bevent_register", bevent_register_fn);
    BSW_LOAD_OPTIONAL("bevent_unregister", bevent_unregister_fn);
    BSW_LOAD_OPTIONAL("bevent_get_call", bevent_get_call_fn);
    BSW_LOAD_OPTIONAL("bevent_get_ua", bevent_get_ua_fn);
    BSW_LOAD_OPTIONAL("bevent_get_text", bevent_get_text_fn);

#undef BSW_LOAD_REQUIRED
#undef BSW_LOAD_OPTIONAL

    bsw_set_last_error(h, "");
    *out_handle = h;
    return BSW_OK;
}

int32_t bsw_stop(bsw_wrapper_t *handle) {
    if (!handle) return BSW_INVALID;

    pthread_mutex_lock(&handle->mu);
    const int joinable = handle->loop_joinable;
    const int running = handle->loop_running;
    const int was_initialized = handle->initialized;
    pthread_mutex_unlock(&handle->mu);

    if (joinable) {
        if (running) {
            handle->re_cancel_fn();
        }
        pthread_join(handle->loop_thread, NULL);

        pthread_mutex_lock(&handle->mu);
        handle->loop_joinable = 0;
        handle->loop_running = 0;
        pthread_mutex_unlock(&handle->mu);
    }

    pthread_mutex_lock(&handle->mu);
    bsw_unregister_events(handle);
    handle->initialized = 0;
    pthread_mutex_unlock(&handle->mu);

    if (was_initialized) {
        handle->ua_stop_all_fn(true);
        handle->baresip_close_fn();
        handle->libre_close_fn();
    }

    return BSW_OK;
}

void bsw_wrapper_destroy(bsw_wrapper_t *handle) {
    if (!handle) return;
    (void)bsw_stop(handle);

    if (handle->lib) dlclose(handle->lib);
    pthread_mutex_destroy(&handle->mu);
    free(handle);
}

int32_t bsw_init(
    bsw_wrapper_t *handle,
    const char *conf_path,
    const char *software,
    int32_t enable_udp,
    int32_t enable_tcp,
    int32_t enable_tls
) {
    if (!handle) return BSW_INVALID;

    pthread_mutex_lock(&handle->mu);
    if (handle->initialized) {
        pthread_mutex_unlock(&handle->mu);
        return BSW_OK;
    }
    pthread_mutex_unlock(&handle->mu);

    int err = handle->libre_init_fn();
    if (err) {
        bsw_set_last_error(handle, "libre_init failed");
        return err;
    }

    if (conf_path && conf_path[0] != '\0') {
        err = handle->conf_path_set_fn(conf_path);
        if (err) {
            bsw_set_last_error(handle, "conf_path_set failed");
            handle->libre_close_fn();
            return err;
        }
    }

    err = handle->conf_configure_fn();
    if (err) {
        bsw_set_last_error(handle, "conf_configure failed");
        handle->libre_close_fn();
        return err;
    }

    struct config *cfg = handle->conf_config_fn();
    if (!cfg) {
        bsw_set_last_error(handle, "conf_config returned null");
        handle->libre_close_fn();
        return BSW_FAILURE;
    }

    err = handle->baresip_init_fn(cfg);
    if (err) {
        bsw_set_last_error(handle, "baresip_init failed");
        handle->libre_close_fn();
        return err;
    }

    const char *sw = (software && software[0] != '\0') ? software : "llm-audio-gate";
    err = handle->ua_init_fn(sw, enable_udp != 0, enable_tcp != 0, enable_tls != 0);
    if (err) {
        bsw_set_last_error(handle, "ua_init failed");
        handle->baresip_close_fn();
        handle->libre_close_fn();
        return err;
    }

    pthread_mutex_lock(&handle->mu);
    handle->initialized = 1;
    handle->last_loop_err = 0;
    pthread_mutex_unlock(&handle->mu);

    err = bsw_register_events(handle);
    if (err) {
        (void)bsw_stop(handle);
        return err;
    }

    bsw_set_last_error(handle, "");
    return BSW_OK;
}

int32_t bsw_load_module(bsw_wrapper_t *handle, const char *module_path, const char *module_name) {
    if (!handle || !module_name || module_name[0] == '\0') return BSW_INVALID;
    if (!handle->module_load_fn) return BSW_NOT_AVAIL;
    return handle->module_load_fn(module_path, module_name);
}

int32_t bsw_run_async(bsw_wrapper_t *handle) {
    if (!handle) return BSW_INVALID;

    pthread_mutex_lock(&handle->mu);
    if (!handle->initialized) {
        pthread_mutex_unlock(&handle->mu);
        return BSW_INVALID;
    }
    if (handle->loop_joinable) {
        pthread_mutex_unlock(&handle->mu);
        return BSW_OK;
    }

    handle->loop_running = 1;
    handle->loop_joinable = 1;
    handle->last_loop_err = 0;
    if (pthread_create(&handle->loop_thread, NULL, bsw_loop_thread_main, handle) != 0) {
        handle->loop_running = 0;
        handle->loop_joinable = 0;
        pthread_mutex_unlock(&handle->mu);
        bsw_set_last_error(handle, "pthread_create failed");
        return BSW_FAILURE;
    }

    pthread_mutex_unlock(&handle->mu);
    return BSW_OK;
}

int32_t bsw_is_running(bsw_wrapper_t *handle) {
    if (!handle) return BSW_INVALID;
    pthread_mutex_lock(&handle->mu);
    const int running = handle->loop_running;
    pthread_mutex_unlock(&handle->mu);
    return running ? 1 : 0;
}

int32_t bsw_last_loop_error(bsw_wrapper_t *handle) {
    if (!handle) return BSW_INVALID;
    pthread_mutex_lock(&handle->mu);
    const int loop_err = handle->last_loop_err;
    pthread_mutex_unlock(&handle->mu);
    return loop_err;
}

int32_t bsw_set_event_callback(bsw_wrapper_t *handle, bsw_event_cb cb, void *user) {
    if (!handle) return BSW_INVALID;

    pthread_mutex_lock(&handle->mu);
    handle->on_event = cb;
    handle->on_event_user = user;
    const int initialized = handle->initialized;
    const int registered = handle->event_registered;
    pthread_mutex_unlock(&handle->mu);

    if (!cb) {
        if (registered) {
            pthread_mutex_lock(&handle->mu);
            bsw_unregister_events(handle);
            pthread_mutex_unlock(&handle->mu);
        }
        return BSW_OK;
    }

    if (!handle->bevent_register_fn || !handle->bevent_unregister_fn) {
        bsw_set_last_error(handle, "bevent API not available");
        return BSW_NOT_AVAIL;
    }

    if (initialized && !registered) {
        pthread_mutex_lock(&handle->mu);
        const int err = bsw_register_events(handle);
        pthread_mutex_unlock(&handle->mu);
        if (err) return err;
    }

    return BSW_OK;
}

int32_t bsw_add_ua(
    bsw_wrapper_t *handle,
    const char *aor,
    int32_t do_register,
    uintptr_t *out_ua
) {
    if (!handle || !aor || aor[0] == '\0' || !out_ua) return BSW_INVALID;

    struct ua *ua = NULL;
    int err = handle->ua_alloc_fn(&ua, aor);
    if (err) {
        bsw_set_last_error(handle, "ua_alloc failed");
        return err;
    }

    if (do_register != 0) {
        err = handle->ua_register_fn(ua);
        if (err) {
            bsw_set_last_error(handle, "ua_register failed");
            return err;
        }
    }

    *out_ua = (uintptr_t)ua;
    return BSW_OK;
}

int32_t bsw_connect_call(
    bsw_wrapper_t *handle,
    uintptr_t ua_ptr,
    const char *from_uri,
    const char *req_uri,
    uintptr_t *out_call
) {
    if (!handle || !ua_ptr || !req_uri || req_uri[0] == '\0' || !out_call) return BSW_INVALID;

    struct ua *ua = (struct ua *)ua_ptr;
    struct call *call = NULL;

    int err = handle->ua_connect_fn(
        ua,
        &call,
        (from_uri && from_uri[0] != '\0') ? from_uri : NULL,
        req_uri,
        VIDMODE_OFF
    );
    if (err) {
        bsw_set_last_error(handle, "ua_connect failed");
        return err;
    }

    *out_call = (uintptr_t)call;
    return BSW_OK;
}

int32_t bsw_answer_call(
    bsw_wrapper_t *handle,
    uintptr_t ua_ptr,
    uintptr_t call_ptr
) {
    if (!handle || !ua_ptr || !call_ptr) return BSW_INVALID;
    if (!handle->ua_answer_fn) return BSW_NOT_AVAIL;

    const int err = handle->ua_answer_fn((struct ua *)ua_ptr, (struct call *)call_ptr, VIDMODE_OFF);
    if (err) {
        bsw_set_last_error(handle, "ua_answer failed");
        return err;
    }

    return BSW_OK;
}

int32_t bsw_hangup_call(
    bsw_wrapper_t *handle,
    uintptr_t ua_ptr,
    uintptr_t call_ptr,
    uint16_t status_code,
    const char *reason
) {
    if (!handle || !ua_ptr || !call_ptr) return BSW_INVALID;

    struct ua *ua = (struct ua *)ua_ptr;
    struct call *call = (struct call *)call_ptr;
    const uint16_t code = status_code == 0 ? 0 : status_code;
    const char *text = (reason && reason[0] != '\0') ? reason : "terminated";

    handle->ua_hangup_fn(ua, call, code, text);
    return BSW_OK;
}

const char *bsw_version(bsw_wrapper_t *handle) {
    if (!handle || !handle->baresip_version_fn) return "unknown";
    return handle->baresip_version_fn();
}

const char *bsw_last_error(bsw_wrapper_t *handle) {
    if (!handle) return "invalid handle";
    return handle->last_error;
}
