import 'package:flutter/material.dart';

import '../../provisioning/provisioning_transport.dart';
import '../../state/onboarding_controller.dart';
import '../../widgets/status_chip.dart';
import 'step_layout.dart';

/// Step 1: put the node into pairing mode and pick it from the list.
class ScanStep extends StatelessWidget {
  const ScanStep({super.key, required this.controller});

  final OnboardingController controller;

  @override
  Widget build(BuildContext context) {
    final nodes = controller.nodes;
    final theme = Theme.of(context);

    return StepLayout(
      icon: Icons.bluetooth_searching,
      title: 'Zapni párovanie na zariadení',
      message:
          'Podrž tlačidlo na zariadení 3 sekundy, kým LED nezačne pomaly '
          'blikať namodro. Nové zariadenie je v párovacom režime samo hneď po '
          'zapnutí.',
      busy: true,
      content: nodes.isEmpty
          ? Text(
              'Hľadám zariadenia v okolí…',
              textAlign: TextAlign.center,
              style: theme.textTheme.bodySmall?.copyWith(
                color: theme.colorScheme.outline,
              ),
            )
          : Card(
              clipBehavior: Clip.antiAlias,
              child: Column(
                children: [
                  for (final node in nodes)
                    _NodeTile(
                      node: node,
                      known: controller.isKnown(node),
                      onTap: () => controller.select(node),
                    ),
                ],
              ),
            ),
    );
  }
}

class _NodeTile extends StatelessWidget {
  const _NodeTile({
    required this.node,
    required this.known,
    required this.onTap,
  });

  final DiscoveredNode node;
  final bool known;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final details = [
      _signalLabel(node.rssi),
      if (node.advert?.provisioned ?? false) 'zmena WiFi',
    ].join(' · ');

    return ListTile(
      leading: const IconBadge(icon: Icons.sensors),
      title: Text(node.name),
      subtitle: Text(details),
      trailing: known
          ? const StatusChip.neutral(icon: Icons.check, label: 'Už pridané')
          : const Icon(Icons.chevron_right),
      onTap: onTap,
    );
  }

  /// RSSI in words: the number alone means nothing to most people, and
  /// "closest first" is what helps pick the right node out of several.
  static String _signalLabel(int rssi) {
    final quality = rssi >= -60
        ? 'veľmi blízko'
        : rssi >= -75
        ? 'blízko'
        : 'ďaleko';
    return 'Signál $rssi dBm, $quality';
  }
}
