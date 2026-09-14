import 'package:flutter/material.dart';

import '../api/api_client.dart';
import '../main.dart';
import '../models/tank.dart';
import '../models/tank_level.dart';
import '../widgets/async_view.dart';
import '../widgets/level_gauge.dart';
import 'tank_detail_screen.dart';
import 'tank_form_screen.dart';

/// Every tank, with how full it is.
class TanksScreen extends StatefulWidget {
  const TanksScreen({super.key});

  @override
  State<TanksScreen> createState() => _TanksScreenState();
}

class _TanksScreenState extends State<TanksScreen> {
  /// Level per tank id. Each one is its own request, so unlike the tank list
  /// these are fetched on entry and on pull-to-refresh rather than on a timer
  /// - a poll here would be one request per tank per tick. The detail screen
  /// is where the live view lives.
  Map<int, TankLevel> _levels = {};

  @override
  void initState() {
    super.initState();
    // The tank list itself is already being loaded by HomeShell; wait for it
    // and then fill in the levels.
    AppScope.of(context).tanks.addListener(_loadLevels);
    _loadLevels();
  }

  @override
  void dispose() {
    AppScope.of(context).tanks.removeListener(_loadLevels);
    super.dispose();
  }

  Future<void> _refresh() async {
    await AppScope.of(context).tanks.refresh();
    // _loadLevels runs via the store listener.
  }

  Future<void> _loadLevels() async {
    final api = AppScope.of(context).api;
    final tanks = AppScope.of(context).tanks.tanks;

    final levels = <int, TankLevel>{};
    for (final tank in tanks) {
      try {
        levels[tank.id] = await api.tankLevel(tank.id);
      } on ApiException catch (e) {
        // 404 is normal: no sensor assigned, or nothing measured yet. Anything
        // else is a real failure, but one tank's level failing should not take
        // the whole list down - the row just shows no level.
        if (!e.isNotFound) debugPrint('level for tank ${tank.id}: $e');
      }
    }

    if (!mounted) return;
    setState(() => _levels = levels);
  }

  Future<void> _openForm([Tank? tank]) async {
    await Navigator.of(context).push<void>(
      MaterialPageRoute(builder: (_) => TankFormScreen(tank: tank)),
    );
    // The store already told the list about the new tank; this fills in its
    // level (and picks up any dimension change that moved the percentage).
    _loadLevels();
  }

  @override
  Widget build(BuildContext context) {
    final store = AppScope.of(context).tanks;

    return Scaffold(
      appBar: AppBar(
        title: const Text('Nádrže'),
        actions: [
          IconButton(
            onPressed: _refresh,
            icon: const Icon(Icons.refresh),
            tooltip: 'Obnoviť',
          ),
        ],
      ),
      floatingActionButton: FloatingActionButton.extended(
        onPressed: _openForm,
        icon: const Icon(Icons.add),
        label: const Text('Nová nádrž'),
      ),
      body: ListenableBuilder(
        listenable: store,
        builder: (context, _) {
          return AsyncView(
            loading: store.loading,
            loaded: store.loaded,
            error: store.error,
            isEmpty: store.tanks.isEmpty,
            emptyMessage: 'Zatiaľ žiadne nádrže.\nPridaj prvú tlačidlom nižšie.',
            onRetry: _refresh,
            child: RefreshIndicator(
              onRefresh: _refresh,
              child: ListView.builder(
                padding: const EdgeInsets.only(bottom: 88),
                itemCount: store.tanks.length,
                itemBuilder: (context, index) {
                  final tank = store.tanks[index];
                  return _TankTile(
                    tank: tank,
                    level: _levels[tank.id],
                    onTap: () => _openDetail(tank),
                  );
                },
              ),
            ),
          );
        },
      ),
    );
  }

  Future<void> _openDetail(Tank tank) async {
    await Navigator.of(context).push<void>(
      MaterialPageRoute(builder: (_) => TankDetailScreen(tankId: tank.id)),
    );
    _loadLevels();
  }
}

class _TankTile extends StatelessWidget {
  const _TankTile({required this.tank, required this.level, required this.onTap});

  final Tank tank;
  final TankLevel? level;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final level = this.level;

    return Card(
      margin: const EdgeInsets.fromLTRB(12, 6, 12, 6),
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(12),
        child: Padding(
          padding: const EdgeInsets.all(16),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                children: [
                  Expanded(
                    child: Text(
                      tank.name,
                      style: theme.textTheme.titleMedium,
                      overflow: TextOverflow.ellipsis,
                    ),
                  ),
                  Text(
                    tank.shapeInfo.label,
                    style: theme.textTheme.labelMedium?.copyWith(
                      color: theme.colorScheme.outline,
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 12),
              if (level == null)
                Text(
                  'Bez merania',
                  style: theme.textTheme.bodySmall?.copyWith(
                    color: theme.colorScheme.outline,
                  ),
                )
              else ...[
                LevelBar(fraction: level.fraction),
                const SizedBox(height: 8),
                Row(
                  children: [
                    Text(
                      '${level.percent.toStringAsFixed(1)} %',
                      style: theme.textTheme.titleSmall?.copyWith(
                        color: theme.colorScheme.primary,
                        fontWeight: FontWeight.bold,
                      ),
                    ),
                    const Spacer(),
                    Text(
                      '${level.volumeL.toStringAsFixed(0)} / '
                      '${level.capacityL.toStringAsFixed(0)} l',
                      style: theme.textTheme.bodySmall?.copyWith(
                        color: theme.colorScheme.outline,
                      ),
                    ),
                  ],
                ),
              ],
            ],
          ),
        ),
      ),
    );
  }
}
