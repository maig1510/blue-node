import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

import '../api/api_client.dart';
import '../models/device.dart';
import '../models/tank_level.dart';
import '../state/device_store.dart';
import '../state/tank_store.dart';
import '../theme.dart';
import '../widgets/async_view.dart';
import '../widgets/polling.dart';
import '../widgets/status_chip.dart';

/// One node: what it is, what it last measured, and the three things a human
/// gets to decide about it.
class DeviceDetailScreen extends StatefulWidget {
  const DeviceDetailScreen({super.key, required this.deviceId});

  final int deviceId;

  @override
  State<DeviceDetailScreen> createState() => _DeviceDetailScreenState();
}

class _DeviceDetailScreenState extends State<DeviceDetailScreen>
    with PollingState<DeviceDetailScreen> {
  final _nameController = TextEditingController();
  final _offsetController = TextEditingController();

  Reading? _reading;

  /// Set when the device has registered but never reported a measurement.
  bool _noReadings = false;

  bool _saving = false;
  bool _initialised = false;

  @override
  void initState() {
    super.initState();
    final device = context.read<DeviceStore>().byId(widget.deviceId);
    if (device != null) _fillForm(device);
  }

  @override
  void dispose() {
    _nameController.dispose();
    _offsetController.dispose();
    super.dispose();
  }

  void _fillForm(Device device) {
    _nameController.text = device.name ?? '';
    _offsetController.text = device.sensorOffsetMm.toString();
    _initialised = true;
  }

  @override
  Future<void> poll() async {
    try {
      final reading = await context.read<ApiClient>().deviceLatest(
        widget.deviceId,
      );
      if (!mounted) return;
      setState(() {
        _reading = reading;
        _noReadings = false;
      });
    } on ApiException catch (e) {
      if (!mounted) return;
      // 404 here means the node has registered but not measured anything yet.
      if (e.isNotFound) setState(() => _noReadings = true);
    }
  }

  Future<void> _save(Device device, int? tankId) async {
    final offset = int.tryParse(_offsetController.text.trim());
    if (offset == null || offset < 0) {
      showMessage(context, 'Posun senzora musí byť celé číslo ≥ 0', isError: true);
      return;
    }

    final name = _nameController.text.trim();
    final update = DeviceUpdate()
      // An empty field means "no name", which the API expresses as null - not
      // as an empty string, and not as leaving the field out.
      ..setName(name.isEmpty ? null : name)
      ..setTankId(tankId)
      ..setSensorOffsetMm(offset);

    setState(() => _saving = true);
    try {
      await context.read<DeviceStore>().update(device.id, update);
      if (!mounted) return;
      showMessage(context, 'Uložené');
    } on ApiException catch (e) {
      if (!mounted) return;
      showMessage(context, e.message, isError: true);
    } finally {
      if (mounted) setState(() => _saving = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Consumer2<DeviceStore, TankStore>(
      builder: (context, deviceStore, tankStore, _) {
        final device = deviceStore.byId(widget.deviceId);
        if (device == null) {
          return const Scaffold(body: Center(child: CircularProgressIndicator()));
        }
        if (!_initialised) _fillForm(device);

        return Scaffold(
          appBar: AppBar(title: Text(device.label)),
          body: ListView(
            padding: const EdgeInsets.all(16),
            children: [
              _ReadingCard(reading: _reading, noReadings: _noReadings),
              const SizedBox(height: 24),

              Row(
                children: [
                  Text('Identita', style: Theme.of(context).textTheme.titleSmall),
                  const Spacer(),
                  deviceStatusChip(device.isOnline),
                ],
              ),
              const SizedBox(height: 8),
              _row(context, 'Node ID', device.nodeId),
              _row(context, 'MAC', device.macAddress ?? 'zatiaľ neohlásená'),
              _row(context, 'Firmvér', device.firmware ?? '—'),
              _row(context, 'Naposledy', _formatDateTime(device.lastSeenAt)),
              _row(context, 'Registrované', _formatDateTime(device.createdAt)),

              const SizedBox(height: 24),
              Text('Nastavenia', style: Theme.of(context).textTheme.titleSmall),
              const SizedBox(height: 12),

              TextField(
                controller: _nameController,
                decoration: const InputDecoration(
                  labelText: 'Názov',
                  helperText: 'Prázdne pole zobrazí node ID',
                  border: OutlineInputBorder(),
                ),
                textCapitalization: TextCapitalization.sentences,
              ),
              const SizedBox(height: 16),

              DropdownButtonFormField<int?>(
                initialValue: device.tankId,
                decoration: const InputDecoration(
                  labelText: 'Nádrž',
                  border: OutlineInputBorder(),
                ),
                items: [
                  const DropdownMenuItem(value: null, child: Text('Žiadna')),
                  for (final tank in tankStore.tanks)
                    DropdownMenuItem(value: tank.id, child: Text(tank.name)),
                ],
                onChanged: (tankId) => _save(device, tankId),
              ),
              const SizedBox(height: 16),

              TextField(
                controller: _offsetController,
                decoration: const InputDecoration(
                  labelText: 'Posun senzora',
                  // This number decides whether the level is right at all, so
                  // it is worth spelling out rather than labelling "offset".
                  helperText: 'Koľko mm nad hornou hranou nádrže je čelo senzora',
                  helperMaxLines: 2,
                  suffixText: 'mm',
                  border: OutlineInputBorder(),
                ),
                keyboardType: TextInputType.number,
                inputFormatters: [FilteringTextInputFormatter.digitsOnly],
              ),
              const SizedBox(height: 24),

              FilledButton.icon(
                onPressed: _saving ? null : () => _save(device, device.tankId),
                icon: _saving
                    ? const SizedBox(
                        width: 18,
                        height: 18,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      )
                    : const Icon(Icons.save_outlined),
                label: const Text('Uložiť'),
              ),
            ],
          ),
        );
      },
    );
  }

  Widget _row(BuildContext context, String label, String value) {
    final theme = Theme.of(context);
    return Padding(
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
          Flexible(
            child: Text(
              value,
              textAlign: TextAlign.end,
              style: theme.textTheme.bodyMedium,
            ),
          ),
        ],
      ),
    );
  }
}

/// The raw distance reading, refreshed while the screen is open.
class _ReadingCard extends StatelessWidget {
  const _ReadingCard({required this.reading, required this.noReadings});

  final Reading? reading;
  final bool noReadings;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return Card(
      child: Padding(
        padding: const EdgeInsets.symmetric(vertical: 24),
        child: Center(
          child: Column(
            children: [
              Text(
                reading?.distanceMm?.toString() ?? (noReadings ? '—' : '…'),
                style: theme.textTheme.displayMedium?.copyWith(
                  fontWeight: FontWeight.bold,
                  // The water blue: this is a distance to a water surface, and
                  // it is the one number this screen exists for.
                  color: context.colors.water,
                ),
              ),
              Text('mm', style: theme.textTheme.bodyMedium),
              const SizedBox(height: 8),
              Text(
                noReadings
                    ? 'Zatiaľ nič nenameralo'
                    : _formatDateTime(reading?.time),
                style: theme.textTheme.bodySmall?.copyWith(
                  color: theme.colorScheme.outline,
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

String _formatDateTime(DateTime? time) {
  if (time == null) return '—';
  final local = time.toLocal();
  String two(int n) => n.toString().padLeft(2, '0');
  return '${two(local.day)}.${two(local.month)}. '
      '${two(local.hour)}:${two(local.minute)}:${two(local.second)}';
}
