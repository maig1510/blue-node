import 'package:flutter/material.dart';

/// A tank drawn as a filled vessel, for the detail screen.
class LevelGauge extends StatelessWidget {
  const LevelGauge({super.key, required this.fraction, required this.percent});

  /// 0.0-1.0, already clamped by the API.
  final double fraction;
  final double percent;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return SizedBox(
      width: 160,
      height: 220,
      child: Stack(
        alignment: Alignment.center,
        children: [
          // The vessel, filled from the bottom up.
          ClipRRect(
            borderRadius: BorderRadius.circular(16),
            child: Container(
              color: theme.colorScheme.surfaceContainerHighest,
              child: Align(
                alignment: Alignment.bottomCenter,
                child: FractionallySizedBox(
                  heightFactor: fraction,
                  widthFactor: 1,
                  child: AnimatedContainer(
                    duration: const Duration(milliseconds: 400),
                    color: theme.colorScheme.primary.withValues(alpha: 0.35),
                  ),
                ),
              ),
            ),
          ),
          Container(
            decoration: BoxDecoration(
              border: Border.all(color: theme.colorScheme.outlineVariant, width: 2),
              borderRadius: BorderRadius.circular(16),
            ),
          ),
          Text(
            '${percent.toStringAsFixed(1)} %',
            style: theme.textTheme.headlineMedium?.copyWith(
              fontWeight: FontWeight.bold,
              color: theme.colorScheme.onSurface,
            ),
          ),
        ],
      ),
    );
  }
}

/// A slim horizontal bar, for list rows where a full gauge is too much.
class LevelBar extends StatelessWidget {
  const LevelBar({super.key, required this.fraction});

  final double fraction;

  @override
  Widget build(BuildContext context) {
    return ClipRRect(
      borderRadius: BorderRadius.circular(4),
      child: LinearProgressIndicator(value: fraction, minHeight: 8),
    );
  }
}
