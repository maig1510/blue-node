/// The phone's side of the BLE link to a node in pairing mode.
///
/// An interface so that the onboarding flow (OnboardingController) can be
/// driven by a fake in tests; the real implementation is EspBleTransport,
/// which goes through a platform channel to Espressif's Android library.
library;

import 'dart:convert';

import 'mfg_data.dart';

/// A node seen advertising while in pairing mode.
class DiscoveredNode {
  const DiscoveredNode({
    required this.address,
    required this.name,
    required this.rssi,
    this.advert,
  });

  /// Bluetooth address, the handle [ProvisioningTransport.connect] takes.
  final String address;

  /// Advertised name, "BN-" followed by the low three MAC bytes.
  final String name;

  /// Signal strength in dBm; closer nodes are less negative.
  final int rssi;

  /// What the node says about itself in its manufacturer data, or null when
  /// the advertisement carried none we recognise (older firmware).
  final NodeAdvert? advert;
}

/// One WiFi network the node can see, from the standard prov-scan endpoint.
///
/// The node reports one entry per access point, so a network served by
/// several (mesh, repeaters) shows up more than once; see [normalizeNetworks].
class WifiNetwork {
  const WifiNetwork({
    required this.ssid,
    required this.rssi,
    required this.secured,
  });

  final String ssid;
  final int rssi;

  /// False for an open network, which needs no password.
  final bool secured;
}

/// The scan as a list to choose from: one entry per SSID (its strongest access
/// point), hidden networks left out (they are typed in by hand), strongest
/// first.
List<WifiNetwork> normalizeNetworks(Iterable<WifiNetwork> networks) {
  final strongest = <String, WifiNetwork>{};
  for (final network in networks) {
    if (network.ssid.isEmpty) continue;
    final known = strongest[network.ssid];
    if (known == null || network.rssi > known.rssi) {
      strongest[network.ssid] = network;
    }
  }
  return strongest.values.toList()..sort((a, b) => b.rssi.compareTo(a.rssi));
}

/// Whether [ssid] fits the 802.11 limit of 32 bytes (UTF-8, as it goes over
/// the air) and is not empty. The node enforces the same.
bool isValidSsid(String ssid) {
  final length = utf8.encode(ssid).length;
  return length > 0 && length <= 32;
}

/// Whether [password] can secure a WiFi network: an 8-63 byte WPA passphrase
/// or a raw 64-digit hex key. Empty (an open network) is not covered here.
///
/// Counted in UTF-8 bytes, as the node and the WiFi standard count them, not
/// in characters: "č" is two bytes.
bool isValidWifiPassword(String password) {
  final length = utf8.encode(password).length;
  if (length >= 8 && length <= 63) return true;
  return length == 64 && RegExp(r'^[0-9a-fA-F]{64}$').hasMatch(password);
}

/// Why the phone cannot do BLE right now.
enum BleReadiness { ok, denied, bluetoothOff, unsupported }

/// The link failed: could not connect, a request timed out, the node went away.
class ProvisioningException implements Exception {
  ProvisioningException(this.message, {this.code});

  final String message;

  /// Machine-readable reason from the platform side, when there is one:
  /// busy, not_connected, timeout, disconnected, request_failed,
  /// wifi_scan_failed, connection_failed, unknown_device.
  final String? code;

  @override
  String toString() => message;
}

abstract class ProvisioningTransport {
  /// Ask for the Bluetooth permissions and check the adapter.
  Future<BleReadiness> ensureReady();

  /// Advertisements of nodes in pairing mode, for as long as it is listened to.
  ///
  /// The same node shows up repeatedly (every advertisement); callers dedupe
  /// by [DiscoveredNode.address].
  Stream<DiscoveredNode> scan();

  /// Open the encrypted session to one node seen by [scan].
  ///
  /// Throws [ProvisioningException] when the node cannot be reached.
  Future<void> connect(String address);

  /// Send [request] to a custom endpoint and return the node's answer.
  ///
  /// The platform side serves one call to the node at a time and refuses an
  /// overlapping one with code "busy"; wrap the transport in SerialTransport
  /// to have calls wait their turn instead. A call always completes: when the
  /// answer is lost, it fails with "timeout" or "disconnected".
  Future<String> request(String endpoint, String request);

  /// Networks the node can see, as it reports them (one entry per access
  /// point). Same one-at-a-time rule as [request].
  Future<List<WifiNetwork>> scanWifi();

  Future<void> disconnect();

  /// Fires when the node drops the connection on its own (e.g. it timed out
  /// waiting for the button confirmation).
  Stream<void> get disconnects;
}
