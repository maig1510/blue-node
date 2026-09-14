/// A tank's fill state, as computed by the backend from the newest reading.
///
/// All of the maths lives in `backend/api/calculators/` — the app only
/// formats what it is given.
class TankLevel {
  const TankLevel({
    required this.tankId,
    required this.deviceId,
    required this.distanceMm,
    required this.time,
    required this.waterHeightMm,
    required this.percent,
    required this.volumeL,
    required this.capacityL,
  });

  final int tankId;

  /// The device whose reading this came from.
  final int deviceId;

  /// Raw sensor reading: distance from the sensor face down to the water.
  final int distanceMm;

  final DateTime time;
  final double waterHeightMm;
  final double percent;
  final double volumeL;
  final double capacityL;

  /// Fill fraction for progress indicators, 0.0-1.0.
  double get fraction => (percent / 100).clamp(0.0, 1.0);

  factory TankLevel.fromJson(Map<String, dynamic> json) => TankLevel(
    tankId: json['tank_id'] as int,
    deviceId: json['device_id'] as int,
    distanceMm: json['distance_mm'] as int,
    time: DateTime.parse(json['time'] as String),
    waterHeightMm: (json['water_height_mm'] as num).toDouble(),
    percent: (json['percent'] as num).toDouble(),
    volumeL: (json['volume_l'] as num).toDouble(),
    capacityL: (json['capacity_l'] as num).toDouble(),
  );
}

/// One raw measurement, from /devices/{id}/latest.
class Reading {
  const Reading({
    required this.deviceId,
    required this.distanceMm,
    required this.time,
  });

  final int deviceId;
  final int? distanceMm;
  final DateTime time;

  factory Reading.fromJson(Map<String, dynamic> json) => Reading(
    deviceId: json['device_id'] as int,
    distanceMm: json['distance_mm'] as int?,
    time: DateTime.parse(json['time'] as String),
  );
}
