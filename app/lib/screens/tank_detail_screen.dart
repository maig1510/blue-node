import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../api/api_client.dart';
import '../models/tank.dart';
import '../models/tank_level.dart';
import '../state/device_store.dart';
import '../state/tank_store.dart';
import '../widgets/async_view.dart';
import '../widgets/level_gauge.dart';
import '../widgets/polling.dart';
import '../widgets/status_chip.dart';
import 'device_detail_screen.dart';
import 'tank_form_screen.dart';
import 'tanks_screen.dart' show tankLevelChip;

/// One tank, with its level refreshed while the screen is open.
class TankDetailScreen extends StatefulWidget {
  const TankDetailScreen({super.key, required this.tankId});

  final int tankId;

  @override
  State<TankDetailScreen> createState() => _TankDetailScreenState();
}

class _TankDetailScreenState extends State<TankDetailScreen>
    with PollingState<TankDetailScreen> {
  TankLevel? _level;

  /// Why there is no level, when there is none. The backend distinguishes
  /// "no device assigned" from "no readings yet", and both are ordinary
  /// states worth explaining rather than errors.
  String? _noLevelReason;

  String? _error;

  @override
  Future<void> poll() async {
    try {
      final level = await context.read<ApiClient>().tankLevel(widget.tankId);
      if (!mounted) return;
      setState(() {
        _level = level;
        _noLevelReason = null;
        _error = null;
      });
    } on ApiException catch (e) {
      if (!mounted) return;
      setState(() {
        if (e.isNotFound) {
          _level = null;
          _noLevelReason = e.message;
          _error = null;
        } else {
          _error = e.message;
        }
      });
    }
  }

  Future<void> _edit(Tank tank) async {
    await Navigator.of(context).push<void>(
      MaterialPageRoute(builder: (_) => TankFormScreen(tank: tank)),
    );
    poll();
  }

  Future<void> _confirmDelete(Tank tank) async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Zmazať nádrž?'),
        content: Text(
          '„${tank.name}" sa zmaže natrvalo. '
          'Priradený senzor zostane, len sa uvoľní.',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(context).pop(false),
            child: const Text('Zrušiť'),
          ),
          FilledButton(
            onPressed: () => Navigator.of(context).pop(true),
            child: const Text('Zmazať'),
          ),
        ],
      ),
    );

    if (confirmed != true || !mounted) return;

    // Stop the timer first: the tank is about to stop existing, and a poll
    // landing after the delete would show a spurious error.
    stopPolling();
    // Both looked up before the await: after it the screen may be gone.
    final tanks = context.read<TankStore>();
    final devices = context.read<DeviceStore>();
    try {
      await tanks.delete(tank.id);
      // The backend clears devices.tank_id itself; mirror that in memory.
      devices.forgetTank(tank.id);
      if (!mounted) return;
      Navigator.of(context).pop();
    } on ApiException catch (e) {
      if (!mounted) return;
      showMessage(context, e.message, isError: true);
      startPolling();
    }
  }

  @override
  Widget build(BuildContext context) {
    return Consumer2<TankStore, DeviceStore>(
      builder: (context, tankStore, deviceStore, _) {
        final tank = tankStore.byId(widget.tankId);
        if (tank == null) {
          // Deleted from under us, or the list has not arrived yet.
          return const Scaffold(body: Center(child: CircularProgressIndicator()));
        }

        final devices = deviceStore.forTank(tank.id);

        return Scaffold(
          appBar: AppBar(
            title: Text(tank.name),
            actions: [
              IconButton(
                onPressed: () => _edit(tank),
                icon: const Icon(Icons.edit_outlined),
                tooltip: 'Upraviť',
              ),
              IconButton(
                onPressed: () => _confirmDelete(tank),
                icon: const Icon(Icons.delete_outline),
                tooltip: 'Zmazať',
              ),
            ],
          ),
          body: ListView(
            padding: const EdgeInsets.all(16),
            children: [
              if (_error != null)
                Card(
                  color: Theme.of(context).colorScheme.errorContainer,
                  child: Padding(
                    padding: const EdgeInsets.all(12),
                    child: Text(_error!),
                  ),
                ),
              const SizedBox(height: 8),
              Center(child: _buildLevel(context)),
              if (_level != null) ...[
                const SizedBox(height: 16),
                Center(child: tankLevelChip(_level)),
              ],
              const SizedBox(height: 24),
              _Section(
                title: 'Nádrž',
                rows: [
                  ('Tvar', tank.shapeInfo.label),
                  ('Výška', '${_trim(tank.heightCm)} cm'),
                  for (final field in tank.shapeInfo.fields)
                    (
                      field.label,
                      '${_trim(tank.dimensions[field.key] ?? 0)} cm',
                    ),
                ],
              ),
              if (_level != null) ...[
                const SizedBox(height: 16),
                _Section(
                  title: 'Meranie',
                  rows: [
                    ('Výška hladiny', '${_trim(_level!.waterHeightMm / 10)} cm'),
                    ('Objem', '${_level!.volumeL.toStringAsFixed(1)} l'),
                    ('Kapacita', '${_level!.capacityL.toStringAsFixed(1)} l'),
                    ('Vzdialenosť senzora', '${_level!.distanceMm} mm'),
                    ('Čas', _formatTime(_level!.time)),
                  ],
                ),
              ],
              const SizedBox(height: 16),
              Text('Senzory', style: Theme.of(context).textTheme.titleSmall),
              if (devices.isEmpty)
                const ListTile(
                  leading: Icon(Icons.sensors_off_outlined),
                  title: Text('Žiadny senzor'),
                  subtitle: Text('Prirad ho v záložke Zariadenia.'),
                )
              else
                for (final device in devices)
                  ListTile(
                    leading: const IconBadge(icon: Icons.sensors),
                    title: Text(device.label),
                    subtitle: Text('Posun senzora ${device.sensorOffsetMm} mm'),
                    trailing: deviceStatusChip(device.isOnline),
                    onTap: () => Navigator.of(context).push<void>(
                      MaterialPageRoute(
                        builder: (_) => DeviceDetailScreen(deviceId: device.id),
                      ),
                    ),
                  ),
            ],
          ),
        );
      },
    );
  }

  Widget _buildLevel(BuildContext context) {
    final level = _level;
    if (level != null) {
      return LevelGauge(fraction: level.fraction, percent: level.percent);
    }

    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 32),
      child: Column(
        children: [
          Icon(
            Icons.water_drop_outlined,
            size: 48,
            color: Theme.of(context).colorScheme.outline,
          ),
          const SizedBox(height: 12),
          Text(
            _noLevelReason == null ? 'Načítavam…' : 'Bez merania',
            style: Theme.of(context).textTheme.titleMedium,
          ),
          if (_noLevelReason != null)
            Padding(
              padding: const EdgeInsets.only(top: 4),
              child: Text(
                _noLevelReason!,
                textAlign: TextAlign.center,
                style: Theme.of(context).textTheme.bodySmall?.copyWith(
                  color: Theme.of(context).colorScheme.outline,
                ),
              ),
            ),
        ],
      ),
    );
  }
}

/// A titled block of label/value rows.
class _Section extends StatelessWidget {
  const _Section({required this.title, required this.rows});

  final String title;
  final List<(String, String)> rows;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(title, style: theme.textTheme.titleSmall),
        const SizedBox(height: 8),
        for (final (label, value) in rows)
          Padding(
            padding: const EdgeInsets.symmetric(vertical: 4),
            child: Row(
              children: [
                Expanded(
                  child: Text(
                    label,
                    style: theme.textTheme.bodyMedium?.copyWith(
                      color: theme.colorScheme.outline,
                    ),
                  ),
                ),
                Text(value, style: theme.textTheme.bodyMedium),
              ],
            ),
          ),
      ],
    );
  }
}

/// Drop a trailing ".0" so 200.0 reads as 200.
String _trim(double value) {
  return value == value.roundToDouble()
      ? value.toStringAsFixed(0)
      : value.toStringAsFixed(1);
}

String _formatTime(DateTime time) {
  final local = time.toLocal();
  String two(int n) => n.toString().padLeft(2, '0');
  return '${two(local.hour)}:${two(local.minute)}:${two(local.second)}';
}
