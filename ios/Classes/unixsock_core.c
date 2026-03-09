#include "internal/unixsock_core.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define UNIXSOCK_CONTROL_IDENT 1U
#define UNIXSOCK_EVENT_BUFFER_SIZE 4096
#define UNIXSOCK_OPTION_TCP_NO_DELAY 1

typedef enum {
  UNIXSOCK_KIND_CLIENT = 1,
  UNIXSOCK_KIND_SERVER = 2,
} unixsock_kind_t;

typedef struct unixsock_write_buffer {
  struct unixsock_write_buffer* next;
  size_t length;
  size_t offset;
  uint8_t data[];
} unixsock_write_buffer_t;

typedef struct unixsock_socket {
  int32_t handle;
  int fd;
  unixsock_kind_t kind;
  bool registered;
  bool connecting;
  bool has_connect_timeout;
  bool write_interest_enabled;
  bool pending_shutdown_write;
  bool write_shutdown;
  bool read_closed_emitted;
  bool closed_emitted;
  bool unlink_path_on_free;
  size_t pending_write_bytes;
  char* remote_path;
  char* local_path;
  unixsock_event_callback_t callback;
  void* context;
  unixsock_write_buffer_t* write_head;
  unixsock_write_buffer_t* write_tail;
  struct unixsock_socket* next;
} unixsock_socket_t;

typedef struct unixsock_event_node {
  unixsock_event_callback_t callback;
  void* context;
  unixsock_event_t event;
  uint8_t* owned_data;
  char* owned_address;
  char* owned_remote_address;
  char* owned_message;
  struct unixsock_event_node* next;
} unixsock_event_node_t;

typedef struct {
  pthread_mutex_t mutex;
  int kqueue_fd;
  pthread_t thread;
  bool thread_running;
  bool stop_requested;
  int32_t next_handle;
  int32_t active_socket_count;
  unixsock_socket_t* sockets;
} unixsock_manager_t;

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static unixsock_manager_t g_manager;

static void unixsock_manager_init_once(void) {
  memset(&g_manager, 0, sizeof(g_manager));
  pthread_mutex_init(&g_manager.mutex, NULL);
  g_manager.kqueue_fd = -1;
  g_manager.next_handle = 1;
}

static char* unixsock_duplicate_string(const char* value) {
  if (value == NULL) {
    return NULL;
  }
  const size_t length = strlen(value) + 1;
  char* copy = (char*)malloc(length);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, value, length);
  return copy;
}

static char* unixsock_format_message_v(const char* format, va_list args) {
  va_list args_copy;
  va_copy(args_copy, args);
  const int length = vsnprintf(NULL, 0, format, args_copy);
  va_end(args_copy);
  if (length < 0) {
    return NULL;
  }

  char* buffer = (char*)malloc((size_t)length + 1);
  if (buffer == NULL) {
    return NULL;
  }

  vsnprintf(buffer, (size_t)length + 1, format, args);
  return buffer;
}

static char* unixsock_format_message(const char* format, ...) {
  va_list args;
  va_start(args, format);
  char* message = unixsock_format_message_v(format, args);
  va_end(args);
  return message;
}

static void unixsock_set_error(int32_t* error_code,
                               char** error_message,
                               int32_t code,
                               const char* format,
                               ...) {
  if (error_code != NULL) {
    *error_code = code;
  }
  if (error_message == NULL) {
    return;
  }
  va_list args;
  va_start(args, format);
  *error_message = unixsock_format_message_v(format, args);
  va_end(args);
}

static void unixsock_set_errno_error(int32_t* error_code,
                                     char** error_message,
                                     int32_t code,
                                     const char* action) {
  unixsock_set_error(
      error_code, error_message, code, "%s: %s", action, strerror(code));
}

static unixsock_socket_t* unixsock_find_socket_by_handle_locked(int32_t handle) {
  unixsock_socket_t* current = g_manager.sockets;
  while (current != NULL) {
    if (current->handle == handle) {
      return current;
    }
    current = current->next;
  }
  return NULL;
}

static unixsock_socket_t* unixsock_find_socket_by_fd_locked(int fd) {
  unixsock_socket_t* current = g_manager.sockets;
  while (current != NULL) {
    if (current->fd == fd) {
      return current;
    }
    current = current->next;
  }
  return NULL;
}

static void unixsock_append_event(unixsock_event_node_t** head,
                                  unixsock_event_node_t** tail,
                                  unixsock_event_node_t* node) {
  if (*tail == NULL) {
    *head = node;
    *tail = node;
    return;
  }
  (*tail)->next = node;
  *tail = node;
}

static void unixsock_queue_event(unixsock_event_node_t** head,
                                 unixsock_event_node_t** tail,
                                 unixsock_socket_t* socket,
                                 unixsock_event_type_t type,
                                 int32_t related_handle,
                                 const char* address,
                                 const char* remote_address,
                                 const uint8_t* data,
                                 size_t data_length,
                                 int32_t error_code,
                                 const char* message) {
  if (socket == NULL || socket->callback == NULL) {
    return;
  }

  unixsock_event_node_t* node =
      (unixsock_event_node_t*)calloc(1, sizeof(unixsock_event_node_t));
  if (node == NULL) {
    return;
  }

  node->callback = socket->callback;
  node->context = socket->context;
  node->event.handle = socket->handle;
  node->event.type = type;
  node->event.related_handle = related_handle;
  node->event.error_code = error_code;

  if (data != NULL && data_length > 0) {
    node->owned_data = (uint8_t*)malloc(data_length);
    if (node->owned_data == NULL) {
      free(node);
      return;
    }
    memcpy(node->owned_data, data, data_length);
    node->event.data = node->owned_data;
    node->event.data_length = data_length;
  }

  if (address != NULL) {
    node->owned_address = unixsock_duplicate_string(address);
    if (node->owned_address != NULL) {
      node->event.address = node->owned_address;
    }
  }

  if (remote_address != NULL) {
    node->owned_remote_address = unixsock_duplicate_string(remote_address);
    if (node->owned_remote_address != NULL) {
      node->event.remote_address = node->owned_remote_address;
    }
  }

  if (message != NULL) {
    node->owned_message = unixsock_duplicate_string(message);
    if (node->owned_message != NULL) {
      node->event.message = node->owned_message;
    }
  }

  unixsock_append_event(head, tail, node);
}

static void unixsock_queue_simple_event(unixsock_event_node_t** head,
                                        unixsock_event_node_t** tail,
                                        unixsock_socket_t* socket,
                                        unixsock_event_type_t type) {
  unixsock_queue_event(head, tail, socket, type, 0, NULL, NULL, NULL, 0, 0, NULL);
}

static void unixsock_queue_error_event(unixsock_event_node_t** head,
                                       unixsock_event_node_t** tail,
                                       unixsock_socket_t* socket,
                                       int32_t code,
                                       const char* action) {
  char* text = unixsock_format_message("%s: %s", action, strerror(code));
  unixsock_queue_event(
      head, tail, socket, UNIXSOCK_EVENT_ERROR, 0, NULL, NULL, NULL, 0, code, text);
  free(text);
}

static void unixsock_queue_accept_event(unixsock_event_node_t** head,
                                        unixsock_event_node_t** tail,
                                        unixsock_socket_t* server,
                                        unixsock_socket_t* client) {
  unixsock_queue_event(head,
                       tail,
                       server,
                       UNIXSOCK_EVENT_ACCEPTED,
                       client->handle,
                       client->local_path,
                       client->remote_path,
                       NULL,
                       0,
                       0,
                       NULL);
}

static void unixsock_dispatch_events(unixsock_event_node_t* head) {
  while (head != NULL) {
    unixsock_event_node_t* next = head->next;
    if (head->callback != NULL) {
      head->callback(&head->event, head->context);
    }
    free(head->owned_data);
    free(head->owned_address);
    free(head->owned_remote_address);
    free(head->owned_message);
    free(head);
    head = next;
  }
}

static void unixsock_free_write_buffers(unixsock_socket_t* socket) {
  unixsock_write_buffer_t* current = socket->write_head;
  while (current != NULL) {
    unixsock_write_buffer_t* next = current->next;
    free(current);
    current = next;
  }
  socket->write_head = NULL;
  socket->write_tail = NULL;
  socket->pending_write_bytes = 0;
}

static void unixsock_socket_free(unixsock_socket_t* socket) {
  if (socket == NULL) {
    return;
  }
  if (socket->unlink_path_on_free && socket->local_path != NULL &&
      socket->local_path[0] != '\0') {
    unlink(socket->local_path);
  }
  if (socket->fd >= 0) {
    close(socket->fd);
  }
  unixsock_free_write_buffers(socket);
  free(socket->remote_path);
  free(socket->local_path);
  free(socket);
}

static bool unixsock_socket_is_server(const unixsock_socket_t* socket) {
  return socket != NULL && socket->kind == UNIXSOCK_KIND_SERVER;
}

static void unixsock_remove_socket_locked(unixsock_socket_t* socket) {
  unixsock_socket_t** current = &g_manager.sockets;
  while (*current != NULL) {
    if (*current == socket) {
      *current = socket->next;
      socket->next = NULL;
      socket->registered = false;
      g_manager.active_socket_count -= 1;
      return;
    }
    current = &(*current)->next;
  }
}

static void unixsock_trigger_control_event_locked(void) {
  if (g_manager.kqueue_fd < 0) {
    return;
  }

  struct kevent change;
  EV_SET(&change, UNIXSOCK_CONTROL_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
  (void)kevent(g_manager.kqueue_fd, &change, 1, NULL, 0, NULL);
}

static void unixsock_maybe_request_stop_locked(void) {
  if (g_manager.active_socket_count == 0 && g_manager.thread_running) {
    g_manager.stop_requested = true;
    unixsock_trigger_control_event_locked();
  }
}

static void unixsock_delete_connect_timer_locked(unixsock_socket_t* socket) {
  if (!socket->has_connect_timeout || g_manager.kqueue_fd < 0) {
    socket->has_connect_timeout = false;
    return;
  }
  struct kevent change;
  EV_SET(&change, (uintptr_t)socket->handle, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
  (void)kevent(g_manager.kqueue_fd, &change, 1, NULL, 0, NULL);
  socket->has_connect_timeout = false;
}

static void unixsock_set_write_interest_locked(unixsock_socket_t* socket, bool enabled) {
  if (unixsock_socket_is_server(socket)) {
    socket->write_interest_enabled = false;
    return;
  }
  if (g_manager.kqueue_fd < 0 || socket->fd < 0 ||
      socket->write_interest_enabled == enabled) {
    socket->write_interest_enabled = enabled;
    return;
  }

  struct kevent change;
  EV_SET(&change,
         (uintptr_t)socket->fd,
         EVFILT_WRITE,
         EV_ADD | EV_CLEAR | (enabled ? EV_ENABLE : EV_DISABLE),
         0,
         0,
         NULL);
  (void)kevent(g_manager.kqueue_fd, &change, 1, NULL, 0, NULL);
  socket->write_interest_enabled = enabled;
}

static void unixsock_delete_socket_filters_locked(unixsock_socket_t* socket) {
  if (g_manager.kqueue_fd < 0 || socket->fd < 0) {
    socket->has_connect_timeout = false;
    socket->write_interest_enabled = false;
    return;
  }

  struct kevent changes[3];
  int change_count = 0;
  EV_SET(&changes[change_count++],
         (uintptr_t)socket->fd,
         EVFILT_READ,
         EV_DELETE,
         0,
         0,
         NULL);
  EV_SET(&changes[change_count++],
         (uintptr_t)socket->fd,
         EVFILT_WRITE,
         EV_DELETE,
         0,
         0,
         NULL);
  if (socket->has_connect_timeout) {
    EV_SET(&changes[change_count++],
           (uintptr_t)socket->handle,
           EVFILT_TIMER,
           EV_DELETE,
           0,
           0,
           NULL);
  }
  (void)kevent(g_manager.kqueue_fd, changes, change_count, NULL, 0, NULL);
  socket->has_connect_timeout = false;
  socket->write_interest_enabled = false;
}

static void unixsock_update_local_path_locked(unixsock_socket_t* socket) {
  if (socket->fd < 0) {
    return;
  }

  struct sockaddr_un address;
  socklen_t address_length = sizeof(address);
  memset(&address, 0, sizeof(address));
  if (getsockname(socket->fd, (struct sockaddr*)&address, &address_length) != 0) {
    return;
  }

  if (address.sun_path[0] == '\0') {
    return;
  }

  free(socket->local_path);
  socket->local_path = unixsock_duplicate_string(address.sun_path);
}

static void unixsock_close_socket_locked(unixsock_socket_t* socket,
                                         unixsock_event_node_t** head,
                                         unixsock_event_node_t** tail,
                                         bool emit_closed_event) {
  if (socket->fd >= 0) {
    unixsock_delete_socket_filters_locked(socket);
    close(socket->fd);
    socket->fd = -1;
  }

  unixsock_free_write_buffers(socket);

  if (emit_closed_event && !socket->closed_emitted) {
    socket->closed_emitted = true;
    unixsock_queue_simple_event(head, tail, socket, UNIXSOCK_EVENT_CLOSED);
  }

  if (socket->registered) {
    unixsock_remove_socket_locked(socket);
    unixsock_maybe_request_stop_locked();
  }
}

static int unixsock_make_sockaddr(const char* path,
                                  struct sockaddr_un* address,
                                  socklen_t* address_length,
                                  int32_t* error_code,
                                  char** error_message) {
  if (path == NULL || path[0] == '\0') {
    unixsock_set_error(error_code, error_message, EINVAL, "socket path is empty");
    return -1;
  }

  const size_t path_length = strlen(path);
  if (path_length >= sizeof(address->sun_path)) {
    unixsock_set_error(error_code,
                       error_message,
                       ENAMETOOLONG,
                       "socket path is too long");
    return -1;
  }

  memset(address, 0, sizeof(*address));
  address->sun_family = AF_UNIX;
  memcpy(address->sun_path, path, path_length + 1);
  *address_length = (socklen_t)sizeof(*address);
#ifdef __APPLE__
  address->sun_len = (uint8_t)(*address_length);
#endif
  return 0;
}

static int unixsock_make_nonblocking_socket(int32_t* error_code,
                                            char** error_message) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    unixsock_set_errno_error(error_code, error_message, errno, "socket");
    return -1;
  }

  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    unixsock_set_errno_error(error_code, error_message, errno, "fcntl");
    close(fd);
    return -1;
  }

  const int no_sigpipe = 1;
#ifdef SO_NOSIGPIPE
  (void)setsockopt(fd,
                   SOL_SOCKET,
                   SO_NOSIGPIPE,
                   &no_sigpipe,
                   (socklen_t)sizeof(no_sigpipe));
#endif
  return fd;
}

static char* unixsock_copy_sockaddr_path(const struct sockaddr_un* address,
                                         socklen_t address_length) {
  if (address == NULL || address_length <= offsetof(struct sockaddr_un, sun_path)) {
    return unixsock_duplicate_string("");
  }

  const size_t max_length =
      (size_t)address_length - offsetof(struct sockaddr_un, sun_path);
  size_t length = strnlen(address->sun_path, max_length);
  if (length == 0 && address->sun_path[0] == '\0') {
    return unixsock_duplicate_string("");
  }

  char* copy = (char*)malloc(length + 1);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, address->sun_path, length);
  copy[length] = '\0';
  return copy;
}

static unixsock_socket_t* unixsock_allocate_socket_record(unixsock_kind_t kind,
                                                          int fd,
                                                          unixsock_event_callback_t callback,
                                                          void* context) {
  unixsock_socket_t* socket =
      (unixsock_socket_t*)calloc(1, sizeof(unixsock_socket_t));
  if (socket == NULL) {
    return NULL;
  }
  socket->fd = fd;
  socket->kind = kind;
  socket->callback = callback;
  socket->context = context;
  return socket;
}

static int unixsock_register_socket_locked(unixsock_socket_t* socket,
                                           bool enable_write,
                                           int32_t timeout_ms,
                                           int32_t* error_code,
                                           char** error_message) {
  socket->handle = g_manager.next_handle++;
  socket->registered = true;
  socket->next = g_manager.sockets;
  g_manager.sockets = socket;
  g_manager.active_socket_count += 1;

  struct kevent changes[3];
  int change_count = 0;
  EV_SET(&changes[change_count++],
         (uintptr_t)socket->fd,
         EVFILT_READ,
         EV_ADD | EV_CLEAR | EV_ENABLE,
         0,
         0,
         NULL);
  if (!unixsock_socket_is_server(socket)) {
    EV_SET(&changes[change_count++],
           (uintptr_t)socket->fd,
           EVFILT_WRITE,
           EV_ADD | EV_CLEAR | (enable_write ? EV_ENABLE : EV_DISABLE),
           0,
           0,
           NULL);
    socket->write_interest_enabled = enable_write;
    if (timeout_ms > 0) {
      EV_SET(&changes[change_count++],
             (uintptr_t)socket->handle,
             EVFILT_TIMER,
             EV_ADD | EV_ONESHOT,
             0,
             timeout_ms,
             NULL);
      socket->has_connect_timeout = true;
    }
  }

  if (kevent(g_manager.kqueue_fd, changes, change_count, NULL, 0, NULL) == 0) {
    return 0;
  }

  unixsock_set_errno_error(error_code, error_message, errno, "kevent add socket");
  unixsock_remove_socket_locked(socket);
  socket->registered = false;
  socket->write_interest_enabled = false;
  socket->has_connect_timeout = false;
  return -1;
}

static bool unixsock_try_flush_locked(unixsock_socket_t* socket,
                                      unixsock_event_node_t** head,
                                      unixsock_event_node_t** tail) {
  while (socket->write_head != NULL && socket->fd >= 0) {
    unixsock_write_buffer_t* buffer = socket->write_head;
    const size_t remaining = buffer->length - buffer->offset;
    const ssize_t written =
        send(socket->fd, buffer->data + buffer->offset, remaining, 0);

    if (written > 0) {
      buffer->offset += (size_t)written;
      socket->pending_write_bytes -= (size_t)written;
      if (buffer->offset == buffer->length) {
        socket->write_head = buffer->next;
        if (socket->write_head == NULL) {
          socket->write_tail = NULL;
        }
        free(buffer);
      }
      continue;
    }

    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      unixsock_set_write_interest_locked(socket, true);
      return true;
    }

    const int saved_errno = errno == 0 ? EIO : errno;
    unixsock_queue_error_event(head, tail, socket, saved_errno, "write");
    unixsock_close_socket_locked(socket, head, tail, true);
    return false;
  }

  if (socket->write_head == NULL) {
    unixsock_set_write_interest_locked(socket, socket->connecting);
    if (socket->pending_shutdown_write && !socket->write_shutdown && socket->fd >= 0) {
      if (shutdown(socket->fd, SHUT_WR) == 0 || errno == ENOTCONN) {
        socket->write_shutdown = true;
      }
    }
    unixsock_queue_simple_event(head, tail, socket, UNIXSOCK_EVENT_DRAINED);
  }

  return true;
}

static void unixsock_handle_connect_complete_locked(unixsock_socket_t* socket,
                                                    unixsock_event_node_t** head,
                                                    unixsock_event_node_t** tail) {
  int socket_error = 0;
  socklen_t socket_error_length = sizeof(socket_error);
  if (getsockopt(socket->fd,
                 SOL_SOCKET,
                 SO_ERROR,
                 &socket_error,
                 &socket_error_length) != 0) {
    socket_error = errno == 0 ? EIO : errno;
  }

  if (socket_error != 0) {
    unixsock_queue_error_event(head, tail, socket, socket_error, "connect");
    unixsock_close_socket_locked(socket, head, tail, true);
    return;
  }

  socket->connecting = false;
  unixsock_delete_connect_timer_locked(socket);
  unixsock_update_local_path_locked(socket);
  unixsock_queue_simple_event(head, tail, socket, UNIXSOCK_EVENT_CONNECTED);
  (void)unixsock_try_flush_locked(socket, head, tail);
}

static void unixsock_handle_write_event_locked(unixsock_socket_t* socket,
                                               unixsock_event_node_t** head,
                                               unixsock_event_node_t** tail) {
  if (socket->fd < 0) {
    return;
  }
  if (socket->connecting) {
    unixsock_handle_connect_complete_locked(socket, head, tail);
    if (socket->fd < 0) {
      return;
    }
  }
  (void)unixsock_try_flush_locked(socket, head, tail);
}

static void unixsock_accept_connections_locked(unixsock_socket_t* server,
                                               unixsock_event_node_t** head,
                                               unixsock_event_node_t** tail) {
  for (;;) {
    struct sockaddr_un remote_address;
    socklen_t remote_address_length = sizeof(remote_address);
    memset(&remote_address, 0, sizeof(remote_address));

    const int client_fd =
        accept(server->fd, (struct sockaddr*)&remote_address, &remote_address_length);
    if (client_fd >= 0) {
      const int flags = fcntl(client_fd, F_GETFL, 0);
      if (flags < 0 || fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        const int saved_errno = errno == 0 ? EIO : errno;
        close(client_fd);
        unixsock_queue_error_event(head, tail, server, saved_errno, "fcntl accepted socket");
        unixsock_close_socket_locked(server, head, tail, true);
        return;
      }

      const int no_sigpipe = 1;
#ifdef SO_NOSIGPIPE
      (void)setsockopt(client_fd,
                       SOL_SOCKET,
                       SO_NOSIGPIPE,
                       &no_sigpipe,
                       (socklen_t)sizeof(no_sigpipe));
#endif

      unixsock_socket_t* client = unixsock_allocate_socket_record(
          UNIXSOCK_KIND_CLIENT, client_fd, server->callback, server->context);
      if (client == NULL) {
        close(client_fd);
        unixsock_queue_error_event(head, tail, server, ENOMEM, "accept allocate client");
        return;
      }

      client->local_path = unixsock_duplicate_string(
          server->local_path == NULL ? "" : server->local_path);
      client->remote_path =
          unixsock_copy_sockaddr_path(&remote_address, remote_address_length);
      if (client->local_path == NULL || client->remote_path == NULL) {
        unixsock_socket_free(client);
        unixsock_queue_error_event(head, tail, server, ENOMEM, "accept copy path");
        return;
      }

      if (unixsock_register_socket_locked(client, false, 0, NULL, NULL) != 0) {
        unixsock_socket_free(client);
        unixsock_queue_error_event(head, tail, server, errno == 0 ? EIO : errno, "accept register");
        return;
      }

      unixsock_queue_accept_event(head, tail, server, client);
      continue;
    }

    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }

    const int saved_errno = errno == 0 ? EIO : errno;
    unixsock_queue_error_event(head, tail, server, saved_errno, "accept");
    unixsock_close_socket_locked(server, head, tail, true);
    return;
  }
}

static void unixsock_handle_read_event_locked(unixsock_socket_t* socket,
                                              unixsock_event_node_t** head,
                                              unixsock_event_node_t** tail) {
  if (socket->fd < 0) {
    return;
  }

  if (unixsock_socket_is_server(socket)) {
    unixsock_accept_connections_locked(socket, head, tail);
    return;
  }

  for (;;) {
    uint8_t buffer[UNIXSOCK_EVENT_BUFFER_SIZE];
    const ssize_t read_count = recv(socket->fd, buffer, sizeof(buffer), 0);

    if (read_count > 0) {
      unixsock_queue_event(head,
                           tail,
                           socket,
                           UNIXSOCK_EVENT_DATA,
                           0,
                           NULL,
                           NULL,
                           buffer,
                           (size_t)read_count,
                           0,
                           NULL);
      continue;
    }

    if (read_count == 0) {
      if (!socket->read_closed_emitted) {
        socket->read_closed_emitted = true;
        unixsock_queue_simple_event(head, tail, socket, UNIXSOCK_EVENT_READ_CLOSED);
      }
      unixsock_close_socket_locked(socket, head, tail, true);
      return;
    }

    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }

    const int saved_errno = errno == 0 ? EIO : errno;
    unixsock_queue_error_event(head, tail, socket, saved_errno, "read");
    unixsock_close_socket_locked(socket, head, tail, true);
    return;
  }
}

static void* unixsock_polling_thread_main(void* context) {
  (void)context;

  for (;;) {
    struct kevent event;
    const int ready = kevent(g_manager.kqueue_fd, NULL, 0, &event, 1, NULL);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (ready == 0) {
      continue;
    }

    unixsock_event_node_t* event_head = NULL;
    unixsock_event_node_t* event_tail = NULL;
    unixsock_socket_t* socket_to_free = NULL;
    bool should_stop = false;

    pthread_mutex_lock(&g_manager.mutex);
    if (event.filter == EVFILT_USER && event.ident == UNIXSOCK_CONTROL_IDENT) {
      should_stop = g_manager.stop_requested && g_manager.active_socket_count == 0;
      pthread_mutex_unlock(&g_manager.mutex);
      if (should_stop) {
        break;
      }
      continue;
    }

    if (event.filter == EVFILT_TIMER) {
      unixsock_socket_t* socket =
          unixsock_find_socket_by_handle_locked((int32_t)event.ident);
      if (socket != NULL && socket->connecting) {
        socket->has_connect_timeout = false;
        unixsock_queue_error_event(
            &event_head, &event_tail, socket, ETIMEDOUT, "connect timeout");
        unixsock_close_socket_locked(socket, &event_head, &event_tail, true);
        socket_to_free = socket;
      }
    } else {
      unixsock_socket_t* socket =
          unixsock_find_socket_by_fd_locked((int)event.ident);
      if (socket != NULL) {
        if ((event.flags & EV_ERROR) != 0) {
          unixsock_queue_error_event(
              &event_head, &event_tail, socket, (int32_t)event.data, "kevent");
          unixsock_close_socket_locked(socket, &event_head, &event_tail, true);
          socket_to_free = socket;
        } else if (event.filter == EVFILT_WRITE) {
          unixsock_handle_write_event_locked(socket, &event_head, &event_tail);
          if (!socket->registered) {
            socket_to_free = socket;
          }
        } else if (event.filter == EVFILT_READ) {
          unixsock_handle_read_event_locked(socket, &event_head, &event_tail);
          if (!socket->registered) {
            socket_to_free = socket;
          }
        }
      }
    }

    should_stop = g_manager.stop_requested && g_manager.active_socket_count == 0;
    pthread_mutex_unlock(&g_manager.mutex);

    unixsock_dispatch_events(event_head);
    if (socket_to_free != NULL) {
      unixsock_socket_free(socket_to_free);
    }
    if (should_stop) {
      break;
    }
  }

  pthread_mutex_lock(&g_manager.mutex);
  if (g_manager.kqueue_fd >= 0) {
    close(g_manager.kqueue_fd);
    g_manager.kqueue_fd = -1;
  }
  g_manager.thread_running = false;
  g_manager.stop_requested = false;
  pthread_mutex_unlock(&g_manager.mutex);
  return NULL;
}

static int unixsock_start_thread_locked(int32_t* error_code, char** error_message) {
  if (g_manager.thread_running) {
    g_manager.stop_requested = false;
    return 0;
  }

  g_manager.kqueue_fd = kqueue();
  if (g_manager.kqueue_fd < 0) {
    unixsock_set_errno_error(error_code, error_message, errno, "kqueue");
    return -1;
  }

  struct kevent control_event;
  EV_SET(
      &control_event, UNIXSOCK_CONTROL_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
  if (kevent(g_manager.kqueue_fd, &control_event, 1, NULL, 0, NULL) != 0) {
    unixsock_set_errno_error(error_code, error_message, errno, "kevent register");
    close(g_manager.kqueue_fd);
    g_manager.kqueue_fd = -1;
    return -1;
  }

  if (pthread_create(&g_manager.thread, NULL, unixsock_polling_thread_main, NULL) != 0) {
    unixsock_set_errno_error(error_code, error_message, errno, "pthread_create");
    close(g_manager.kqueue_fd);
    g_manager.kqueue_fd = -1;
    return -1;
  }

  g_manager.thread_running = true;
  g_manager.stop_requested = false;
  return 0;
}

int unixsock_core_initialize(void) {
  pthread_once(&g_once, unixsock_manager_init_once);
  return 0;
}

int32_t unixsock_core_connect(const char* path,
                              int32_t timeout_ms,
                              unixsock_event_callback_t callback,
                              void* context,
                              int32_t* error_code,
                              char** error_message) {
  pthread_once(&g_once, unixsock_manager_init_once);

  struct sockaddr_un address;
  socklen_t address_length = 0;
  if (unixsock_make_sockaddr(path, &address, &address_length, error_code, error_message) !=
      0) {
    return -1;
  }

  int fd = unixsock_make_nonblocking_socket(error_code, error_message);
  if (fd < 0) {
    return -1;
  }

  unixsock_socket_t* socket =
      unixsock_allocate_socket_record(UNIXSOCK_KIND_CLIENT, fd, callback, context);
  if (socket == NULL) {
    unixsock_set_error(error_code, error_message, ENOMEM, "out of memory");
    close(fd);
    return -1;
  }

  socket->remote_path = unixsock_duplicate_string(path);
  if (socket->remote_path == NULL) {
    unixsock_set_error(error_code, error_message, ENOMEM, "out of memory");
    unixsock_socket_free(socket);
    return -1;
  }

  pthread_mutex_lock(&g_manager.mutex);
  if (unixsock_start_thread_locked(error_code, error_message) != 0) {
    pthread_mutex_unlock(&g_manager.mutex);
    unixsock_socket_free(socket);
    return -1;
  }

  if (unixsock_register_socket_locked(
          socket, true, timeout_ms, error_code, error_message) != 0) {
    pthread_mutex_unlock(&g_manager.mutex);
    unixsock_socket_free(socket);
    return -1;
  }

  const int connect_result =
      connect(socket->fd, (struct sockaddr*)&address, address_length);
  unixsock_event_node_t* event_head = NULL;
  unixsock_event_node_t* event_tail = NULL;
  int32_t handle = socket->handle;

  if (connect_result == 0) {
    socket->connecting = false;
    unixsock_delete_connect_timer_locked(socket);
    unixsock_update_local_path_locked(socket);
    unixsock_set_write_interest_locked(socket, false);
    unixsock_queue_simple_event(&event_head, &event_tail, socket, UNIXSOCK_EVENT_CONNECTED);
  } else if (errno == EINPROGRESS) {
    socket->connecting = true;
  } else {
    const int saved_errno = errno;
    unixsock_set_errno_error(error_code, error_message, saved_errno, "connect");
    unixsock_close_socket_locked(socket, NULL, NULL, false);
    pthread_mutex_unlock(&g_manager.mutex);
    unixsock_socket_free(socket);
    return -1;
  }

  pthread_mutex_unlock(&g_manager.mutex);
  unixsock_dispatch_events(event_head);
  return handle;
}

int32_t unixsock_core_bind_server(const char* path,
                                  int32_t backlog,
                                  unixsock_event_callback_t callback,
                                  void* context,
                                  int32_t* error_code,
                                  char** error_message) {
  pthread_once(&g_once, unixsock_manager_init_once);

  struct sockaddr_un address;
  socklen_t address_length = 0;
  if (unixsock_make_sockaddr(path, &address, &address_length, error_code, error_message) !=
      0) {
    return -1;
  }

  int fd = unixsock_make_nonblocking_socket(error_code, error_message);
  if (fd < 0) {
    return -1;
  }

  unixsock_socket_t* server =
      unixsock_allocate_socket_record(UNIXSOCK_KIND_SERVER, fd, callback, context);
  if (server == NULL) {
    unixsock_set_error(error_code, error_message, ENOMEM, "out of memory");
    close(fd);
    return -1;
  }

  server->local_path = unixsock_duplicate_string(path);
  if (server->local_path == NULL) {
    unixsock_set_error(error_code, error_message, ENOMEM, "out of memory");
    unixsock_socket_free(server);
    return -1;
  }

  if (bind(fd, (struct sockaddr*)&address, address_length) != 0) {
    unixsock_set_errno_error(error_code, error_message, errno, "bind");
    unixsock_socket_free(server);
    return -1;
  }
  server->unlink_path_on_free = true;

  const int effective_backlog = backlog > 0 ? backlog : SOMAXCONN;
  if (listen(fd, effective_backlog) != 0) {
    unixsock_set_errno_error(error_code, error_message, errno, "listen");
    unixsock_socket_free(server);
    return -1;
  }

  pthread_mutex_lock(&g_manager.mutex);
  if (unixsock_start_thread_locked(error_code, error_message) != 0) {
    pthread_mutex_unlock(&g_manager.mutex);
    unixsock_socket_free(server);
    return -1;
  }

  if (unixsock_register_socket_locked(server, false, 0, error_code, error_message) != 0) {
    pthread_mutex_unlock(&g_manager.mutex);
    unixsock_socket_free(server);
    return -1;
  }

  const int32_t handle = server->handle;
  pthread_mutex_unlock(&g_manager.mutex);
  return handle;
}

int32_t unixsock_core_write(int32_t handle,
                            const uint8_t* bytes,
                            size_t length,
                            int32_t* error_code,
                            char** error_message) {
  pthread_once(&g_once, unixsock_manager_init_once);
  if (length == 0) {
    return 0;
  }
  if (bytes == NULL) {
    unixsock_set_error(error_code, error_message, EINVAL, "write buffer is null");
    return -1;
  }

  pthread_mutex_lock(&g_manager.mutex);
  unixsock_socket_t* socket = unixsock_find_socket_by_handle_locked(handle);
  if (socket == NULL || socket->fd < 0) {
    pthread_mutex_unlock(&g_manager.mutex);
    unixsock_set_error(error_code, error_message, EBADF, "invalid socket handle");
    return -1;
  }

  unixsock_write_buffer_t* buffer =
      (unixsock_write_buffer_t*)malloc(sizeof(unixsock_write_buffer_t) + length);
  if (buffer == NULL) {
    pthread_mutex_unlock(&g_manager.mutex);
    unixsock_set_error(error_code, error_message, ENOMEM, "out of memory");
    return -1;
  }

  memset(buffer, 0, sizeof(unixsock_write_buffer_t));
  buffer->length = length;
  memcpy(buffer->data, bytes, length);

  if (socket->write_tail == NULL) {
    socket->write_head = buffer;
    socket->write_tail = buffer;
  } else {
    socket->write_tail->next = buffer;
    socket->write_tail = buffer;
  }
  socket->pending_write_bytes += length;

  unixsock_event_node_t* event_head = NULL;
  unixsock_event_node_t* event_tail = NULL;
  unixsock_socket_t* socket_to_free = NULL;

  if (socket->connecting) {
    unixsock_set_write_interest_locked(socket, true);
  } else {
    (void)unixsock_try_flush_locked(socket, &event_head, &event_tail);
    if (!socket->registered) {
      socket_to_free = socket;
    }
  }

  pthread_mutex_unlock(&g_manager.mutex);
  unixsock_dispatch_events(event_head);
  if (socket_to_free != NULL) {
    unixsock_socket_free(socket_to_free);
  }
  return 0;
}

int32_t unixsock_core_shutdown_write(int32_t handle,
                                     int32_t* error_code,
                                     char** error_message) {
  pthread_once(&g_once, unixsock_manager_init_once);
  (void)error_code;
  (void)error_message;

  pthread_mutex_lock(&g_manager.mutex);
  unixsock_socket_t* socket = unixsock_find_socket_by_handle_locked(handle);
  if (socket == NULL || socket->fd < 0) {
    pthread_mutex_unlock(&g_manager.mutex);
    return 0;
  }

  socket->pending_shutdown_write = true;

  unixsock_event_node_t* event_head = NULL;
  unixsock_event_node_t* event_tail = NULL;
  unixsock_socket_t* socket_to_free = NULL;

  if (!socket->connecting && socket->pending_write_bytes == 0 && !socket->write_shutdown) {
    if (shutdown(socket->fd, SHUT_WR) == 0 || errno == ENOTCONN) {
      socket->write_shutdown = true;
      unixsock_queue_simple_event(&event_head, &event_tail, socket, UNIXSOCK_EVENT_DRAINED);
    } else {
      const int saved_errno = errno == 0 ? EIO : errno;
      unixsock_queue_error_event(&event_head, &event_tail, socket, saved_errno, "shutdown");
      unixsock_close_socket_locked(socket, &event_head, &event_tail, true);
      socket_to_free = socket;
    }
  }

  pthread_mutex_unlock(&g_manager.mutex);
  unixsock_dispatch_events(event_head);
  if (socket_to_free != NULL) {
    unixsock_socket_free(socket_to_free);
  }
  return 0;
}

int32_t unixsock_core_destroy(int32_t handle,
                              int32_t* error_code,
                              char** error_message) {
  (void)error_code;
  (void)error_message;
  pthread_once(&g_once, unixsock_manager_init_once);

  pthread_mutex_lock(&g_manager.mutex);
  unixsock_socket_t* socket = unixsock_find_socket_by_handle_locked(handle);
  if (socket == NULL) {
    pthread_mutex_unlock(&g_manager.mutex);
    return 0;
  }

  unixsock_event_node_t* event_head = NULL;
  unixsock_event_node_t* event_tail = NULL;
  unixsock_close_socket_locked(socket, &event_head, &event_tail, true);
  pthread_mutex_unlock(&g_manager.mutex);

  unixsock_dispatch_events(event_head);
  unixsock_socket_free(socket);
  return 0;
}

int32_t unixsock_core_set_option(int32_t handle,
                                 int32_t option,
                                 bool enabled,
                                 int32_t* error_code,
                                 char** error_message) {
  pthread_once(&g_once, unixsock_manager_init_once);

  pthread_mutex_lock(&g_manager.mutex);
  unixsock_socket_t* socket = unixsock_find_socket_by_handle_locked(handle);
  if (socket == NULL || socket->fd < 0) {
    pthread_mutex_unlock(&g_manager.mutex);
    unixsock_set_error(error_code, error_message, EBADF, "invalid socket handle");
    return -1;
  }

  int32_t result = 0;
  if (option == UNIXSOCK_OPTION_TCP_NO_DELAY) {
    const int value = enabled ? 1 : 0;
    if (setsockopt(socket->fd,
                   IPPROTO_TCP,
                   TCP_NODELAY,
                   &value,
                   (socklen_t)sizeof(value)) == 0) {
      result = 1;
    } else {
      result = 0;
    }
  } else {
    pthread_mutex_unlock(&g_manager.mutex);
    unixsock_set_error(error_code, error_message, ENOTSUP, "unsupported socket option");
    return -1;
  }

  pthread_mutex_unlock(&g_manager.mutex);
  return result;
}

int32_t unixsock_core_get_raw_option(int32_t handle,
                                     int32_t level,
                                     int32_t option,
                                     uint8_t* value,
                                     size_t value_length,
                                     int32_t* actual_length,
                                     int32_t* error_code,
                                     char** error_message) {
  pthread_once(&g_once, unixsock_manager_init_once);

  pthread_mutex_lock(&g_manager.mutex);
  unixsock_socket_t* socket = unixsock_find_socket_by_handle_locked(handle);
  if (socket == NULL || socket->fd < 0) {
    pthread_mutex_unlock(&g_manager.mutex);
    unixsock_set_error(error_code, error_message, EBADF, "invalid socket handle");
    return -1;
  }

  socklen_t result_length = (socklen_t)value_length;
  if (getsockopt(socket->fd, level, option, value, &result_length) != 0) {
    const int saved_errno = errno == 0 ? EIO : errno;
    pthread_mutex_unlock(&g_manager.mutex);
    unixsock_set_errno_error(error_code, error_message, saved_errno, "getsockopt");
    return -1;
  }
  pthread_mutex_unlock(&g_manager.mutex);

  if (actual_length != NULL) {
    *actual_length = (int32_t)result_length;
  }
  return 0;
}

int32_t unixsock_core_set_raw_option(int32_t handle,
                                     int32_t level,
                                     int32_t option,
                                     const uint8_t* value,
                                     size_t value_length,
                                     int32_t* error_code,
                                     char** error_message) {
  pthread_once(&g_once, unixsock_manager_init_once);

  pthread_mutex_lock(&g_manager.mutex);
  unixsock_socket_t* socket = unixsock_find_socket_by_handle_locked(handle);
  if (socket == NULL || socket->fd < 0) {
    pthread_mutex_unlock(&g_manager.mutex);
    unixsock_set_error(error_code, error_message, EBADF, "invalid socket handle");
    return -1;
  }

  if (setsockopt(socket->fd, level, option, value, (socklen_t)value_length) != 0) {
    const int saved_errno = errno == 0 ? EIO : errno;
    pthread_mutex_unlock(&g_manager.mutex);
    unixsock_set_errno_error(error_code, error_message, saved_errno, "setsockopt");
    return -1;
  }
  pthread_mutex_unlock(&g_manager.mutex);
  return 0;
}

int64_t unixsock_core_pending_write_bytes(int32_t handle,
                                          int32_t* error_code,
                                          char** error_message) {
  pthread_once(&g_once, unixsock_manager_init_once);

  pthread_mutex_lock(&g_manager.mutex);
  unixsock_socket_t* socket = unixsock_find_socket_by_handle_locked(handle);
  if (socket == NULL) {
    pthread_mutex_unlock(&g_manager.mutex);
    unixsock_set_error(error_code, error_message, EBADF, "invalid socket handle");
    return -1;
  }

  const int64_t pending = (int64_t)socket->pending_write_bytes;
  pthread_mutex_unlock(&g_manager.mutex);
  return pending;
}

bool unixsock_core_is_thread_running(void) {
  pthread_once(&g_once, unixsock_manager_init_once);
  pthread_mutex_lock(&g_manager.mutex);
  const bool running = g_manager.thread_running;
  pthread_mutex_unlock(&g_manager.mutex);
  return running;
}

int32_t unixsock_core_active_socket_count(void) {
  pthread_once(&g_once, unixsock_manager_init_once);
  pthread_mutex_lock(&g_manager.mutex);
  const int32_t count = g_manager.active_socket_count;
  pthread_mutex_unlock(&g_manager.mutex);
  return count;
}

void unixsock_core_shutdown_for_tests(void) {
  pthread_once(&g_once, unixsock_manager_init_once);

  pthread_t thread;
  bool should_join = false;

  pthread_mutex_lock(&g_manager.mutex);
  should_join = g_manager.thread_running;
  thread = g_manager.thread;
  g_manager.stop_requested = true;
  unixsock_trigger_control_event_locked();
  pthread_mutex_unlock(&g_manager.mutex);

  if (should_join) {
    pthread_join(thread, NULL);
  }

  pthread_mutex_lock(&g_manager.mutex);
  unixsock_socket_t* current = g_manager.sockets;
  g_manager.sockets = NULL;
  g_manager.active_socket_count = 0;
  pthread_mutex_unlock(&g_manager.mutex);

  while (current != NULL) {
    unixsock_socket_t* next = current->next;
    unixsock_socket_free(current);
    current = next;
  }
}

void unixsock_core_free_string(char* string) {
  free(string);
}
