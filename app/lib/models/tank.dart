import 'tank_shape.dart';

/// A tank as the API returns it from /tanks.
class Tank {
  const Tank({
    required this.id,
    required this.name,
    required this.shape,
    required this.heightCm,
    required this.dimensions,
    required this.createdAt,
    required this.updatedAt,
  });

  final int id;
  final String name;

  /// The raw API value (`cylinder`, `rectangular`, ...). Use [shapeInfo] for
  /// labels and the dimension fields.
  final String shape;

  final double heightCm;

  /// Shape-specific measurements, keyed as the API expects them.
  final Map<String, double> dimensions;

  final DateTime createdAt;
  final DateTime updatedAt;

  TankShape get shapeInfo => TankShape.byValue(shape);

  factory Tank.fromJson(Map<String, dynamic> json) {
    final dimensions = (json['dimensions'] as Map).map(
      // Postgres hands back whatever was stored, so 150 and 150.0 both happen.
      (key, value) => MapEntry(key as String, (value as num).toDouble()),
    );

    return Tank(
      id: json['id'] as int,
      name: json['name'] as String,
      shape: json['shape'] as String,
      heightCm: (json['height_cm'] as num).toDouble(),
      dimensions: dimensions,
      createdAt: DateTime.parse(json['created_at'] as String),
      updatedAt: DateTime.parse(json['updated_at'] as String),
    );
  }

  /// Body for POST /tanks and PATCH /tanks/{id}.
  ///
  /// Always complete: shape and dimensions have to agree, and the backend
  /// validates them as a pair, so a partial update of one without the other
  /// would be rejected.
  Map<String, dynamic> toJson() => {
    'name': name,
    'shape': shape,
    'height_cm': heightCm,
    'dimensions': dimensions,
  };
}
