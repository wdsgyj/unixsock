# unixsock_plugin

`unixsock_plugin` is an iOS-only Flutter FFI plugin that exposes Unix domain
socket client and server capabilities.

It provides:

- `UnixSocket`, which implements Dart's standard `Socket` interface.
- `UnixServerSocket`, which implements Dart's standard `ServerSocket` interface.

The native implementation lives in C/Objective-C on iOS and uses `kqueue` for
event-driven async I/O. No `MethodChannel` is used.

## Features

- iOS-only Flutter plugin built with `dart:ffi`
- Unix domain socket client support
- Unix domain socket server support
- Accepted server-side connections are also exposed as standard `Socket`
  objects
- Native `kqueue` polling thread is fully hidden from Dart
- Native polling thread starts on demand and stops automatically when no socket
  handles remain
- `Socket.setOption`, `Socket.getRawOption`, and `Socket.setRawOption` support
- No TLS layer

## Platform Support

Supported platform:

- iOS

Unsupported platforms:

- Android
- macOS
- Linux
- Windows
- Web

On non-iOS platforms the package throws `UnsupportedError`.

## Requirements

- Flutter `>= 3.3.0`
- Dart `^3.5.4`
- iOS 12.0+

## Installation

Add the package to `pubspec.yaml`:

```yaml
dependencies:
  unixsock_plugin:
    path: ../unixsock_plugin
```

Then fetch dependencies:

```bash
flutter pub get
```

## Public API

The package exports:

```dart
import 'package:unixsock_plugin/unixsock_plugin.dart';
```

Available classes:

- `UnixSocket`
- `UnixServerSocket`

## Usage

### Client

Use `UnixSocket.connect` the same way you would use `Socket.connect`, except
the address must be a Unix socket path and the port must be `0`.

```dart
import 'dart:convert';
import 'dart:io';

import 'package:unixsock_plugin/unixsock_plugin.dart';

Future<void> main() async {
  final Socket socket = await UnixSocket.connect('/tmp/demo.sock', 0);

  socket.write('ping\n');
  await socket.flush();

  await for (final Uint8List chunk in socket) {
    stdout.write(utf8.decode(chunk));
    break;
  }

  await socket.close();
  await socket.done;
}
```

### Server

Use `UnixServerSocket.bind` the same way you would use `ServerSocket.bind`,
except the address must be a Unix socket path and the port must be `0`.

```dart
import 'dart:convert';
import 'dart:io';

import 'package:unixsock_plugin/unixsock_plugin.dart';

Future<void> main() async {
  final UnixServerSocket server = await UnixServerSocket.bind(
    '/tmp/demo.sock',
    0,
    backlog: 16,
  );

  await for (final Socket client in server) {
    client.listen((Uint8List data) async {
      final String text = utf8.decode(data);
      stdout.write('received: $text');
      client.add(utf8.encode('ack:$text'));
      await client.flush();
    });
  }
}
```

### With `InternetAddressType.unix`

You can also pass a Unix `InternetAddress`:

```dart
final address = InternetAddress(
  '/tmp/demo.sock',
  type: InternetAddressType.unix,
);

final socket = await UnixSocket.connect(address, 0);
final server = await UnixServerSocket.bind(address, 0);
```

## Behavioral Notes

- Unix socket paths must be passed as `String` or `InternetAddress` with
  `InternetAddressType.unix`.
- `port` must always be `0`.
- `sourceAddress` and `sourcePort` are not supported for `UnixSocket.connect`.
- `v6Only` and `shared` are not supported for `UnixServerSocket.bind`.
- `remotePort` and `port` on client sockets are always `0`.
- `port` on server sockets is always `0`.
- `Socket.close()` closes the write side and waits for pending writes to drain.
- `Socket.destroy()` destroys the native handle immediately.
- `UnixServerSocket.close()` closes the listening socket. Accepted client
  sockets continue to live independently until they are closed or destroyed.

## Native Architecture

The iOS implementation is in [`ios/Classes`](ios/Classes).

Key points:

- `dart:ffi` calls directly into exported C symbols.
- Native socket state and event dispatch are implemented in C.
- `kqueue` is used for:
  - connect completion
  - readable events
  - writable events
  - accept events on server sockets
  - connect timeout timers
- The polling thread is native-only and invisible to Dart.
- The polling thread is started lazily and torn down automatically when the
  last native socket handle is released.
- Objective-C is only used for symbol retention at link time.

Important native files:

- [`ios/Classes/unixsock_core.c`](ios/Classes/unixsock_core.c)
- [`ios/Classes/unixsock_ffi.c`](ios/Classes/unixsock_ffi.c)
- [`ios/Classes/UnixsockRetention.m`](ios/Classes/UnixsockRetention.m)

## Testing

Package-level analysis and Dart tests:

```bash
dart analyze
flutter test
```

Native C harness:

```bash
clang -Wall -Wextra -std=c11 -pthread \
  -Iios/Classes -Iios/Classes/include -Iios/Classes/internal \
  -o /tmp/unixsock_core_test \
  ios/Classes/unixsock_core.c \
  ios/tests/unixsock_core_test.c

/tmp/unixsock_core_test
```

Example iOS build verification:

```bash
cd example
flutter build ios --simulator --debug --no-codesign
```

## Example App

The example app is intentionally minimal. See
[`example/lib/main.dart`](example/lib/main.dart).

## Limitations

- iOS only
- No TLS support
- No `MethodChannel` fallback
- No non-Unix socket support
- No abstract namespace Unix sockets
- No Dart-side server implementation; all socket state lives in native code
