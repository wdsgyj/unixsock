#ifndef UNIXSOCK_CORE_H_
#define UNIXSOCK_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  UNIXSOCK_EVENT_CONNECTED = 1,
  UNIXSOCK_EVENT_DATA = 2,
  UNIXSOCK_EVENT_READ_CLOSED = 3,
  UNIXSOCK_EVENT_CLOSED = 4,
  UNIXSOCK_EVENT_ERROR = 5,
  UNIXSOCK_EVENT_DRAINED = 6,
  UNIXSOCK_EVENT_ACCEPTED = 7,
} unixsock_event_type_t;

typedef struct {
  int32_t handle;
  unixsock_event_type_t type;
  int32_t related_handle;
  const char* address;
  const char* remote_address;
  const uint8_t* data;
  size_t data_length;
  int32_t error_code;
  const char* message;
} unixsock_event_t;

typedef void (*unixsock_event_callback_t)(const unixsock_event_t* event,
                                          void* context);

int unixsock_core_initialize(void);
int32_t unixsock_core_connect(const char* path,
                              int32_t timeout_ms,
                              unixsock_event_callback_t callback,
                              void* context,
                              int32_t* error_code,
                              char** error_message);
int32_t unixsock_core_bind_server(const char* path,
                                  int32_t backlog,
                                  unixsock_event_callback_t callback,
                                  void* context,
                                  int32_t* error_code,
                                  char** error_message);
int32_t unixsock_core_write(int32_t handle,
                            const uint8_t* bytes,
                            size_t length,
                            int32_t* error_code,
                            char** error_message);
int32_t unixsock_core_shutdown_write(int32_t handle,
                                     int32_t* error_code,
                                     char** error_message);
int32_t unixsock_core_destroy(int32_t handle,
                              int32_t* error_code,
                              char** error_message);
int32_t unixsock_core_set_option(int32_t handle,
                                 int32_t option,
                                 bool enabled,
                                 int32_t* error_code,
                                 char** error_message);
int32_t unixsock_core_get_raw_option(int32_t handle,
                                     int32_t level,
                                     int32_t option,
                                     uint8_t* value,
                                     size_t value_length,
                                     int32_t* actual_length,
                                     int32_t* error_code,
                                     char** error_message);
int32_t unixsock_core_set_raw_option(int32_t handle,
                                     int32_t level,
                                     int32_t option,
                                     const uint8_t* value,
                                     size_t value_length,
                                     int32_t* error_code,
                                     char** error_message);
int64_t unixsock_core_pending_write_bytes(int32_t handle,
                                          int32_t* error_code,
                                          char** error_message);
bool unixsock_core_is_thread_running(void);
int32_t unixsock_core_active_socket_count(void);
void unixsock_core_shutdown_for_tests(void);
void unixsock_core_free_string(char* string);

#ifdef __cplusplus
}
#endif

#endif  // UNIXSOCK_CORE_H_
