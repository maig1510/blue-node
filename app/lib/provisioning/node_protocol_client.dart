import 'dart:convert';

import 'provisioning_transport.dart';

/// Typed calls to the node's bn-* endpoints during pairing.
///
/// The protocol is defined by the firmware (components/provisioning/include/
/// provisioning.h); this is its only implementation on the phone. Every
/// request and response is a small JSON object.
class NodeProtocolClient {
  NodeProtocolClient(this._transport);

  final ProvisioningTransport _transport;

  /// Ask whether the user has confirmed this connection on the node.
  ///
  /// The first call is what makes the node start waiting for the button, so
  /// call it as soon as the session is up, then poll.
  Future<bool> confirm() async {
    final json = await _call('bn-confirm');
    return json['state'] == 'confirmed';
  }

  Future<NodeInfo> info() async => NodeInfo.fromJson(await _call('bn-info'));

  /// Hand over WiFi credentials; the node starts testing them in the
  /// background - poll [status] for the outcome.
  Future<void> sendWifi(String ssid, String password) =>
      _expectOk(_call('bn-wifi', {'ssid': ssid, 'password': password}));

  Future<WifiStatus> status() async =>
      WifiStatus.fromJson(await _call('bn-status'));

  /// The last step: the broker address. The node stores everything and
  /// restarts into normal operation a moment later.
  Future<void> sendCloud(String brokerUri) =>
      _expectOk(_call('bn-cloud', {'broker_uri': brokerUri}));

  Future<Map<String, dynamic>> _call(
    String endpoint, [
    Map<String, Object?> body = const {},
  ]) async {
    final raw = await _transport.request(endpoint, jsonEncode(body));
    final Object? decoded;
    try {
      decoded = jsonDecode(raw);
    } on FormatException {
      throw NodeProtocolException('bad_response', endpoint);
    }
    if (decoded is! Map<String, dynamic>) {
      throw NodeProtocolException('bad_response', endpoint);
    }
    // Every endpoint but bn-confirm refuses until the button was pressed.
    if (decoded['ok'] == false) {
      throw NodeProtocolException(
        decoded['error'] as String? ?? 'unknown',
        endpoint,
      );
    }
    return decoded;
  }

  Future<void> _expectOk(Future<Map<String, dynamic>> call) async {
    final json = await call;
    if (json['ok'] != true) {
      throw NodeProtocolException('bad_response', 'unknown');
    }
  }
}

/// The node refused a request, or answered something unreadable.
class NodeProtocolException implements Exception {
  NodeProtocolException(this.error, this.endpoint);

  /// The node's error code: not_confirmed, invalid, busy, wifi_not_connected,
  /// invalid_uri, storage - or bad_response for an unreadable answer.
  final String error;
  final String endpoint;

  @override
  String toString() => '$endpoint: $error';
}

/// What bn-info reports.
class NodeInfo {
  const NodeInfo({
    required this.protocolVersion,
    required this.mac,
    required this.nodeId,
    required this.firmware,
    required this.hardware,
    required this.provisioned,
  });

  final int protocolVersion;

  /// Colon notation, as the backend's POST /devices accepts it.
  final String mac;

  /// The same MAC as 12 hex digits, as it appears in MQTT topics.
  final String nodeId;
  final String firmware;
  final String hardware;

  /// The node already has a configuration: this is a re-pairing.
  final bool provisioned;

  factory NodeInfo.fromJson(Map<String, dynamic> json) => NodeInfo(
    protocolVersion: json['v'] as int,
    mac: json['mac'] as String,
    nodeId: json['node_id'] as String,
    firmware: json['fw'] as String,
    hardware: json['hw'] as String? ?? '',
    provisioned: json['provisioned'] as bool,
  );
}

enum WifiTestState { idle, connecting, connected, failed }

/// Why a WiFi test failed, as the node classifies it.
enum WifiFailure { auth, noAp, timeout, other }

/// What bn-status reports about the WiFi test.
class WifiStatus {
  const WifiStatus({required this.state, this.failure, this.ip});

  final WifiTestState state;
  final WifiFailure? failure;
  final String? ip;

  factory WifiStatus.fromJson(Map<String, dynamic> json) => WifiStatus(
    state: switch (json['wifi']) {
      'connecting' => WifiTestState.connecting,
      'connected' => WifiTestState.connected,
      'failed' => WifiTestState.failed,
      _ => WifiTestState.idle,
    },
    failure: switch (json['reason']) {
      null => null,
      'auth' => WifiFailure.auth,
      'no_ap' => WifiFailure.noAp,
      'timeout' => WifiFailure.timeout,
      _ => WifiFailure.other,
    },
    ip: json['ip'] as String?,
  );
}
