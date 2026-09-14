import 'package:flutter/material.dart';

import '../main.dart';
import '../models/device.dart';
import '../widgets/async_view.dart';
import 'device_detail_screen.dart';

/// Every node the ingest service has seen.
///
/// Nothing here creates a device: they register themselves the first time they
/// publish, so this list is a view of what is out there, not a form.
class DevicesScreen extends StatelessWidget {
  const DevicesScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final scope = AppScope.of(context);
    final store = scope.devices;

    return Scaffold(
      appBar: AppBar(
        title: const Text('Zariadenia'),
        actions: [
          IconButton(
            onPressed: store.refresh,
            icon: const Icon(Icons.refresh),
            tooltip: 'Obnoviť',
          ),
        ],
      ),
      // Merged so a tank rename shows up in the subtitle without a reload.
      body: ListenableBuilder(
        listenable: Listenable.merge([store, scope.tanks]),
        builder: (context, _) {
          return AsyncView(
            loading: store.loading,
            loaded: store.loaded,
            error: store.error,
            isEmpty: store.devices.isEmpty,
            emptyMessage:
                'Žiadne zariadenia.\nZapni node — zaregistruje sa sám, '
                'hneď ako pošle prvé meranie.',
            onRetry: store.refresh,
            child: RefreshIndicator(
              onRefresh: store.refresh,
              child: ListView.builder(
                itemCount: store.devices.length,
                itemBuilder: (context, index) {
                  final device = store.devices[index];
                  final tank = device.tankId == null
                      ? null
                      : scope.tanks.byId(device.tankId!);

                  return ListTile(
                    leading: Icon(
                      Icons.circle,
                      size: 12,
                      color: device.isOnline
                          ? Colors.green.shade600
                          : Colors.amber.shade700,
                    ),
                    title: Text(device.label),
                    subtitle: Text(_subtitle(device, tank?.name)),
                    trailing: const Icon(Icons.chevron_right),
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
