import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:unixsock_plugin/src/native_bindings.dart';
import 'package:unixsock_plugin/src/native_protocol.dart';
import 'package:unixsock_plugin/unixsock_plugin.dart';

void main() {
  late _FakeUnixsockBinding binding;

  setUp(() {
    binding = _FakeUnixsockBinding();
    UnixsockBindings.debugOverride = binding;
    UnixSocket.debugPlatformIsIos = true;
  });

  tearDown(() {
    UnixsockBindings.debugOverride = null;
    UnixSocket.debugPlatformIsIos = null;
  });

  test('connect completes after native connected event and streams bytes',
      () async {
    final Socket socket = await UnixSocket.connect('/tmp/unixsock.sock', 0);
    expect(socket.remoteAddress.address, '/tmp/unixsock.sock');
    expect(socket.port, 0);
    expect(socket.remotePort, 0);

    final Future<Uint8List> firstChunk = socket.first;
    binding.emitData(1, Uint8List.fromList(utf8.encode('pong')));
    expect(utf8.decode(await firstChunk), 'pong');

    binding.emitReadClosed(1);
    binding.emitClosed(1);
    await socket.done;
  });

  test('flush waits for the native queue to drain and close shuts down writes',
      () async {
    final Socket socket = await UnixSocket.connect('/tmp/unixsock.sock', 0);

    socket.add(utf8.encode('ping'));
    expect(binding.writes.single,
        _WriteCall(handle: 1, bytes: utf8.encode('ping')));

    bool flushed = false;
    final Future<void> flushFuture = socket.flush().then((_) {
      flushed = true;
    });

    await Future<void>.delayed(Duration.zero);
    expect(flushed, isFalse);

    binding.drain(1);
    await flushFuture;
    expect(flushed, isTrue);

    final Future<void> closeFuture = socket.close();
    expect(binding.shutdownWriteHandles, [1]);
    binding.drain(1);
    await closeFuture;

    binding.emitClosed(1);
    await socket.done;
  });

  test('setOption/getRawOption/setRawOption delegate to the binding', () async {
    final Socket socket = await UnixSocket.connect('/tmp/unixsock.sock', 0);

    expect(socket.setOption(SocketOption.tcpNoDelay, true), isTrue);

    binding.rawOptionValue = Uint8List.fromList([1, 0, 0, 0]);
    final Uint8List value =
        socket.getRawOption(RawSocketOption(1, 2, Uint8List(4)));
    expect(value, Uint8List.fromList([1, 0, 0, 0]));

    socket.setRawOption(RawSocketOption(1, 2, Uint8List.fromList([7, 8, 9])));
    expect(binding.setRawOptionCalls.single, Uint8List.fromList([7, 8, 9]));

    socket.destroy();
    binding.emitClosed(1);
    await socket.done;
  });

  test('server accepts native connections and yields accepted sockets',
      () async {
    final UnixServerSocket server =
        await UnixServerSocket.bind('/tmp/unixsock_server.sock', 0, backlog: 8);
    expect(server.address.address, '/tmp/unixsock_server.sock');
    expect(server.port, 0);

    final Future<Socket> acceptedFuture = server.first;
    final int acceptedHandle =
        binding.accept(serverHandle: 1, path: '/tmp/unixsock_server.sock');
    final Socket accepted = await acceptedFuture;

    expect(accepted.address.address, '/tmp/unixsock_server.sock');
    expect(accepted.remoteAddress.address, '');

    accepted.add(utf8.encode('reply'));
    expect(
      binding.writes.last,
      _WriteCall(handle: acceptedHandle, bytes: utf8.encode('reply')),
    );

    final Future<Uint8List> firstChunk = accepted.first;
    binding.emitData(acceptedHandle, Uint8List.fromList(utf8.encode('hello')));
    expect(utf8.decode(await firstChunk), 'hello');

    final Future<UnixServerSocket> closeFuture = server.close();
    expect(binding.destroyedHandles, contains(1));
    binding.emitClosed(1);
    await closeFuture;

    binding.emitReadClosed(acceptedHandle);
    binding.emitClosed(acceptedHandle);
    await accepted.done;
  });
}

final class _FakeUnixsockBinding implements UnixsockBinding {
  final Map<int, SendPort> _ports = <int, SendPort>{};
  final Map<int, int> _pendingBytes = <int, int>{};
  final List<_WriteCall> writes = <_WriteCall>[];
  final List<int> shutdownWriteHandles = <int>[];
  final List<int> destroyedHandles = <int>[];
  final List<Uint8List> setRawOptionCalls = <Uint8List>[];

  Uint8List rawOptionValue = Uint8List(4);
  int _nextHandle = 1;

  int accept(
      {required int serverHandle,
      required String path,
      String remotePath = ''}) {
    final int handle = _nextHandle++;
    _ports[handle] = _ports[serverHandle]!;
    _pendingBytes[handle] = 0;
    _emit(serverHandle, <Object?>[
      unixsockEventAccepted,
      serverHandle,
      handle,
      path,
      remotePath,
    ]);
    return handle;
  }

  @override
  int bindServer({
    required String path,
    required SendPort sendPort,
    int backlog = 0,
  }) {
    final int handle = _nextHandle++;
    _ports[handle] = sendPort;
    _pendingBytes[handle] = 0;
    return handle;
  }

  @override
  int connect({
    required String path,
    required SendPort sendPort,
    Duration? timeout,
  }) {
    final int handle = _nextHandle++;
    _ports[handle] = sendPort;
    _pendingBytes[handle] = 0;
    scheduleMicrotask(() {
      sendPort.send(<Object?>[unixsockEventConnected, handle]);
    });
    return handle;
  }

  @override
  void destroy(int handle) {
    destroyedHandles.add(handle);
  }

  void drain(int handle) {
    _pendingBytes[handle] = 0;
    _emit(handle, <Object?>[unixsockEventDrained, handle]);
  }

  void emitClosed(int handle) {
    _emit(handle, <Object?>[unixsockEventClosed, handle]);
  }

  void emitData(int handle, Uint8List data) {
    _emit(handle, <Object?>[unixsockEventData, handle, data]);
  }

  void emitReadClosed(int handle) {
    _emit(handle, <Object?>[unixsockEventReadClosed, handle]);
  }

  @override
  void ensureInitialized() {}

  @override
  Uint8List getRawOption(int handle, RawSocketOption option) {
    return Uint8List.fromList(rawOptionValue);
  }

  @override
  int pendingWriteBytes(int handle) {
    return _pendingBytes[handle] ?? 0;
  }

  @override
  bool setOption(int handle, SocketOption option, bool enabled) {
    return true;
  }

  @override
  void setRawOption(int handle, RawSocketOption option) {
    setRawOptionCalls.add(Uint8List.fromList(option.value));
  }

  @override
  void shutdownWrite(int handle) {
    shutdownWriteHandles.add(handle);
  }

  @override
  void write(int handle, Uint8List bytes) {
    writes.add(_WriteCall(handle: handle, bytes: bytes));
    _pendingBytes[handle] = (_pendingBytes[handle] ?? 0) + bytes.length;
  }

  void _emit(int handle, List<Object?> event) {
    _ports[handle]?.send(event);
  }
}

final class _WriteCall {
  const _WriteCall({
    required this.handle,
    required this.bytes,
  });

  final int handle;
  final List<int> bytes;

  @override
  bool operator ==(Object other) {
    return other is _WriteCall &&
        other.handle == handle &&
        _listEquals(other.bytes, bytes);
  }

  @override
  int get hashCode => Object.hash(handle, Object.hashAll(bytes));

  @override
  String toString() => '_WriteCall(handle: $handle, bytes: $bytes)';
}

bool _listEquals(List<int> left, List<int> right) {
  if (left.length != right.length) {
    return false;
  }
  for (int index = 0; index < left.length; index += 1) {
    if (left[index] != right[index]) {
      return false;
    }
  }
  return true;
}
