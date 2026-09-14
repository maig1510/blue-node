/// The tank shapes the backend knows about, and the measurements each needs.
///
/// This mirrors `backend/api/calculators/` — one module per shape there, one
/// entry here. A fourth shape means touching both sides, plus the
/// `tanks_shape_dimensions` CHECK in `backend/infra/timescale/init.sql`.
///
/// The form is driven off this table rather than a `switch` spread across the
/// screens: pick a shape, render its fields.
library;

import 'package:flutter/material.dart';

/// One measurement a shape needs, as it appears in the tank's `dimensions` map.
class DimensionField {
  const DimensionField(this.key, this.label, {this.hint});

  /// The JSON key the API expects, e.g. `diameter_cm`.
  final String key;

  /// Label shown in the form. All dimensions are centimetres.
  final String label;

  /// Optional clarification under the field.
  final String? hint;
}

class TankShape {
  const TankShape(this.value, this.label, this.icon, this.fields);

  /// The exact string stored in `tanks.shape` and accepted by the API.
  final String value;

  /// Human label for the dropdown.
  final String label;

  /// The tank's FOOTPRINT, which is what the shape actually describes: a
  /// circle, a square or a triangle seen from above.
  final IconData icon;

  final List<DimensionField> fields;

  static const cylinder = TankShape('cylinder', 'Valec', Icons.circle_outlined, [
    DimensionField('diameter_cm', 'Priemer'),
  ]);

  static const rectangular = TankShape('rectangular', 'Kváder', Icons.crop_square, [
    DimensionField('width_cm', 'Šírka'),
    DimensionField('depth_cm', 'Hĺbka'),
  ]);

  static const triangular = TankShape('triangular', 'Trojuholník', Icons.change_history, [
    DimensionField('base_cm', 'Základňa'),
    // Two different "heights" end up side by side in the form, so this one has
    // to say which it is: the altitude of the floor plan, not the tank's own
    // height. See backend/api/calculators/triangular.py.
    DimensionField(
      'triangle_height_cm',
      'Výška trojuholníka',
      hint: 'Výška podstavy v pôdoryse, nie výška nádrže',
    ),
  ]);

  static const all = [cylinder, rectangular, triangular];

  /// Look up a shape by the value the API uses.
  ///
  /// Falls back to the first shape rather than throwing: a tank created
  /// against a newer backend should still render, not crash the list.
  static TankShape byValue(String value) {
    return all.firstWhere(
      (shape) => shape.value == value,
      orElse: () => all.first,
    );
  }
}
