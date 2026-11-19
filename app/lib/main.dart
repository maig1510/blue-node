import 'package:flutter/material.dart';

import 'screens/devices_screen.dart';
import 'screens/tanks_screen.dart';
import 'state/app_providers.dart';
import 'theme.dart';

void main() {
  runApp(const AppProviders(child: BlueNodeApp()));
}

class BlueNodeApp extends StatelessWidget {
  const BlueNodeApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'blue-node',
      theme: lightTheme,
      darkTheme: darkTheme,
      // Follows the phone; both modes were checked against their own surface
      // rather than one being an automatic flip of the other.
      themeMode: ThemeMode.system,
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
  // The tank and device lists start loading in AppProviders, not here.
  int _index = 0;

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
