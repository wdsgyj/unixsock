#include "../Classes/internal/unixsock_core.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct {
  pthread_mutex_t mutex;
  bool connected;
  bool read_closed;
  bool closed;
  bool received_pong;
  int error_code;
  char error_text[256];
} client_observer_t;

typedef struct {
  pthread_mutex_t mutex;
  int32_t server_handle;
  int32_t accepted_handle;
  bool received_ping;
  bool server_closed;
  bool client_closed;
  int error_code;
  char error_text[256];
} server_observer_t;

typedef struct {
  char path[104];
  bool received_pong;
} plain_client_context_t;

static void client_observer_init(client_observer_t* observer) {
  memset(observer, 0, sizeof(*observer));
  pthread_mutex_init(&observer->mutex, NULL);
}

static void server_observer_init(server_observer_t* observer) {
  memset(observer, 0, sizeof(*observer));
  pthread_mutex_init(&observer->mutex, NULL);
}

static void client_observer_destroy(client_observer_t* observer) {
  pthread_mutex_destroy(&observer->mutex);
}

static void server_observer_destroy(server_observer_t* observer) {
  pthread_mutex_destroy(&observer->mutex);
}

static void client_observer_callback(const unixsock_event_t* event, void* context) {
  client_observer_t* observer = (client_observer_t*)context;
  pthread_mutex_lock(&observer->mutex);
  switch (event->type) {
    case UNIXSOCK_EVENT_CONNECTED:
      observer->connected = true;
      break;
    case UNIXSOCK_EVENT_DATA:
      if (event->data_length == 4 && memcmp(event->data, "pong", 4) == 0) {
        observer->received_pong = true;
      }
      break;
    case UNIXSOCK_EVENT_READ_CLOSED:
      observer->read_closed = true;
      break;
    case UNIXSOCK_EVENT_CLOSED:
      observer->closed = true;
      break;
    case UNIXSOCK_EVENT_ERROR:
      observer->error_code = event->error_code;
      snprintf(observer->error_text,
               sizeof(observer->error_text),
               "%s",
               event->message == NULL ? "native error" : event->message);
      break;
    case UNIXSOCK_EVENT_DRAINED:
    case UNIXSOCK_EVENT_ACCEPTED:
      break;
  }
  pthread_mutex_unlock(&observer->mutex);
}

static void server_observer_callback(const unixsock_event_t* event, void* context) {
  server_observer_t* observer = (server_observer_t*)context;
  pthread_mutex_lock(&observer->mutex);
  switch (event->type) {
    case UNIXSOCK_EVENT_ACCEPTED:
      observer->accepted_handle = event->related_handle;
      break;
    case UNIXSOCK_EVENT_DATA:
      if (event->handle == observer->accepted_handle &&
          event->data_length == 4 &&
          memcmp(event->data, "ping", 4) == 0) {
        observer->received_ping = true;
      }
      break;
    case UNIXSOCK_EVENT_CLOSED:
      if (event->handle == observer->server_handle) {
        observer->server_closed = true;
      }
      if (event->handle == observer->accepted_handle) {
        observer->client_closed = true;
      }
      break;
    case UNIXSOCK_EVENT_ERROR:
      observer->error_code = event->error_code;
      snprintf(observer->error_text,
               sizeof(observer->error_text),
               "%s",
               event->message == NULL ? "native error" : event->message);
      break;
    case UNIXSOCK_EVENT_CONNECTED:
    case UNIXSOCK_EVENT_READ_CLOSED:
    case UNIXSOCK_EVENT_DRAINED:
      break;
  }
  pthread_mutex_unlock(&observer->mutex);
}

static bool wait_for_condition(bool (*predicate)(void*), void* context, int attempts) {
  for (int index = 0; index < attempts; index += 1) {
    if (predicate(context)) {
      return true;
    }
    usleep(20 * 1000);
  }
  return false;
}

static bool client_connected(void* context) {
  client_observer_t* observer = (client_observer_t*)context;
  pthread_mutex_lock(&observer->mutex);
  const bool result = observer->connected;
  pthread_mutex_unlock(&observer->mutex);
  return result;
}

static bool client_received_pong(void* context) {
  client_observer_t* observer = (client_observer_t*)context;
  pthread_mutex_lock(&observer->mutex);
  const bool result = observer->received_pong;
  pthread_mutex_unlock(&observer->mutex);
  return result;
}

static bool client_closed(void* context) {
  client_observer_t* observer = (client_observer_t*)context;
  pthread_mutex_lock(&observer->mutex);
  const bool result = observer->closed && observer->read_closed;
  pthread_mutex_unlock(&observer->mutex);
  return result;
}

static bool server_accepted_client(void* context) {
  server_observer_t* observer = (server_observer_t*)context;
  pthread_mutex_lock(&observer->mutex);
  const bool result = observer->accepted_handle > 0;
  pthread_mutex_unlock(&observer->mutex);
  return result;
}

static bool server_received_ping(void* context) {
  server_observer_t* observer = (server_observer_t*)context;
  pthread_mutex_lock(&observer->mutex);
  const bool result = observer->received_ping;
  pthread_mutex_unlock(&observer->mutex);
  return result;
}

static bool server_and_client_closed(void* context) {
  server_observer_t* observer = (server_observer_t*)context;
  pthread_mutex_lock(&observer->mutex);
  const bool result = observer->server_closed && observer->client_closed;
  pthread_mutex_unlock(&observer->mutex);
  return result;
}

static bool thread_stopped(void* context) {
  (void)context;
  return !unixsock_core_is_thread_running();
}

static void make_sockaddr(const char* path,
                          struct sockaddr_un* address,
                          socklen_t* address_length) {
  memset(address, 0, sizeof(*address));
  address->sun_family = AF_UNIX;
  strncpy(address->sun_path, path, sizeof(address->sun_path) - 1);
  *address_length = (socklen_t)sizeof(*address);
#ifdef __APPLE__
  address->sun_len = (uint8_t)(*address_length);
#endif
}

static void* plain_server_thread_main(void* context) {
  const char* path = (const char*)context;

  const int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  assert(listen_fd >= 0);
  unlink(path);

  struct sockaddr_un address;
  socklen_t address_length = 0;
  make_sockaddr(path, &address, &address_length);

  assert(bind(listen_fd, (struct sockaddr*)&address, address_length) == 0);
  assert(listen(listen_fd, 1) == 0);

  const int client_fd = accept(listen_fd, NULL, NULL);
  assert(client_fd >= 0);

  char buffer[16];
  const ssize_t read_size = read(client_fd, buffer, sizeof(buffer));
  assert(read_size == 4);
  assert(memcmp(buffer, "ping", 4) == 0);

  assert(write(client_fd, "pong", 4) == 4);
  shutdown(client_fd, SHUT_WR);

  close(client_fd);
  close(listen_fd);
  unlink(path);
  return NULL;
}

static void* plain_client_thread_main(void* context) {
  plain_client_context_t* client = (plain_client_context_t*)context;

  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  assert(fd >= 0);

  struct sockaddr_un address;
  socklen_t address_length = 0;
  make_sockaddr(client->path, &address, &address_length);

  assert(connect(fd, (struct sockaddr*)&address, address_length) == 0);
  assert(write(fd, "ping", 4) == 4);

  char buffer[16];
  const ssize_t read_size = read(fd, buffer, sizeof(buffer));
  assert(read_size == 4);
  assert(memcmp(buffer, "pong", 4) == 0);
  client->received_pong = true;

  while (read(fd, buffer, sizeof(buffer)) > 0) {
  }

  close(fd);
  return NULL;
}

static void test_core_client_connect(void) {
  const char* path = "/tmp/unixsock_core_client.sock";
  unlink(path);

  pthread_t server_thread;
  assert(pthread_create(&server_thread, NULL, plain_server_thread_main, (void*)path) == 0);
  usleep(100 * 1000);

  client_observer_t observer;
  client_observer_init(&observer);

  int32_t error_code = 0;
  char* error_message = NULL;
  const int32_t handle = unixsock_core_connect(
      path, 1000, client_observer_callback, &observer, &error_code, &error_message);
  if (error_message != NULL) {
    fprintf(stderr, "connect error: %s\n", error_message);
    unixsock_core_free_string(error_message);
    error_message = NULL;
  }
  assert(handle > 0);

  assert(wait_for_condition(client_connected, &observer, 100));
  assert(unixsock_core_is_thread_running());
  assert(unixsock_core_active_socket_count() == 1);

  const uint8_t ping[] = {'p', 'i', 'n', 'g'};
  assert(unixsock_core_write(handle, ping, sizeof(ping), &error_code, &error_message) == 0);
  assert(error_message == NULL);

  assert(unixsock_core_shutdown_write(handle, &error_code, &error_message) == 0);
  assert(error_message == NULL);

  assert(wait_for_condition(client_received_pong, &observer, 100));
  assert(wait_for_condition(client_closed, &observer, 100));
  assert(wait_for_condition(thread_stopped, NULL, 100));
  assert(unixsock_core_active_socket_count() == 0);

  pthread_join(server_thread, NULL);
  client_observer_destroy(&observer);
}

static void test_core_server_accept(void) {
  const char* path = "/tmp/unixsock_core_server.sock";
  unlink(path);

  server_observer_t observer;
  server_observer_init(&observer);

  int32_t error_code = 0;
  char* error_message = NULL;
  const int32_t server_handle = unixsock_core_bind_server(
      path, 8, server_observer_callback, &observer, &error_code, &error_message);
  if (error_message != NULL) {
    fprintf(stderr, "bind error: %s\n", error_message);
    unixsock_core_free_string(error_message);
    error_message = NULL;
  }
  assert(server_handle > 0);

  pthread_mutex_lock(&observer.mutex);
  observer.server_handle = server_handle;
  pthread_mutex_unlock(&observer.mutex);

  assert(unixsock_core_active_socket_count() == 1);

  plain_client_context_t client;
  memset(&client, 0, sizeof(client));
  snprintf(client.path, sizeof(client.path), "%s", path);

  pthread_t client_thread;
  assert(pthread_create(&client_thread, NULL, plain_client_thread_main, &client) == 0);

  assert(wait_for_condition(server_accepted_client, &observer, 100));
  assert(wait_for_condition(server_received_ping, &observer, 100));

  pthread_mutex_lock(&observer.mutex);
  const int32_t accepted_handle = observer.accepted_handle;
  pthread_mutex_unlock(&observer.mutex);
  assert(accepted_handle > 0);

  const uint8_t pong[] = {'p', 'o', 'n', 'g'};
  assert(unixsock_core_write(accepted_handle, pong, sizeof(pong), &error_code, &error_message) ==
         0);
  assert(error_message == NULL);

  assert(unixsock_core_shutdown_write(accepted_handle, &error_code, &error_message) == 0);
  assert(error_message == NULL);

  pthread_join(client_thread, NULL);
  assert(client.received_pong);

  assert(unixsock_core_destroy(server_handle, &error_code, &error_message) == 0);
  assert(error_message == NULL);

  assert(wait_for_condition(server_and_client_closed, &observer, 100));
  assert(wait_for_condition(thread_stopped, NULL, 100));
  assert(unixsock_core_active_socket_count() == 0);

  server_observer_destroy(&observer);
}

int main(void) {
  test_core_client_connect();
  test_core_server_accept();
  unixsock_core_shutdown_for_tests();
  return 0;
}
