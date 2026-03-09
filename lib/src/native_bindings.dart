import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

abstract interface class UnixsockBinding {
  void ensureInitialized();

  int connect({
    required String path,
    required SendPort sendPort,
    Duration? timeout,
  });

  int bindServer({
    required String path,
    required SendPort sendPort,
    int backlog,
  });

  void write(int handle, Uint8List bytes);

  void shutdownWrite(int handle);

  void destroy(int handle);

  bool setOption(int handle, SocketOption option, bool enabled);

  Uint8List getRawOption(int handle, RawSocketOption option);

  void setRawOption(int handle, RawSocketOption option);

  int pendingWriteBytes(int handle);
}

final class UnixsockBindings {
  static UnixsockBinding? debugOverride;

  static final UnixsockBinding _ffiBinding = _FfiUnixsockBinding();

  static UnixsockBinding get instance => debugOverride ?? _ffiBinding;
}

typedef _InitializeNative = IntPtr Function(Pointer<Void>);
typedef _InitializeDart = int Function(Pointer<Void>);

typedef _ConnectNative = Int32 Function(
  Pointer<Utf8>,
  Int64,
  Int32,
  Pointer<Int32>,
  Pointer<Pointer<Char>>,
);
typedef _ConnectDart = int Function(
  Pointer<Utf8>,
  int,
  int,
  Pointer<Int32>,
  Pointer<Pointer<Char>>,
);

typedef _BindServerNative = Int32 Function(
  Pointer<Utf8>,
  Int64,
  Int32,
  Pointer<Int32>,
  Pointer<Pointer<Char>>,
);
typedef _BindServerDart = int Function(
  Pointer<Utf8>,
  int,
  int,
  Pointer<Int32>,
  Pointer<Pointer<Char>>,
);

typedef _WriteNative = Int32 Function(
  Int32,
  Pointer<Uint8>,
  Int32,
  Pointer<Int32>,
  Pointer<Pointer<Char>>,
);
typedef _WriteDart = int Function(
  int,
  Pointer<Uint8>,
  int,
  Pointer<Int32>,
  Pointer<Pointer<Char>>,
);

typedef _SimpleOperationNative = Int32 Function(
  Int32,
  Pointer<Int32>,
  Pointer<Pointer<Char>>,
);
typedef _SimpleOperationDart = int Function(
  int,
  Pointer<Int32>,
  Pointer<Pointer<Char>>,
);

typedef _SetOptionNative = Int32 Function(
  Int32,
  Int32,
  Uint8,
  Pointer<Int32>,
  Pointer<Pointer<Char>>,
);
typedef _SetOptionDart = int Function(
  int,
  int,
  int,
  Pointer<Int32>,
  Pointer<Pointer<Char>>,
);

typedef _GetRawOptionNative = Int32 Function(
  Int32,
  Int32,
  Int32,
  Pointer<Uint8>,
  Int32,
  Pointer<Int32>,
  Pointer<Int32>,
  Pointer<Pointer<Char>>,
);
typedef _GetRawOptionDart = int Function(
  int,
  int,
  int,
  Pointer<Uint8>,
  int,
  Pointer<Int32>,
  Pointer<Int32>,
  Pointer<Pointer<Char>>,
);

typedef _SetRawOptionNative = Int32 Function(
  Int32,
  Int32,
  Int32,
  Pointer<Uint8>,
  Int32,
  Pointer<Int32>,
  Pointer<Pointer<Char>>,
);
typedef _SetRawOptionDart = int Function(
  int,
  int,
  int,
  Pointer<Uint8>,
  int,
  Pointer<Int32>,
  Pointer<Pointer<Char>>,
);

typedef _PendingBytesNative = Int64 Function(
  Int32,
  Pointer<Int32>,
  Pointer<Pointer<Char>>,
);
typedef _PendingBytesDart = int Function(
  int,
  Pointer<Int32>,
  Pointer<Pointer<Char>>,
);

typedef _FreeStringNative = Void Function(Pointer<Char>);
typedef _FreeStringDart = void Function(Pointer<Char>);

final class _FfiUnixsockBinding implements UnixsockBinding {
  _FfiUnixsockBinding() : _library = DynamicLibrary.process();

  final DynamicLibrary _library;

  late final _InitializeDart _initialize =
      _library.lookupFunction<_InitializeNative, _InitializeDart>(
          'unixsock_initialize_dart_api');
  late final _ConnectDart _connect =
      _library.lookupFunction<_ConnectNative, _ConnectDart>('unixsock_connect');
  late final _BindServerDart _bindServer =
      _library.lookupFunction<_BindServerNative, _BindServerDart>(
          'unixsock_bind_server');
  late final _WriteDart _write =
      _library.lookupFunction<_WriteNative, _WriteDart>('unixsock_write');
  late final _SimpleOperationDart _shutdownWrite =
      _library.lookupFunction<_SimpleOperationNative, _SimpleOperationDart>(
          'unixsock_shutdown_write');
  late final _SimpleOperationDart _destroy =
      _library.lookupFunction<_SimpleOperationNative, _SimpleOperationDart>(
          'unixsock_destroy');
  late final _SetOptionDart _setOption = _library
      .lookupFunction<_SetOptionNative, _SetOptionDart>('unixsock_set_option');
  late final _GetRawOptionDart _getRawOption =
      _library.lookupFunction<_GetRawOptionNative, _GetRawOptionDart>(
          'unixsock_get_raw_option');
  late final _SetRawOptionDart _setRawOption =
      _library.lookupFunction<_SetRawOptionNative, _SetRawOptionDart>(
          'unixsock_set_raw_option');
  late final _PendingBytesDart _pendingWriteBytes =
      _library.lookupFunction<_PendingBytesNative, _PendingBytesDart>(
          'unixsock_pending_write_bytes');
  late final _FreeStringDart _freeString =
      _library.lookupFunction<_FreeStringNative, _FreeStringDart>(
          'unixsock_free_string');

  bool _initialized = false;

  @override
  void ensureInitialized() {
    if (_initialized) {
      return;
    }
    final int result = _initialize(NativeApi.initializeApiDLData);
    if (result != 0) {
      throw StateError(
          'Failed to initialize the native Dart API bridge: $result');
    }
    _initialized = true;
  }

  @override
  int connect({
    required String path,
    required SendPort sendPort,
    Duration? timeout,
  }) {
    ensureInitialized();
    return _invoke('connect', (errorCode, errorMessage) {
      final Pointer<Utf8> pathPointer = path.toNativeUtf8();
      try {
        return _connect(
          pathPointer,
          sendPort.nativePort,
          timeout?.inMilliseconds ?? 0,
          errorCode,
          errorMessage,
        );
      } finally {
        calloc.free(pathPointer);
      }
    }, expectedNonNegative: true);
  }

  @override
  int bindServer({
    required String path,
    required SendPort sendPort,
    int backlog = 0,
  }) {
    ensureInitialized();
    return _invoke('bindServer', (errorCode, errorMessage) {
      final Pointer<Utf8> pathPointer = path.toNativeUtf8();
      try {
        return _bindServer(
          pathPointer,
          sendPort.nativePort,
          backlog,
          errorCode,
          errorMessage,
        );
      } finally {
        calloc.free(pathPointer);
      }
    }, expectedNonNegative: true);
  }

  @override
  void write(int handle, Uint8List bytes) {
    _invoke('write', (errorCode, errorMessage) {
      final Pointer<Uint8> pointer = calloc<Uint8>(bytes.length);
      try {
        pointer.asTypedList(bytes.length).setAll(0, bytes);
        return _write(handle, pointer, bytes.length, errorCode, errorMessage);
      } finally {
        calloc.free(pointer);
      }
    });
  }

  @override
  void shutdownWrite(int handle) {
    _invoke(
        'shutdownWrite',
        (errorCode, errorMessage) =>
            _shutdownWrite(handle, errorCode, errorMessage));
  }

  @override
  void destroy(int handle) {
    _invoke('destroy', (errorCode, errorMessage) {
      return _destroy(handle, errorCode, errorMessage);
    }, ignoreErrors: true);
  }

  @override
  bool setOption(int handle, SocketOption option, bool enabled) {
    final int optionCode;
    if (identical(option, SocketOption.tcpNoDelay)) {
      optionCode = 1;
    } else {
      throw UnsupportedError('Unsupported socket option: $option');
    }
    return _invoke(
            'setOption',
            (errorCode, errorMessage) => _setOption(handle, optionCode,
                enabled ? 1 : 0, errorCode, errorMessage)) ==
        1;
  }

  @override
  Uint8List getRawOption(int handle, RawSocketOption option) {
    final Pointer<Uint8> value = calloc<Uint8>(option.value.length);
    final Pointer<Int32> actualLength = calloc<Int32>();
    try {
      final int status = _invoke('getRawOption', (errorCode, errorMessage) {
        return _getRawOption(
          handle,
          option.level,
          option.option,
          value,
          option.value.length,
          actualLength,
          errorCode,
          errorMessage,
        );
      });
      if (status != 0) {
        throw StateError('Unexpected getRawOption status: $status');
      }
      return Uint8List.fromList(value.asTypedList(actualLength.value));
    } finally {
      calloc.free(value);
      calloc.free(actualLength);
    }
  }

  @override
  void setRawOption(int handle, RawSocketOption option) {
    final Pointer<Uint8> value = calloc<Uint8>(option.value.length);
    try {
      value.asTypedList(option.value.length).setAll(0, option.value);
      _invoke('setRawOption', (errorCode, errorMessage) {
        return _setRawOption(
          handle,
          option.level,
          option.option,
          value,
          option.value.length,
          errorCode,
          errorMessage,
        );
      });
    } finally {
      calloc.free(value);
    }
  }

  @override
  int pendingWriteBytes(int handle) {
    return _invoke(
        'pendingWriteBytes',
        (errorCode, errorMessage) =>
            _pendingWriteBytes(handle, errorCode, errorMessage));
  }

  int _invoke(
    String operation,
    int Function(Pointer<Int32>, Pointer<Pointer<Char>>) body, {
    bool expectedNonNegative = false,
    bool ignoreErrors = false,
  }) {
    final Pointer<Int32> errorCode = calloc<Int32>();
    final Pointer<Pointer<Char>> errorMessage = calloc<Pointer<Char>>();
    try {
      final int result = body(errorCode, errorMessage);
      final Pointer<Char> errorPointer = errorMessage.value;
      final bool hasError = errorPointer.address != 0 || errorCode.value != 0;
      if (hasError && !ignoreErrors) {
        throw _socketException(
          operation,
          errorCode.value,
          errorPointer.address == 0
              ? '$operation failed'
              : errorPointer.cast<Utf8>().toDartString(),
        );
      }
      if (!hasError && expectedNonNegative && result < 0) {
        throw _socketException(
            operation, 0, '$operation returned an invalid handle');
      }
      return result;
    } finally {
      final Pointer<Char> errorPointer = errorMessage.value;
      if (errorPointer.address != 0) {
        _freeString(errorPointer);
      }
      calloc.free(errorCode);
      calloc.free(errorMessage);
    }
  }

  SocketException _socketException(String operation, int code, String message) {
    return SocketException(
      'unixsock $operation failed: $message',
      osError: code == 0 ? null : OSError(message, code),
    );
  }
}
