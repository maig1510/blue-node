import 'package:flutter/material.dart';

import '../theme.dart';

/// A small coloured badge carrying one piece of state.
///
/// Every status in the app goes through here, and it always ships an icon AND a
/// word alongside the colour. That is not politeness: the warning yellow has a
/// contrast ratio of 1.75:1 on the light surface, so as a bare dot it would be
/// both invisible and meaningless to anyone who cannot separate it from the
/// green. The tinted background with dark text is what makes it readable, and
/// the icon plus label is what makes it understandable.
class StatusChip extends StatelessWidget {
  const StatusChip._({
    required this.icon,
    required this.label,
    required this.tone,
  });

  /// Fine: device online, plenty of water.
  const StatusChip.good({required IconData icon, required String label})
    : this._(icon: icon, label: label, tone: _Tone.good);

  /// Worth a look: device offline, level running low.
  const StatusChip.warning({required IconData icon, required String label})
    : this._(icon: icon, label: label, tone: _Tone.warning);

  /// Neither - nothing is wrong, there is simply nothing to report.
  const StatusChip.neutral({required IconData icon, required String label})
    : this._(icon: icon, label: label, tone: _Tone.neutral);

  final IconData icon;
  final String label;

  // Private: callers pick a tone through the named constructors, so that the
  // set of possible statuses stays closed.
  // ignore: library_private_types_in_public_api
  final _Tone tone;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final colors = context.colors;

    final (background, foreground) = switch (tone) {
      _Tone.good => (colors.goodContainer, colors.onGoodContainer),
      _Tone.warning => (colors.warningContainer, colors.onWarningContainer),
      _Tone.neutral => (
        theme.colorScheme.surfaceContainerHighest,
        theme.colorScheme.onSurfaceVariant,
      ),
    };

    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
      decoration: BoxDecoration(
        color: background,
        borderRadius: BorderRadius.circular(999),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(icon, size: 14, color: foreground),
          const SizedBox(width: 6),
          Text(
            label,
            style: theme.textTheme.labelMedium?.copyWith(
              color: foreground,
              fontWeight: FontWeight.w600,
            ),
          ),
        ],
      ),
    );
  }
}

enum _Tone { good, warning, neutral }

/// Whether a node is reporting. Shared by the device list, the device detail
/// and the sensor row on a tank, so all three say it the same way.
StatusChip deviceStatusChip(bool isOnline) {
  return isOnline
      ? const StatusChip.good(icon: Icons.wifi, label: 'Online')
      : const StatusChip.warning(icon: Icons.wifi_off, label: 'Offline');
}

/// An icon inside a tinted round badge, for list leading slots and empty states.
class IconBadge extends StatelessWidget {
  const IconBadge({
    super.key,
    required this.icon,
    this.color,
    this.size = 40,
  });

  final IconData icon;

  /// Defaults to the water blue - the app's own colour.
  final Color? color;
  final double size;

  @override
  Widget build(BuildContext context) {
    final tint = color ?? context.colors.water;

    return Container(
      width: size,
      height: size,
      decoration: BoxDecoration(
        color: tint.withValues(alpha: 0.12),
        shape: BoxShape.circle,
      ),
      child: Icon(icon, size: size * 0.5, color: tint),
    );
  }
}
