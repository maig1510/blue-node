import 'package:flutter/material.dart';

import 'api/api_client.dart';
import 'screens/devices_screen.dart';
import 'screens/tanks_screen.dart';
import 'state/device_store.dart';
import 'state/tank_store.dart';

void main() {
  final api = ApiClient();
  runApp(
    AppScope(
      api: api,
      tanks: TankStore(api),
      devices: DeviceStore(api),
      child: const BlueNodeApp(),
    ),
  );
}

/// Holds the two stores and the API client for the whole app.
///
/// An InheritedWidget rather than a state-management package: there are
/// exactly two stores, they live for the life of the app, and this keeps the
/// dependency list empty. Screens reach them with `AppScope.of(context)`.
class AppScope extends InheritedWidget {
  const AppScope({
    super.key,
    required this.api,
    required this.tanks,
    required this.devices,
    required super.child,
  });

  final ApiClient api;
  final TankStore tanks;
  final DeviceStore devices;

  /// Look the scope up without subscribing to it.
  ///
  /// getInheritedWidgetOfExactType rather than dependOnInheritedWidgetOfExactType
  /// for two reasons: nothing here ever changes (see updateShouldNotify), and
  /// registering a dependency is illegal from initState, which is exactly
  /// where a screen wants to kick off its first load.
  static AppScope of(BuildContext context) {
    final scope = context.getInheritedWidgetOfExactType<AppScope>();
    assert(scope != null, 'AppScope is missing above this widget');
    return scope!;
  }

  // The stores are created once in main() and never replaced, so nothing here
  // can change - dependents are rebuilt by the stores themselves.
  @override
  bool updateShouldNotify(AppScope oldWidget) => false;
}

class BlueNodeApp extends StatelessWidget {
  const BlueNodeApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'blue-node',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.blue),
        useMaterial3: true,
      ),
      home: const HomeShell(),
    );
  }
}

/// The two top-level sections: tanks and the nodes that watch them.
class HomeShell extends StatefulWidget {
  const HomeShell({super.key});

  @override
  State<HomeShell> createState() => _HomeShellState();
}

class _HomeShellState extends State<HomeShell> {
  int _index = 0;

  @override
  void initState() {
    super.initState();
    // Both lists are loaded up front: the tank screen names the device
    // watching each tank, and the device screen names each device's tank, so
    // whichever tab opens first needs the other one's data anyway.
    final scope = AppScope.of(context);
    scope.tanks.refresh();
    scope.devices.refresh();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      // IndexedStack rather than swapping children: switching tabs keeps each
      // screen's scroll position and avoids re-running its initState.
      body: IndexedStack(
        index: _index,
        children: const [TanksScreen(), DevicesScreen()],
      ),
      bottomNavigationBar: NavigationBar(
        selectedIndex: _index,
        onDestinationSelected: (index) => setState(() => _index = index),
        destinations: const [
          NavigationDestination(
            icon: Icon(Icons.water_drop_outlined),
            selectedIcon: Icon(Icons.water_drop),
            label: 'Nádrže',
          ),
          NavigationDestination(
            icon: Icon(Icons.sensors_outlined),
            selectedIcon: Icon(Icons.sensors),
            label: 'Zariadenia',
          ),
        ],
      ),
    );
  }
}
