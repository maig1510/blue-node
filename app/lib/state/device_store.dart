import 'package:flutter/foundation.dart';

import '../api/api_client.dart';
import '../models/device.dart';

/// The list of nodes. Same shape as TankStore, and shared for the same reason:
/// the tank form and the device screen both assign devices to tanks, and
/// neither should show a stale assignment afterwards.
class DeviceStore extends ChangeNotifier {
  DeviceStore(this._api);

  final ApiClient _api;

  List<Device> devices = [];
  bool loading = false;
  String? error;
  bool loaded = false;

  Device? byId(int id) {
    for (final device in devices) {
      if (device.id == id) return device;
    }
    return null;
  }

  /// Every device assigned to one tank. Usually one, but nothing stops two.
  List<Device> forTank(int tankId) =>
      devices.where((device) => device.tankId == tankId).toList();

  Future<void> refresh() async {
    loading = true;
    notifyListeners();

    try {
      devices = await _api.listDevices();
      error = null;
    } on ApiException catch (e) {
      error = e.message;
    } finally {
      loading = false;
      loaded = true;
      notifyListeners();
    }
  }

  /// Register a node during onboarding and keep the list in step.
  ///
  /// A MAC that was registered before replaces its existing entry instead of
  /// being added twice.
  Future<DeviceRegistration> register(String macAddress) async {
    final registration = await _api.registerDevice(macAddress);
    _upsert(registration.device);
    return registration;
  }

  /// Fetch one device and keep the list in step (onboarding polls this until
  /// the node comes online).
  Future<Device> fetch(int id) async {
    final device = await _api.getDevice(id);
    _upsert(device);
    return device;
  }

  void _upsert(Device device) {
    final known = devices.any((existing) => existing.id == device.id);
    devices = known
        ? [
            for (final existing in devices)
              if (existing.id == device.id) device else existing,
          ]
        : [...devices, device];
    notifyListeners();
  }

  Future<Device> update(int id, DeviceUpdate update) async {
    final updated = await _api.updateDevice(id, update);
    devices = [
      for (final existing in devices)
        if (existing.id == id) updated else existing,
    ];
    notifyListeners();
    return updated;
  }

  /// Point a device at a tank, or at nothing when [tankId] is null.
  Future<Device> assignToTank(int deviceId, int? tankId) {
    return update(deviceId, DeviceUpdate()..setTankId(tankId));
  }

  /// Forget assignments to a tank that no longer exists.
  ///
  /// The backend clears devices.tank_id itself (ON DELETE SET NULL), but it
  /// does so in the database - this keeps the copy in memory honest without a
  /// second round trip.
  void forgetTank(int tankId) {
    if (!devices.any((device) => device.tankId == tankId)) return;
    devices = devices.map((device) {
      if (device.tankId != tankId) return device;
      return Device(
        id: device.id,
        nodeId: device.nodeId,
        macAddress: device.macAddress,
        name: device.name,
        tankId: null,
        sensorOffsetMm: device.sensorOffsetMm,
        firmware: device.firmware,
        isOnline: device.isOnline,
        createdAt: device.createdAt,
        lastSeenAt: device.lastSeenAt,
      );
    }).toList();
    notifyListeners();
  }
}
