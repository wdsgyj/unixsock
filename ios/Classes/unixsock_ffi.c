#include "include/unixsock_ffi.h"

#include "include/unixsock_dart_api.h"
#include "internal/unixsock_core.h"

#include <stdbool.h>
#include <stdint.h>

static bool unixsock_post_simple_event(Dart_Port_DL port,
                                       int32_t type,
                                       int32_t handle) {
  Dart_CObject type_object;
  type_object.type = Dart_CObject_kInt32;
  type_object.value.as_int32 = type;

  Dart_CObject handle_object;
  handle_object.type = Dart_CObject_kInt32;
  handle_object.value.as_int32 = handle;

  Dart_CObject* values[] = {&type_object, &handle_object};

  Dart_CObject message;
  message.type = Dart_CObject_kArray;
  message.value.as_array.length = 2;
  message.value.as_array.values = values;

  return Dart_PostCObject_DL != NULL && Dart_PostCObject_DL(port, &message);
}

static bool unixsock_post_data_event(Dart_Port_DL port,
                                     int32_t handle,
                                     const uint8_t* data,
                                     size_t length) {
  Dart_CObject type_object;
  type_object.type = Dart_CObject_kInt32;
  type_object.value.as_int32 = UNIXSOCK_EVENT_DATA;

  Dart_CObject handle_object;
  handle_object.type = Dart_CObject_kInt32;
  handle_object.value.as_int32 = handle;

  Dart_CObject data_object;
  data_object.type = Dart_CObject_kTypedData;
  data_object.value.as_typed_data.type = Dart_TypedData_kUint8;
  data_object.value.as_typed_data.length = (intptr_t)length;
  data_object.value.as_typed_data.values = data;

  Dart_CObject* values[] = {&type_object, &handle_object, &data_object};

  Dart_CObject message;
  message.type = Dart_CObject_kArray;
  message.value.as_array.length = 3;
  message.value.as_array.values = values;

  return Dart_PostCObject_DL != NULL && Dart_PostCObject_DL(port, &message);
}

static bool unixsock_post_error_event(Dart_Port_DL port,
                                      int32_t handle,
                                      int32_t code,
                                      const char* text) {
  Dart_CObject type_object;
  type_object.type = Dart_CObject_kInt32;
  type_object.value.as_int32 = UNIXSOCK_EVENT_ERROR;

  Dart_CObject handle_object;
  handle_object.type = Dart_CObject_kInt32;
  handle_object.value.as_int32 = handle;

  Dart_CObject code_object;
  code_object.type = Dart_CObject_kInt32;
  code_object.value.as_int32 = code;

  Dart_CObject message_object;
  message_object.type = Dart_CObject_kString;
  message_object.value.as_string = text;

  Dart_CObject* values[] = {
      &type_object,
      &handle_object,
      &code_object,
      &message_object,
  };

  Dart_CObject message;
  message.type = Dart_CObject_kArray;
  message.value.as_array.length = 4;
  message.value.as_array.values = values;

  return Dart_PostCObject_DL != NULL && Dart_PostCObject_DL(port, &message);
}

static bool unixsock_post_accepted_event(Dart_Port_DL port,
                                         int32_t server_handle,
                                         int32_t client_handle,
                                         const char* address,
                                         const char* remote_address) {
  Dart_CObject type_object;
  type_object.type = Dart_CObject_kInt32;
  type_object.value.as_int32 = UNIXSOCK_EVENT_ACCEPTED;

  Dart_CObject server_handle_object;
  server_handle_object.type = Dart_CObject_kInt32;
  server_handle_object.value.as_int32 = server_handle;

  Dart_CObject client_handle_object;
  client_handle_object.type = Dart_CObject_kInt32;
  client_handle_object.value.as_int32 = client_handle;

  Dart_CObject address_object;
  address_object.type = Dart_CObject_kString;
  address_object.value.as_string = address == NULL ? "" : address;

  Dart_CObject remote_address_object;
  remote_address_object.type = Dart_CObject_kString;
  remote_address_object.value.as_string =
      remote_address == NULL ? "" : remote_address;

  Dart_CObject* values[] = {
      &type_object,
      &server_handle_object,
      &client_handle_object,
      &address_object,
      &remote_address_object,
  };

  Dart_CObject message;
  message.type = Dart_CObject_kArray;
  message.value.as_array.length = 5;
  message.value.as_array.values = values;

  return Dart_PostCObject_DL != NULL && Dart_PostCObject_DL(port, &message);
}

static void unixsock_core_event_bridge(const unixsock_event_t* event, void* context) {
  const Dart_Port_DL port = (Dart_Port_DL)(intptr_t)context;
  if (event == NULL || Dart_PostCObject_DL == NULL || port == 0) {
    return;
  }

  switch (event->type) {
    case UNIXSOCK_EVENT_CONNECTED:
      (void)unixsock_post_simple_event(port, UNIXSOCK_EVENT_CONNECTED, event->handle);
      break;
    case UNIXSOCK_EVENT_DATA:
      (void)unixsock_post_data_event(port, event->handle, event->data, event->data_length);
      break;
    case UNIXSOCK_EVENT_READ_CLOSED:
      (void)unixsock_post_simple_event(port, UNIXSOCK_EVENT_READ_CLOSED, event->handle);
      break;
    case UNIXSOCK_EVENT_CLOSED:
      (void)unixsock_post_simple_event(port, UNIXSOCK_EVENT_CLOSED, event->handle);
      break;
    case UNIXSOCK_EVENT_ERROR:
      (void)unixsock_post_error_event(
          port,
          event->handle,
          event->error_code,
          event->message == NULL ? "native socket error" : event->message);
      break;
    case UNIXSOCK_EVENT_DRAINED:
      (void)unixsock_post_simple_event(port, UNIXSOCK_EVENT_DRAINED, event->handle);
      break;
    case UNIXSOCK_EVENT_ACCEPTED:
      (void)unixsock_post_accepted_event(
          port,
          event->handle,
          event->related_handle,
          event->address,
          event->remote_address);
      break;
  }
}

intptr_t unixsock_initialize_dart_api(void* data) {
  return Dart_InitializeApiDL(data);
}

int32_t unixsock_connect(const char* path,
                         int64_t dart_port,
                         int32_t timeout_ms,
                         int32_t* error_code,
                         char** error_message) {
  return unixsock_core_connect(path,
                               timeout_ms,
                               unixsock_core_event_bridge,
                               (void*)(intptr_t)dart_port,
                               error_code,
                               error_message);
}

int32_t unixsock_bind_server(const char* path,
                             int64_t dart_port,
                             int32_t backlog,
                             int32_t* error_code,
                             char** error_message) {
  return unixsock_core_bind_server(path,
                                   backlog,
                                   unixsock_core_event_bridge,
                                   (void*)(intptr_t)dart_port,
                                   error_code,
                                   error_message);
}

int32_t unixsock_write(int32_t handle,
                       const uint8_t* bytes,
                       int32_t length,
                       int32_t* error_code,
                       char** error_message) {
  return unixsock_core_write(handle, bytes, (size_t)length, error_code, error_message);
}

int32_t unixsock_shutdown_write(int32_t handle,
                                int32_t* error_code,
                                char** error_message) {
  return unixsock_core_shutdown_write(handle, error_code, error_message);
}

int32_t unixsock_destroy(int32_t handle,
                         int32_t* error_code,
                         char** error_message) {
  return unixsock_core_destroy(handle, error_code, error_message);
}

int32_t unixsock_set_option(int32_t handle,
                            int32_t option,
                            uint8_t enabled,
                            int32_t* error_code,
                            char** error_message) {
  return unixsock_core_set_option(handle, option, enabled != 0, error_code, error_message);
}

int32_t unixsock_get_raw_option(int32_t handle,
                                int32_t level,
                                int32_t option,
                                uint8_t* value,
                                int32_t value_length,
                                int32_t* actual_length,
                                int32_t* error_code,
                                char** error_message) {
  return unixsock_core_get_raw_option(handle,
                                      level,
                                      option,
                                      value,
                                      (size_t)value_length,
                                      actual_length,
                                      error_code,
                                      error_message);
}

int32_t unixsock_set_raw_option(int32_t handle,
                                int32_t level,
                                int32_t option,
                                const uint8_t* value,
                                int32_t value_length,
                                int32_t* error_code,
                                char** error_message) {
  return unixsock_core_set_raw_option(handle,
                                      level,
                                      option,
                                      value,
                                      (size_t)value_length,
                                      error_code,
                                      error_message);
}

int64_t unixsock_pending_write_bytes(int32_t handle,
                                     int32_t* error_code,
                                     char** error_message) {
  return unixsock_core_pending_write_bytes(handle, error_code, error_message);
}

void unixsock_free_string(char* string) {
  unixsock_core_free_string(string);
}
