import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:isolate';
import 'dart:typed_data';

import 'native_bindings.dart';
import 'native_protocol.dart';

typedef _NativeEventHandler = void Function(List<Object?> message);

final class _NativeEventRouter {
  _NativeEventRouter() {
    _subscription = _receivePort.listen(_dispatchMessage);
  }

  final ReceivePort _receivePort = ReceivePort();
  final Map<int, _NativeEventHandler> _handlers = <int, _NativeEventHandler>{};
  final Map<int, List<List<Object?>>> _pendingMessages =
      <int, List<List<Object?>>>{};

  late final StreamSubscription<dynamic> _subscription;

  int _ownerCount = 0;
  bool _disposed = false;

  SendPort get sendPort => _receivePort.sendPort;

  void retain() {
    if (_disposed) {
      throw StateError('The native event router has already been disposed.');
    }
    _ownerCount += 1;
  }

  void register(int handle, _NativeEventHandler handler) {
    _handlers[handle] = handler;
    final List<List<Object?>>? pending = _pendingMessages.remove(handle);
    if (pending == null) {
      return;
    }
    for (final List<Object?> message in pending) {
      handler(message);
    }
  }

  void unregister(int handle) {
    _handlers.remove(handle);
    _pendingMessages.remove(handle);
  }

  Future<void> release() async {
    if (_ownerCount > 0) {
      _ownerCount -= 1;
    }
    if (_ownerCount != 0 || _disposed) {
      return;
    }
    _disposed = true;
    await _subscription.cancel();
    _handlers.clear();
    _pendingMessages.clear();
    _receivePort.close();
  }

  void _dispatchMessage(dynamic message) {
    if (message is! List<Object?> || message.length < 2) {
      return;
    }
    final Object? handleObject = message[1];
    if (handleObject is! int) {
      return;
    }
    final _NativeEventHandler? handler = _handlers[handleObject];
    if (handler != null) {
      handler(message);
      return;
    }
    (_pendingMessages[handleObject] ??= <List<Object?>>[])
        .add(List<Object?>.from(message));
  }
}

final class UnixSocket extends Stream<Uint8List> implements Socket {
  UnixSocket._(
    this._nativeBinding,
    this._router, {
    required String addressPath,
    required String remotePath,
  }) {
    _address = InternetAddress(addressPath, type: InternetAddressType.unix);
    _remoteAddress =
        InternetAddress(remotePath, type: InternetAddressType.unix);
    _sink = IOSink(_UnixSocketConsumer(this));
  }

  static bool? debugPlatformIsIos;

  static UnixsockBinding? get _debugBinding => UnixsockBindings.debugOverride;

  static bool get _isIos => debugPlatformIsIos ?? Platform.isIOS;

  static Future<Socket> connect(
    Object host,
    int port, {
    Object? sourceAddress,
    int sourcePort = 0,
    Duration? timeout,
  }) async {
    _assertSocketArguments(sourceAddress, sourcePort);
    final String path = _normalizeUnixPath(host, port);
    final UnixsockBinding binding = _platformBinding;
    final _NativeEventRouter router = _NativeEventRouter()..retain();
    final UnixSocket socket = UnixSocket._(
      binding,
      router,
      addressPath: path,
      remotePath: path,
    );
    try {
      final int handle = binding.connect(
        path: path,
        sendPort: router.sendPort,
        timeout: timeout,
      );
      socket._attachHandle(handle);
    } catch (_) {
      unawaited(router.release());
      rethrow;
    }
    return socket._connected.future;
  }

  static Future<ConnectionTask<Socket>> startConnect(
    Object host,
    int port, {
    Object? sourceAddress,
    int sourcePort = 0,
  }) async {
    _assertSocketArguments(sourceAddress, sourcePort);
    final String path = _normalizeUnixPath(host, port);
    final UnixsockBinding binding = _platformBinding;
    final _NativeEventRouter router = _NativeEventRouter()..retain();
    final UnixSocket socket = UnixSocket._(
      binding,
      router,
      addressPath: path,
      remotePath: path,
    );
    final Future<Socket> future = () async {
      try {
        final int handle = binding.connect(
          path: path,
          sendPort: router.sendPort,
        );
        socket._attachHandle(handle);
      } catch (_) {
        unawaited(router.release());
        rethrow;
      }
      return socket._connected.future;
    }();
    return ConnectionTask.fromSocket(future, () => socket.destroy());
  }

  static UnixSocket _accepted(
    UnixsockBinding binding,
    _NativeEventRouter router, {
    required int handle,
    required String addressPath,
    required String remotePath,
  }) {
    router.retain();
    final UnixSocket socket = UnixSocket._(
      binding,
      router,
      addressPath: addressPath,
      remotePath: remotePath,
    );
    socket._attachHandle(handle);
    socket._connected.complete(socket);
    return socket;
  }

  static UnixsockBinding get _platformBinding {
    if (!_isIos && _debugBinding == null) {
      throw UnsupportedError('unixsock_plugin only supports iOS.');
    }
    return UnixsockBindings.instance;
  }

  static void _assertSocketArguments(Object? sourceAddress, int sourcePort) {
    if (sourceAddress != null || sourcePort != 0) {
      throw UnsupportedError(
        'Unix domain sockets do not support sourceAddress/sourcePort in this plugin.',
      );
    }
  }

  static String _normalizeUnixPath(Object address, int port) {
    if (port != 0) {
      throw ArgumentError.value(
          port, 'port', 'Unix domain sockets require port 0.');
    }
    if (address is InternetAddress) {
      if (address.type != InternetAddressType.unix) {
        throw ArgumentError.value(
          address,
          'address',
          'Only InternetAddressType.unix is supported.',
        );
      }
      return address.address;
    }
    if (address is String) {
      return address;
    }
    throw ArgumentError.value(
        address, 'address', 'Expected a unix socket path.');
  }

  final UnixsockBinding _nativeBinding;
  final _NativeEventRouter _router;
  final StreamController<Uint8List> _controller =
      StreamController<Uint8List>(sync: true);
  final Completer<Socket> _connected = Completer<Socket>();
  final Completer<void> _doneCompleter = Completer<void>();

  late final IOSink _sink;
  late final InternetAddress _address;
  late final InternetAddress _remoteAddress;

  Completer<void>? _drainCompleter;

  int _handle = 0;
  bool _destroyed = false;
  bool _streamClosed = false;
  bool _transportReleased = false;

  @override
  InternetAddress get address => _address;

  @override
  Encoding get encoding => _sink.encoding;

  @override
  set encoding(Encoding value) {
    _sink.encoding = value;
  }

  @override
  Future<void> addStream(Stream<List<int>> stream) => _sink.addStream(stream);

  @override
  void add(List<int> data) => _sink.add(data);

  @override
  void addError(Object error, [StackTrace? stackTrace]) =>
      _sink.addError(error, stackTrace);

  @override
  Future<void> close() => _sink.close();

  @override
  Future<void> get done => _doneCompleter.future;

  @override
  Future<void> flush() => _sink.flush();

  @override
  void write(Object? object) => _sink.write(object);

  @override
  void writeAll(Iterable<Object?> objects, [String separator = '']) =>
      _sink.writeAll(objects, separator);

  @override
  void writeCharCode(int charCode) => _sink.writeCharCode(charCode);

  @override
  void writeln([Object? object = '']) => _sink.writeln(object);

  @override
  int get port => 0;

  @override
  InternetAddress get remoteAddress => _remoteAddress;

  @override
  int get remotePort => 0;

  @override
  bool setOption(SocketOption option, bool enabled) {
    _ensureHandle();
    return _nativeBinding.setOption(_handle, option, enabled);
  }

  @override
  Uint8List getRawOption(RawSocketOption option) {
    _ensureHandle();
    return _nativeBinding.getRawOption(_handle, option);
  }

  @override
  void setRawOption(RawSocketOption option) {
    _ensureHandle();
    _nativeBinding.setRawOption(_handle, option);
  }

  @override
  void destroy() {
    if (_destroyed || _handle == 0) {
      return;
    }
    _destroyed = true;
    _nativeBinding.destroy(_handle);
  }

  @override
  StreamSubscription<Uint8List> listen(
    void Function(Uint8List event)? onData, {
    Function? onError,
    void Function()? onDone,
    bool? cancelOnError,
  }) {
    return _controller.stream.listen(
      onData,
      onError: onError,
      onDone: onDone,
      cancelOnError: cancelOnError,
    );
  }

  void _attachHandle(int handle) {
    _handle = handle;
    _router.register(handle, _handleNativeMessage);
  }

  void _handleNativeMessage(List<Object?> message) {
    final int type = message[0]! as int;
    switch (type) {
      case unixsockEventConnected:
        if (!_connected.isCompleted) {
          _connected.complete(this);
        }
        break;
      case unixsockEventData:
        if (message.length >= 3 && !_streamClosed) {
          final Uint8List bytes = message[2]! as Uint8List;
          _controller.add(Uint8List.fromList(bytes));
        }
        break;
      case unixsockEventReadClosed:
        _closeInboundStream();
        break;
      case unixsockEventClosed:
        _destroyed = true;
        if (!_connected.isCompleted) {
          _connected.completeError(const SocketException.closed());
        }
        _completeDrain();
        _closeInboundStream();
        _completeDone();
        _releaseTransport();
        break;
      case unixsockEventError:
        final int code = message.length >= 3 ? message[2]! as int : 0;
        final String text =
            message.length >= 4 ? message[3]! as String : 'native socket error';
        final SocketException exception = SocketException(
          text,
          osError: code == 0 ? null : OSError(text, code),
          address: _remoteAddress,
          port: 0,
        );
        if (!_connected.isCompleted) {
          _connected.completeError(exception);
        }
        if (!_streamClosed) {
          _controller.addError(exception);
        }
        if (!_doneCompleter.isCompleted) {
          _doneCompleter.completeError(exception);
        }
        _completeDrain(exception);
        _closeInboundStream();
        _releaseTransport();
        break;
      case unixsockEventDrained:
        _completeDrain();
        break;
    }
  }

  void _writeBytes(List<int> bytes) {
    _ensureHandle();
    if (_destroyed) {
      throw const SocketException.closed();
    }
    final Uint8List data =
        bytes is Uint8List ? bytes : Uint8List.fromList(bytes);
    _nativeBinding.write(_handle, data);
  }

  Future<void> _shutdownWrite() async {
    _ensureHandle();
    if (_destroyed) {
      return;
    }
    _nativeBinding.shutdownWrite(_handle);
    await _awaitDrained();
  }

  Future<void> _awaitDrained() {
    _ensureHandle();
    if (_nativeBinding.pendingWriteBytes(_handle) == 0) {
      return Future<void>.value();
    }
    _drainCompleter ??= Completer<void>();
    if (_nativeBinding.pendingWriteBytes(_handle) == 0 &&
        !_drainCompleter!.isCompleted) {
      _drainCompleter!.complete();
    }
    return _drainCompleter!.future;
  }

  void _ensureHandle() {
    if (_handle == 0) {
      throw const SocketException.closed();
    }
  }

  void _closeInboundStream() {
    if (_streamClosed) {
      return;
    }
    _streamClosed = true;
    unawaited(_controller.close());
  }

  void _completeDone() {
    if (!_doneCompleter.isCompleted) {
      _doneCompleter.complete();
    }
  }

  void _completeDrain([Object? error, StackTrace? stackTrace]) {
    final Completer<void>? completer = _drainCompleter;
    if (completer == null || completer.isCompleted) {
      _drainCompleter = null;
      return;
    }
    if (error == null) {
      completer.complete();
    } else {
      completer.completeError(error, stackTrace);
    }
    _drainCompleter = null;
  }

  void _releaseTransport() {
    if (_transportReleased) {
      return;
    }
    _transportReleased = true;
    if (_handle != 0) {
      _router.unregister(_handle);
    }
    unawaited(_router.release());
  }
}

final class UnixServerSocket extends Stream<Socket> implements ServerSocket {
  UnixServerSocket._(
    this._nativeBinding,
    this._router,
    this._path,
  ) {
    _address = InternetAddress(_path, type: InternetAddressType.unix);
  }

  static Future<UnixServerSocket> bind(
    Object address,
    int port, {
    int backlog = 0,
    bool v6Only = false,
    bool shared = false,
  }) async {
    if (v6Only) {
      throw UnsupportedError('Unix domain sockets do not support v6Only.');
    }
    if (shared) {
      throw UnsupportedError(
          'Unix domain sockets do not support shared listeners.');
    }
    final String path = UnixSocket._normalizeUnixPath(address, port);
    final UnixsockBinding binding = UnixSocket._platformBinding;
    final _NativeEventRouter router = _NativeEventRouter()..retain();
    final UnixServerSocket server = UnixServerSocket._(binding, router, path);
    try {
      final int handle = binding.bindServer(
        path: path,
        sendPort: router.sendPort,
        backlog: backlog,
      );
      server._attachHandle(handle);
    } catch (_) {
      unawaited(router.release());
      rethrow;
    }
    return server;
  }

  final UnixsockBinding _nativeBinding;
  final _NativeEventRouter _router;
  final String _path;
  final StreamController<Socket> _controller =
      StreamController<Socket>(sync: true);
  final Completer<UnixServerSocket> _closed = Completer<UnixServerSocket>();

  late final InternetAddress _address;

  int _handle = 0;
  bool _closedLocally = false;
  bool _transportReleased = false;

  @override
  InternetAddress get address => _address;

  @override
  int get port => 0;

  @override
  Future<UnixServerSocket> close() {
    if (_handle == 0 || _closedLocally) {
      return _closed.future;
    }
    _closedLocally = true;
    _nativeBinding.destroy(_handle);
    return _closed.future;
  }

  @override
  StreamSubscription<Socket> listen(
    void Function(Socket event)? onData, {
    Function? onError,
    void Function()? onDone,
    bool? cancelOnError,
  }) {
    return _controller.stream.listen(
      onData,
      onError: onError,
      onDone: onDone,
      cancelOnError: cancelOnError,
    );
  }

  void _attachHandle(int handle) {
    _handle = handle;
    _router.register(handle, _handleNativeMessage);
  }

  void _handleNativeMessage(List<Object?> message) {
    final int type = message[0]! as int;
    switch (type) {
      case unixsockEventAccepted:
        final int clientHandle = message[2]! as int;
        final String addressPath =
            message.length >= 4 ? message[3]! as String : _path;
        final String remotePath =
            message.length >= 5 ? message[4]! as String : '';
        final UnixSocket socket = UnixSocket._accepted(
          _nativeBinding,
          _router,
          handle: clientHandle,
          addressPath: addressPath,
          remotePath: remotePath,
        );
        _controller.add(socket);
        break;
      case unixsockEventError:
        final int code = message.length >= 3 ? message[2]! as int : 0;
        final String text =
            message.length >= 4 ? message[3]! as String : 'native server error';
        _controller.addError(SocketException(
          text,
          osError: code == 0 ? null : OSError(text, code),
          address: _address,
          port: 0,
        ));
        break;
      case unixsockEventClosed:
        _controller.close();
        _releaseTransport();
        if (!_closed.isCompleted) {
          _closed.complete(this);
        }
        break;
    }
  }

  void _releaseTransport() {
    if (_transportReleased) {
      return;
    }
    _transportReleased = true;
    if (_handle != 0) {
      _router.unregister(_handle);
    }
    unawaited(_router.release());
  }
}

final class _UnixSocketConsumer implements StreamConsumer<List<int>> {
  const _UnixSocketConsumer(this._socket);

  final UnixSocket _socket;

  @override
  Future<void> addStream(Stream<List<int>> stream) async {
    await for (final List<int> chunk in stream) {
      _socket._writeBytes(chunk);
    }
    await _socket._awaitDrained();
  }

  @override
  Future<void> close() => _socket._shutdownWrite();
}
