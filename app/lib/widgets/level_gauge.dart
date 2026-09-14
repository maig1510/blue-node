import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../theme.dart';

/// The tank drawn as a vessel of water, for the detail screen.
///
/// The water is blue because it is water - the status colours stay out of the
/// gauge and are carried by the chip beside it.
class LevelGauge extends StatefulWidget {
  const LevelGauge({super.key, required this.fraction, required this.percent});

  /// 0.0-1.0, already clamped by the API.
  final double fraction;
  final double percent;

  @override
  State<LevelGauge> createState() => _LevelGaugeState();
}

class _LevelGaugeState extends State<LevelGauge>
    with SingleTickerProviderStateMixin {
  late final AnimationController _wave = AnimationController(
    // Slow enough to read as water rather than as a loading indicator.
    duration: const Duration(seconds: 4),
    vsync: this,
  )..repeat();

  @override
  void dispose() {
    _wave.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final water = context.colors.water;

    return SizedBox(
      width: 170,
      height: 230,
      child: Stack(
        alignment: Alignment.center,
        children: [
          ClipRRect(
            borderRadius: BorderRadius.circular(20),
            child: Container(
              color: theme.colorScheme.surfaceContainerHighest,
              child: AnimatedBuilder(
                animation: _wave,
                builder: (context, _) => CustomPaint(
                  painter: _WaterPainter(
                    fraction: widget.fraction,
                    phase: _wave.value,
                    color: water,
                  ),
                  size: Size.infinite,
                ),
              ),
            ),
          ),

          // The vessel outline, drawn over the water so the fill reaches the
          // edge rather than stopping short of a border.
          IgnorePointer(
            child: Container(
              decoration: BoxDecoration(
                border: Border.all(color: theme.colorScheme.outlineVariant, width: 2),
                borderRadius: BorderRadius.circular(20),
              ),
            ),
          ),

          // Plain ink, not the water colour: a number is text, and a percentage
          // sitting on a half-filled tank has to stay legible against both the
          // water and the empty space above it.
          Text(
            '${widget.percent.toStringAsFixed(1)} %',
            style: theme.textTheme.headlineMedium?.copyWith(
              fontWeight: FontWeight.bold,
              color: theme.colorScheme.onSurface,
              shadows: [
                Shadow(
                  color: theme.colorScheme.surface.withValues(alpha: 0.85),
                  blurRadius: 12,
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

/// Fills the bottom [fraction] of the canvas with water, with a wave on top.
class _WaterPainter extends CustomPainter {
  _WaterPainter({
    required this.fraction,
    required this.phase,
    required this.color,
  });

  final double fraction;

  /// 0.0-1.0, one full trip of the wave.
  final double phase;
  final Color color;

  static const _waveHeight = 5.0;

  @override
  void paint(Canvas canvas, Size size) {
    if (fraction <= 0) return;

    // Leave room for the crest so a full tank does not clip its own wave.
    final surfaceY = (1 - fraction) * (size.height - _waveHeight) + _waveHeight;

    final path = Path()..moveTo(0, surfaceY);
    for (double x = 0; x <= size.width; x++) {
      final angle = (x / size.width) * 2 * math.pi + phase * 2 * math.pi;
      path.lineTo(x, surfaceY + math.sin(angle) * _waveHeight);
    }
    path
      ..lineTo(size.width, size.height)
      ..lineTo(0, size.height)
      ..close();

    // Lighter at the surface, denser at the bottom - the way a body of water
    // actually reads.
    final paint = Paint()
      ..shader = LinearGradient(
        begin: Alignment.topCenter,
        end: Alignment.bottomCenter,
        colors: [
          color.withValues(alpha: 0.35),
          color.withValues(alpha: 0.75),
        ],
      ).createShader(Rect.fromLTWH(0, surfaceY, size.width, size.height - surfaceY));

    canvas.drawPath(path, paint);

    // A brighter line right on the waterline, so the surface reads as a surface.
    canvas.drawPath(
      path,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 2
        ..color = color.withValues(alpha: 0.9),
    );
  }

  @override
  bool shouldRepaint(_WaterPainter old) =>
      old.fraction != fraction || old.phase != phase || old.color != color;
}

/// A slim horizontal bar, for list rows where a full gauge is too much.
class LevelBar extends StatelessWidget {
  const LevelBar({super.key, required this.fraction});

  final double fraction;

  @override
  Widget build(BuildContext context) {
    final water = context.colors.water;

    return ClipRRect(
      borderRadius: BorderRadius.circular(999),
      child: LinearProgressIndicator(
        value: fraction,
        minHeight: 8,
        color: water,
        backgroundColor: water.withValues(alpha: 0.15),
      ),
    );
  }
}
