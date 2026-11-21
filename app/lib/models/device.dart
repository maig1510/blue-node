/// A node as the API returns it from /devices.
///
/// Rows are created by onboarding (POST /devices, see OnboardingController),
/// before the node ever publishes; firmware, online state and last-seen then
/// arrive on their own through ingest. Only [name], [tankId] and
/// [sensorOffsetMm] are set by a human.
class Device {
  const Device({
    required this.id,
    required this.nodeId,
    required this.macAddress,
    required this.name,
    required this.tankId,
    required this.sensorOffsetMm,
    required this.firmware,
    required this.isOnline,
    required this.createdAt,
    required this.lastSeenAt,
  });

  final int id;

  /// Technical identifier from the MQTT topic: the full MAC as 12 hex digits,
  /// e.g. `A4C1F82B70C8`.
  final String nodeId;

  /// Full MAC in colon notation. Always present for a device registered by
  /// onboarding; nullable only for rows from before registration existed.
  final String? macAddress;

  final String? name;
  final int? tankId;

  /// How far the sensor face sits above the tank rim, in millimetres.
  final int sensorOffsetMm;

  final String? firmware;
  final bool isOnline;
  final DateTime createdAt;
  final DateTime? lastSeenAt;

  /// What to call this device on screen.
  String get label => (name != null && name!.isNotEmpty) ? name! : nodeId;

  factory Device.fromJson(Map<String, dynamic> json) => Device(
    id: json['id'] as int,
    nodeId: json['node_id'] as String,
    macAddress: json['mac_address'] as String?,
    name: json['name'] as String?,
    tankId: json['tank_id'] as int?,
    sensorOffsetMm: json['sensor_offset_mm'] as int,
    firmware: json['firmware'] as String?,
    isOnline: json['is_online'] as bool,
    createdAt: DateTime.parse(json['created_at'] as String),
    lastSeenAt: json['last_seen_at'] == null
        ? null
        : DateTime.parse(json['last_seen_at'] as String),
  );
}

/// What POST /devices answers: the device, plus what the node needs next.
class DeviceRegistration {
  const DeviceRegistration({
    required this.device,
    required this.brokerUri,
    required this.created,
  });

  final Device device;

  /// The broker address to hand to the node over BLE as the last step.
  final String brokerUri;

  /// False when the MAC was already registered (201 vs 200): re-onboarding
  /// keeps the device's name, tank and history.
  final bool created;

  factory DeviceRegistration.fromJson(
    Map<String, dynamic> json, {
    required bool created,
  }) => DeviceRegistration(
    device: Device.fromJson(json),
    brokerUri: json['broker_uri'] as String,
    created: created,
  );
}

/// A partial update for PATCH /devices/{id}.
///
/// The backend reads the body with `exclude_unset`, so an absent key means
/// "leave it alone" and an explicit null means "clear it". Those are different
/// requests, which is why this cannot just be a nullable-field object: it
/// tracks which fields were actually set.
///
/// `sensor_offset_mm` is the exception - its column is NOT NULL, so it can be
/// changed but never cleared, and the backend answers 422 to an explicit null.
class DeviceUpdate {
  final Map<String, dynamic> _fields = {};

  void setName(String? name) => _fields['name'] = name;

  void setTankId(int? tankId) => _fields['tank_id'] = tankId;

  void setSensorOffsetMm(int offset) => _fields['sensor_offset_mm'] = offset;

  bool get isEmpty => _fields.isEmpty;

  Map<String, dynamic> toJson() => Map.unmodifiable(_fields);
}
