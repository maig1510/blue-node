import 'dart:async';

import 'package:flutter/services.dart';

import 'mfg_data.dart';
import 'provisioning_transport.dart';

/// [ProvisioningTransport] over the platform channel to BleProvisioningPlugin
/// (android/app/src/main/kotlin/.../BleProvisioningPlugin.kt).
class EspBleTransport implements ProvisioningTransport {
  EspBleTransport() {
    _methods.setMethodCallHandler((call) async {
      if (call.method == 'onDisconnected') _disconnects.add(null);
    });
  }

  static const _methods = MethodChannel('blue_node/provisioning');
  static const _scanEvents = EventChannel('blue_node/provisioning/scan');

  /// Advertised name prefix of a node in pairing mode.
  static const namePrefix = 'BN-';

  final _disconnects = StreamController<void>.broadcast();

  @override
  Stream<void> get disconnects => _disconnects.stream;

  @override
  Future<BleReadiness> ensureReady() async {
    final state = await _call<String>('ensureReady');
    return switch (state) {
      'ok' => BleReadiness.ok,
      'bluetooth_off' => BleReadiness.bluetoothOff,
      'unsupported' => BleReadiness.unsupported,
      _ => BleReadiness.denied,
    };
  }

  @override
  Stream<DiscoveredNode> scan() {
    return _scanEvents.receiveBroadcastStream(namePrefix).map((event) {
      final map = event as Map;
      return DiscoveredNode(
        address: map['address'] as String,
        name: map['name'] as String,
        rssi: map['rssi'] as int,
        advert: NodeAdvert.tryParse(map['mfg'] as Uint8List?),
      );
    });
  }

  @override
  Future<void> connect(String address) =>
      _call<void>('connect', {'address': address});

  @override
  Future<String> request(String endpoint, String request) async =>
      await _call<String>('sendCustom', {
        'endpoint': endpoint,
        'data': request,
      }) ??
      '';

  @override
  Future<List<WifiNetwork>> scanWifi() async {
    final list = await _call<List<Object?>>('scanWifi') ?? const [];
    return list
        .cast<Map>()
        .map(
          (map) => WifiNetwork(
            ssid: map['ssid'] as String,
            rssi: map['rssi'] as int,
            secured: map['secured'] as bool,
          ),
        )
        .toList();
  }

  @override
  Future<void> disconnect() => _call<void>('disconnect');

  /// Release the channel handler; the app-lifetime provider calls this.
  void dispose() {
    _methods.setMethodCallHandler(null);
    _disconnects.close();
  }

  /// Invoke a platform method, turning its errors into ProvisioningException.
  Future<T?> _call<T>(String method, [Map<String, Object?>? args]) async {
    try {
      return await _methods.invokeMethod<T>(method, args);
    } on PlatformException catch (e) {
      throw ProvisioningException(e.message ?? e.code, code: e.code);
    }
  }
}
