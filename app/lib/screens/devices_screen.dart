import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/device.dart';
import '../state/device_store.dart';
import '../state/tank_store.dart';
import '../widgets/async_view.dart';
import '../widgets/status_chip.dart';
import 'device_detail_screen.dart';
import 'onboarding/add_device_screen.dart';

/// Every registered node.
///
/// Nodes are added through the onboarding wizard (the "Pridať" button): it
/// pairs with the node over BLE, gives it WiFi and registers it. A node that
/// skipped onboarding never shows up here - ingest drops its data.
class DevicesScreen extends StatelessWidget {
  const DevicesScreen({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Zariadenia'),
        actions: [
          IconButton(
            onPressed: () => context.read<DeviceStore>().refresh(),
            icon: const Icon(Icons.refresh),
            tooltip: 'Obnoviť',
          ),
        ],
      ),
      // Listens to the tanks too, so a tank rename shows up in the subtitle
      // without a reload.
      body: Consumer2<DeviceStore, TankStore>(
        builder: (context, store, tanks, _) {
          return AsyncView(
            loading: store.loading,
            loaded: store.loaded,
            error: store.error,
            isEmpty: store.devices.isEmpty,
            emptyMessage:
                'Žiadne zariadenia.\nPridaj prvé tlačidlom Pridať - '
                'spáruješ ho cez Bluetooth a nastavíš mu WiFi.',
            onRetry: store.refresh,
            child: RefreshIndicator(
              onRefresh: store.refresh,
              child: ListView.builder(
                itemCount: store.devices.length,
                itemBuilder: (context, index) {
                  final device = store.devices[index];
                  final tank = device.tankId == null
                      ? null
                      : tanks.byId(device.tankId!);

                  return ListTile(
                    leading: const IconBadge(icon: Icons.sensors),
                    title: Text(device.label),
                    subtitle: Padding(
                      padding: const EdgeInsets.only(top: 4),
                      child: Text(_subtitle(device, tank?.name)),
                    ),
                    // The chip, not a coloured dot: the offline yellow is
                    // unreadable at dot size on a light surface, and colour on
                    // its own never says what it means.
                    trailing: deviceStatusChip(device.isOnline),
                    onTap: () => Navigator.of(context).push<void>(
                      MaterialPageRoute(
                        builder: (_) => DeviceDetailScreen(deviceId: device.id),
                      ),
                    ),
                  );
                },
              ),
            ),
          );
        },
      ),
      floatingActionButton: FloatingActionButton.extended(
        // Both tabs live in one IndexedStack, i.e. one route, and the tanks
        // tab has a FAB of its own: two default hero tags would collide on
        // every navigation.
        heroTag: 'add-device',
        onPressed: () => Navigator.of(context).push<void>(
          MaterialPageRoute(builder: (_) => const AddDeviceScreen()),
        ),
        icon: const Icon(Icons.add),
        label: const Text('Pridať'),
      ),
    );
  }

  String _subtitle(Device device, String? tankName) {
    final parts = <String>[
      // Always show the node id: it is what appears in the MQTT topic and in
      // the logs, so it is what you search for when something is wrong.
      device.nodeId,
      if (device.firmware != null) 'fw ${device.firmware}',
      tankName ?? 'bez nádrže',
    ];
    return parts.join(' · ');
  }
}
