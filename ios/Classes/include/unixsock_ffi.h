#ifndef UNIXSOCK_FFI_H_
#define UNIXSOCK_FFI_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if __GNUC__ >= 4
#define UNIXSOCK_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#else
#define UNIXSOCK_EXPORT
#endif

UNIXSOCK_EXPORT intptr_t unixsock_initialize_dart_api(void* data);
UNIXSOCK_EXPORT int32_t unixsock_connect(const char* path,
                                         int64_t dart_port,
                                         int32_t timeout_ms,
                                         int32_t* error_code,
                                         char** error_message);
UNIXSOCK_EXPORT int32_t unixsock_bind_server(const char* path,
                                             int64_t dart_port,
                                             int32_t backlog,
                                             int32_t* error_code,
                                             char** error_message);
UNIXSOCK_EXPORT int32_t unixsock_write(int32_t handle,
                                       const uint8_t* bytes,
                                       int32_t length,
                                       int32_t* error_code,
                                       char** error_message);
UNIXSOCK_EXPORT int32_t unixsock_shutdown_write(int32_t handle,
                                                int32_t* error_code,
                                                char** error_message);
UNIXSOCK_EXPORT int32_t unixsock_destroy(int32_t handle,
                                         int32_t* error_code,
                                         char** error_message);
UNIXSOCK_EXPORT int32_t unixsock_set_option(int32_t handle,
                                            int32_t option,
                                            uint8_t enabled,
                                            int32_t* error_code,
                                            char** error_message);
UNIXSOCK_EXPORT int32_t unixsock_get_raw_option(int32_t handle,
                                                int32_t level,
                                                int32_t option,
                                                uint8_t* value,
                                                int32_t value_length,
                                                int32_t* actual_length,
                                                int32_t* error_code,
                                                char** error_message);
UNIXSOCK_EXPORT int32_t unixsock_set_raw_option(int32_t handle,
                                                int32_t level,
                                                int32_t option,
                                                const uint8_t* value,
                                                int32_t value_length,
                                                int32_t* error_code,
                                                char** error_message);
UNIXSOCK_EXPORT int64_t unixsock_pending_write_bytes(int32_t handle,
                                                     int32_t* error_code,
                                                     char** error_message);
UNIXSOCK_EXPORT void unixsock_free_string(char* string);

#ifdef __cplusplus
}
#endif

#endif  // UNIXSOCK_FFI_H_
