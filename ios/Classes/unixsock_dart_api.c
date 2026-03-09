#include "include/unixsock_dart_api.h"

#include <stddef.h>
#include <string.h>

typedef struct {
  const char* name;
  void (*function)(void);
} UnixsockDartApiEntry;

typedef struct {
  const int major;
  const int minor;
  const UnixsockDartApiEntry* const functions;
} UnixsockDartApi;

Dart_PostCObject_Type Dart_PostCObject_DL = NULL;

intptr_t Dart_InitializeApiDL(void* data) {
  UnixsockDartApi* api = (UnixsockDartApi*)data;
  if (api == NULL || api->major != UNIXSOCK_DART_API_DL_MAJOR_VERSION) {
    return -1;
  }

  Dart_PostCObject_DL = NULL;
  const UnixsockDartApiEntry* entry = api->functions;
  while (entry != NULL && entry->name != NULL) {
    if (strcmp(entry->name, "Dart_PostCObject") == 0) {
      Dart_PostCObject_DL = (Dart_PostCObject_Type)entry->function;
      break;
    }
    entry++;
  }

  return Dart_PostCObject_DL == NULL ? -1 : 0;
}
