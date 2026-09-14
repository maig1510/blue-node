/// The app's colour system.
///
/// Two layers that must not be mixed up:
///
///  * **Chrome** - app bar, buttons, cards, navigation - comes from
///    ColorScheme.fromSeed(). Material derives a tonally consistent scheme, and
///    letting it do that is why the app looks coherent rather than assembled.
///  * **Data and status** - the water in the gauge, online/offline, low level -
///    uses the exact values in [BlueNodeColors] below. These must NOT go through
///    fromSeed: they were validated as specific hexes, and Material would retone
///    them into something that never passed that check.
///
/// The three hues carry meaning, they are not decoration:
///
///   blue   water, and the brand (it is called blue-node)
///   green  fine: device online, plenty of water
///   yellow attention: device offline, level running low
///
/// Checked with the Machado-Oliveira-Fernandes colour-vision simulation
/// (severity 1.0, Delta E in OKLab x100) against both surfaces. Worst pair is
/// green/yellow at CVD Delta E 11.3 (target >= 8) and 27.6 for normal vision
/// (floor 15) - clear in both modes.
///
/// One measured result shapes the whole design: **yellow on a light surface has
/// a contrast ratio of 1.75:1**, far below 3:1. It can never carry meaning on
/// its own and must never be thin text or a small dot on a light background.
/// That is why status is shown as an icon + label chip (see StatusChip) and why
/// numbers stay in ordinary text colour.
library;

import 'package:flutter/material.dart';

/// The seed the whole Material scheme is derived from: the water blue.
const _seed = Color(0xFF2A78D6);

/// Status and data colours, carried on the theme so widgets never hardcode one.
@immutable
class BlueNodeColors extends ThemeExtension<BlueNodeColors> {
  const BlueNodeColors({
    required this.water,
    required this.good,
    required this.warning,
    required this.goodContainer,
    required this.warningContainer,
    required this.onGoodContainer,
    required this.onWarningContainer,
  });

  /// The water itself. Lightened for the dark surface.
  final Color water;

  /// "Everything is fine" - online, plenty of water.
  final Color good;

  /// "Worth a look" - offline, level running low.
  final Color warning;

  /// Chip backgrounds, and the text that sits on them. Tinted rather than
  /// saturated so the label keeps its contrast - which is what makes a yellow
  /// chip readable where yellow text would not be.
  final Color goodContainer;
  final Color warningContainer;
  final Color onGoodContainer;
  final Color onWarningContainer;

  static const light = BlueNodeColors(
    water: Color(0xFF2A78D6),
    good: Color(0xFF0CA30C),
    warning: Color(0xFFFAB219),
    goodContainer: Color(0xFFDCF5DC),
    warningContainer: Color(0xFFFDF0D2),
    onGoodContainer: Color(0xFF0A5C0A),
    onWarningContainer: Color(0xFF6B4A00),
  );

  static const dark = BlueNodeColors(
    // Stepped up for the dark surface; green and yellow already clear it
    // (5.5:1 and 10:1) and stay as they are, so a screenshot of one mode is
    // recognisably the same app as the other.
    water: Color(0xFF3987E5),
    good: Color(0xFF0CA30C),
    warning: Color(0xFFFAB219),
    goodContainer: Color(0xFF143614),
    warningContainer: Color(0xFF3B2F0E),
    onGoodContainer: Color(0xFF7CD97C),
    onWarningContainer: Color(0xFFFAD27A),
  );

  @override
  BlueNodeColors copyWith({
    Color? water,
    Color? good,
    Color? warning,
    Color? goodContainer,
    Color? warningContainer,
    Color? onGoodContainer,
    Color? onWarningContainer,
  }) {
    return BlueNodeColors(
      water: water ?? this.water,
      good: good ?? this.good,
      warning: warning ?? this.warning,
      goodContainer: goodContainer ?? this.goodContainer,
      warningContainer: warningContainer ?? this.warningContainer,
      onGoodContainer: onGoodContainer ?? this.onGoodContainer,
      onWarningContainer: onWarningContainer ?? this.onWarningContainer,
    );
  }

  @override
  BlueNodeColors lerp(BlueNodeColors? other, double t) {
    if (other == null) return this;
    return BlueNodeColors(
      water: Color.lerp(water, other.water, t)!,
      good: Color.lerp(good, other.good, t)!,
      warning: Color.lerp(warning, other.warning, t)!,
      goodContainer: Color.lerp(goodContainer, other.goodContainer, t)!,
      warningContainer: Color.lerp(warningContainer, other.warningContainer, t)!,
      onGoodContainer: Color.lerp(onGoodContainer, other.onGoodContainer, t)!,
      onWarningContainer: Color.lerp(onWarningContainer, other.onWarningContainer, t)!,
    );
  }
}

ThemeData _theme(Brightness brightness, BlueNodeColors colors) {
  final scheme = ColorScheme.fromSeed(seedColor: _seed, brightness: brightness);

  return ThemeData(
    colorScheme: scheme,
    useMaterial3: true,
    extensions: [colors],
    appBarTheme: AppBarTheme(
      backgroundColor: scheme.surface,
      surfaceTintColor: scheme.surfaceTint,
      centerTitle: false,
    ),
    cardTheme: CardThemeData(
      elevation: 0,
      // A hairline instead of a shadow: the cards sit on a tinted surface, and
      // an outline separates them without the list looking like stacked paper.
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(16),
        side: BorderSide(color: scheme.outlineVariant),
      ),
      margin: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
    ),
    inputDecorationTheme: const InputDecorationTheme(
      border: OutlineInputBorder(),
      helperMaxLines: 2,
    ),
    listTileTheme: const ListTileThemeData(
      contentPadding: EdgeInsets.symmetric(horizontal: 16, vertical: 4),
    ),
  );
}

ThemeData get lightTheme => _theme(Brightness.light, BlueNodeColors.light);
ThemeData get darkTheme => _theme(Brightness.dark, BlueNodeColors.dark);

extension BlueNodeTheme on BuildContext {
  /// The status and data colours for the current brightness.
  BlueNodeColors get colors => Theme.of(this).extension<BlueNodeColors>()!;
}
