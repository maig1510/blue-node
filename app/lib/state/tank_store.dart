import 'package:flutter/foundation.dart';

import '../api/api_client.dart';
import '../models/tank.dart';

/// The list of tanks, shared by every screen that shows or changes one.
///
/// Mutations go through here rather than straight to the client so that a tank
/// created on the form screen shows up in the list behind it without anyone
/// having to remember to refresh.
class TankStore extends ChangeNotifier {
  TankStore(this._api);

  final ApiClient _api;

  List<Tank> tanks = [];
  bool loading = false;

  /// Message from the last failed load, cleared by the next successful one.
  String? error;

  /// True once a load has finished, so the UI can tell "still starting up"
  /// apart from "loaded, and there really are no tanks".
  bool loaded = false;

  Tank? byId(int id) {
    for (final tank in tanks) {
      if (tank.id == id) return tank;
    }
    return null;
  }

  Future<void> refresh() async {
    loading = true;
    notifyListeners();

    try {
      tanks = await _api.listTanks();
      error = null;
    } on ApiException catch (e) {
      error = e.message;
    } finally {
      loading = false;
      loaded = true;
      notifyListeners();
    }
  }

  /// Create a tank and return it. Throws ApiException, which the form shows.
  Future<Tank> create(Tank tank) async {
    final created = await _api.createTank(tank);
    tanks = [...tanks, created];
    notifyListeners();
    return created;
  }

  Future<Tank> update(int id, Tank tank) async {
    final updated = await _api.updateTank(id, tank);
    tanks = [
      for (final existing in tanks)
        if (existing.id == id) updated else existing,
    ];
    notifyListeners();
    return updated;
  }

  Future<void> delete(int id) async {
    await _api.deleteTank(id);
    tanks = tanks.where((tank) => tank.id != id).toList();
    notifyListeners();
  }
}
