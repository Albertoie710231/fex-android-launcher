/*
 * libpipewire-0.3 stub — provides all symbols SDL3 needs.
 * Audio is disabled (STEAM_DISABLE_AUDIO=1), so these are never called.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* Opaque types */
typedef struct pw_context pw_context;
typedef struct pw_core pw_core;
typedef struct pw_loop pw_loop;
typedef struct pw_properties pw_properties;
typedef struct pw_proxy pw_proxy;
typedef struct pw_stream pw_stream;
typedef struct pw_thread_loop pw_thread_loop;
typedef struct pw_node_info pw_node_info;
typedef struct spa_dict spa_dict;
typedef struct spa_hook spa_hook;

enum pw_stream_state { PW_STREAM_STATE_ERROR = -1 };

void pw_init(int *argc, char ***argv) {}
void pw_deinit(void) {}

pw_context *pw_context_new(pw_loop *loop, pw_properties *props, size_t user_data_size) { return NULL; }
void pw_context_destroy(pw_context *ctx) {}
pw_core *pw_context_connect(pw_context *ctx, pw_properties *props, size_t user_data_size) { return NULL; }
void pw_core_disconnect(pw_core *core) {}

pw_properties *pw_properties_new(const char *key, ...) { return NULL; }
pw_properties *pw_properties_new_dict(const spa_dict *dict) { return NULL; }
int pw_properties_set(pw_properties *props, const char *key, const char *value) { return 0; }
int pw_properties_setf(pw_properties *props, const char *key, const char *fmt, ...) { return 0; }

void pw_proxy_add_listener(pw_proxy *proxy, spa_hook *listener, const void *events, void *data) {}
void pw_proxy_add_object_listener(pw_proxy *proxy, spa_hook *listener, const void *events, void *data) {}
void pw_proxy_destroy(pw_proxy *proxy) {}
void *pw_proxy_get_user_data(pw_proxy *proxy) { return NULL; }

pw_stream *pw_stream_new(pw_core *core, const char *name, pw_properties *props) { return NULL; }
pw_stream *pw_stream_new_simple(pw_loop *loop, const char *name, pw_properties *props,
                                 const void *events, void *data) { return NULL; }
void pw_stream_destroy(pw_stream *stream) {}
int pw_stream_connect(pw_stream *stream, int direction, uint32_t target_id,
                       int flags, const void **params, uint32_t n_params) { return -1; }
enum pw_stream_state pw_stream_get_state(pw_stream *stream, const char **error) { return PW_STREAM_STATE_ERROR; }
void pw_stream_add_listener(pw_stream *stream, spa_hook *listener, const void *events, void *data) {}
void *pw_stream_dequeue_buffer(pw_stream *stream) { return NULL; }
int pw_stream_queue_buffer(pw_stream *stream, void *buffer) { return -1; }

pw_thread_loop *pw_thread_loop_new(const char *name, const void *props) { return NULL; }
void pw_thread_loop_destroy(pw_thread_loop *loop) {}
pw_loop *pw_thread_loop_get_loop(pw_thread_loop *loop) { return NULL; }
int pw_thread_loop_start(pw_thread_loop *loop) { return -1; }
void pw_thread_loop_stop(pw_thread_loop *loop) {}
void pw_thread_loop_lock(pw_thread_loop *loop) {}
void pw_thread_loop_unlock(pw_thread_loop *loop) {}
void pw_thread_loop_signal(pw_thread_loop *loop, int wait_for_accept) {}
void pw_thread_loop_wait(pw_thread_loop *loop) {}

pw_node_info *pw_node_info_merge(pw_node_info *info, const pw_node_info *update, int changed) { return NULL; }
void pw_node_info_free(pw_node_info *info) {}
pw_node_info *pw_node_info_update(pw_node_info *info, const pw_node_info *update) { return NULL; }

/* pw_port_info */
typedef struct pw_port_info pw_port_info;
pw_port_info *pw_port_info_update(pw_port_info *info, const pw_port_info *update) { return NULL; }
void pw_port_info_free(pw_port_info *info) {}

/* pw_device_info */
typedef struct pw_device_info pw_device_info;
pw_device_info *pw_device_info_update(pw_device_info *info, const pw_device_info *update) { return NULL; }
void pw_device_info_free(pw_device_info *info) {}

/* pw_link_info */
typedef struct pw_link_info pw_link_info;
pw_link_info *pw_link_info_update(pw_link_info *info, const pw_link_info *update) { return NULL; }
void pw_link_info_free(pw_link_info *info) {}

/* Additional symbols needed by steamui.so / steamclient.so */
pw_core *pw_context_connect_fd(pw_context *ctx, int fd, pw_properties *props, size_t user_data_size) { return NULL; }
const char *pw_stream_state_as_string(enum pw_stream_state state) { return "error"; }
int pw_stream_update_params(pw_stream *stream, const void **params, uint32_t n_params) { return -1; }
const char *pw_get_library_version(void) { return "0.3.0-stub"; }
