import 'dart:async';

import 'provisioning_transport.dart';

/// A [ProvisioningTransport] whose calls to the node run one at a time.
///
/// The platform side serves a single request at a time (Espressif's library
/// keeps one response listener per device) and refuses an overlapping one
/// with "busy". Overlaps happen in normal use - refreshing the WiFi list and
/// tapping "connect" a moment later - and failing the second call there would
/// end the whole pairing. Queued, it simply waits for the first.
///
/// Only [request] and [scanWifi] are queued. [connect] and [disconnect] are
/// not: disconnect is how a call that never answers gets cut short (the
/// platform side fails the pending call when the link goes), so it must not
/// wait behind it. Every queued call completes, because the platform side
/// times out a lost answer.
class SerialTransport implements ProvisioningTransport {
  SerialTransport(this._inner);

  final ProvisioningTransport _inner;

  /// Completes when the last queued call has; never with an error.
  Future<void> _tail = Future<void>.value();

  Future<T> _queued<T>(Future<T> Function() call) {
    final result = _tail.then((_) => call());
    _tail = result.then<void>((_) {}, onError: (Object _) {});
    return result;
  }

  @override
  Future<String> request(String endpoint, String request) =>
      _queued(() => _inner.request(endpoint, request));

  @override
  Future<List<WifiNetwork>> scanWifi() => _queued(_inner.scanWifi);

  @override
  Future<BleReadiness> ensureReady() => _inner.ensureReady();

  @override
  Stream<DiscoveredNode> scan() => _inner.scan();

  @override
  Future<void> connect(String address) => _inner.connect(address);

  @override
  Future<void> disconnect() => _inner.disconnect();

  @override
  Stream<void> get disconnects => _inner.disconnects;
}
