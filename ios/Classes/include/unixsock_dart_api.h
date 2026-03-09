#ifndef UNIXSOCK_DART_API_H_
#define UNIXSOCK_DART_API_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UNIXSOCK_DART_API_DL_MAJOR_VERSION 2

typedef int64_t Dart_Port_DL;

typedef enum {
  Dart_TypedData_kByteData = 0,
  Dart_TypedData_kInt8,
  Dart_TypedData_kUint8,
  Dart_TypedData_kUint8Clamped,
  Dart_TypedData_kInt16,
  Dart_TypedData_kUint16,
  Dart_TypedData_kInt32,
  Dart_TypedData_kUint32,
  Dart_TypedData_kInt64,
  Dart_TypedData_kUint64,
  Dart_TypedData_kFloat32,
  Dart_TypedData_kFloat64,
  Dart_TypedData_kInt32x4,
  Dart_TypedData_kFloat32x4,
  Dart_TypedData_kFloat64x2,
  Dart_TypedData_kInvalid
} Dart_TypedData_Type;

typedef void (*Dart_HandleFinalizer)(void* isolate_callback_data, void* peer);

typedef enum {
  Dart_CObject_kNull = 0,
  Dart_CObject_kBool,
  Dart_CObject_kInt32,
  Dart_CObject_kInt64,
  Dart_CObject_kDouble,
  Dart_CObject_kString,
  Dart_CObject_kArray,
  Dart_CObject_kTypedData,
  Dart_CObject_kExternalTypedData,
  Dart_CObject_kSendPort,
  Dart_CObject_kCapability,
  Dart_CObject_kNativePointer,
  Dart_CObject_kUnsupported,
  Dart_CObject_kUnmodifiableExternalTypedData,
  Dart_CObject_kNumberOfTypes
} Dart_CObject_Type;

typedef struct _Dart_CObject {
  Dart_CObject_Type type;
  union {
    bool as_bool;
    int32_t as_int32;
    int64_t as_int64;
    double as_double;
    const char* as_string;
    struct {
      Dart_Port_DL id;
      Dart_Port_DL origin_id;
    } as_send_port;
    struct {
      int64_t id;
    } as_capability;
    struct {
      intptr_t length;
      struct _Dart_CObject** values;
    } as_array;
    struct {
      Dart_TypedData_Type type;
      intptr_t length;
      const uint8_t* values;
    } as_typed_data;
    struct {
      Dart_TypedData_Type type;
      intptr_t length;
      uint8_t* data;
      void* peer;
      Dart_HandleFinalizer callback;
    } as_external_typed_data;
    struct {
      intptr_t ptr;
      intptr_t size;
      Dart_HandleFinalizer callback;
    } as_native_pointer;
  } value;
} Dart_CObject;

typedef bool (*Dart_PostCObject_Type)(Dart_Port_DL port_id, Dart_CObject* message);

extern Dart_PostCObject_Type Dart_PostCObject_DL;

intptr_t Dart_InitializeApiDL(void* data);

#ifdef __cplusplus
}
#endif

#endif  // UNIXSOCK_DART_API_H_
