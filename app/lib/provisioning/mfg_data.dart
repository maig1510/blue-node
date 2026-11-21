import 'dart:typed_data';

/// What a node in pairing mode advertises about itself.
///
/// Layout (firmware components/provisioning/provisioning.c), after the 2-byte
/// company id that Android has already stripped off:
///
///     'B' 'N' | format version | flags | MAC (6 bytes)
///
/// It is there so the scan list can tell "already added" apart from "new"
/// before connecting, without a round trip per node.
class NodeAdvert {
  const NodeAdvert({required this.mac, required this.provisioned});

  /// Full MAC in colon notation, upper case: `A4:C1:F8:2B:70:C8`.
  final String mac;

  /// The node already holds WiFi and broker settings, i.e. it is being
  /// re-paired (new WiFi) rather than set up for the first time.
  final bool provisioned;

  /// The MAC as the backend's node_id: 12 hex digits, no separators.
  String get nodeId => mac.replaceAll(':', '');

  static const _formatVersion = 1;
  static const _flagProvisioned = 0x01;
  static const _length = 10;

  /// Parse manufacturer data, or null if it is not a blue-node advertisement
  /// of a format this app understands.
  static NodeAdvert? tryParse(Uint8List? data) {
    if (data == null || data.length < _length) return null;
    if (data[0] != 0x42 || data[1] != 0x4E) return null; // "BN"
    if (data[2] != _formatVersion) return null;

    final mac = data
        .sublist(4, 10)
        .map((b) => b.toRadixString(16).padLeft(2, '0').toUpperCase())
        .join(':');
    return NodeAdvert(mac: mac, provisioned: data[3] & _flagProvisioned != 0);
  }
}
