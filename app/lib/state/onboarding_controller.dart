import 'dart:async';

import 'package:flutter/foundation.dart';

import '../api/api_client.dart';
import '../models/device.dart';
import '../provisioning/node_protocol_client.dart';
import '../provisioning/provisioning_transport.dart';
import '../provisioning/serial_transport.dart';
import 'device_store.dart';

/// Where the add-device flow is. Each value is one screen of the wizard.
enum OnboardingStep {
  /// Asking for Bluetooth permissions / checking the adapter.
  preparing,

  /// Bluetooth cannot be used; see [OnboardingController.readiness].
  blocked,

  /// Listing nodes in pairing mode.
  scanning,

  connecting,

  /// Connected; waiting for the user to click the node's button.
  awaitingConfirm,

  selectingWifi,

  /// The node is trying the WiFi credentials.
  testingWifi,

  /// POST /devices.
  registering,

  /// Broker address sent; waiting for the node to come online.
  finishing,

  done,
  failed,
}

/// How long each wait may take. Injectable so tests do not sit through them.
class OnboardingTimings {
  const OnboardingTimings({
    this.confirmPoll = const Duration(seconds: 1),
    // A little under the node's own 60 s, so the app reports the timeout
    // instead of only seeing the node hang up.
    this.confirmTimeout = const Duration(seconds: 55),
    this.statusPoll = const Duration(seconds: 1),
    // The node gives one test 20 s plus a few retries.
    this.wifiTimeout = const Duration(seconds: 30),
    this.onlinePoll = const Duration(seconds: 2),
    // Restart, WiFi association, DHCP and the MQTT session.
    this.onlineTimeout = const Duration(seconds: 90),
    // A node advertises several times a second while it is pairing; one not
    // heard from for this long has closed its window or gone out of range.
    this.nodeStale = const Duration(seconds: 10),
  });

  final Duration confirmPoll;
  final Duration confirmTimeout;
  final Duration statusPoll;
  final Duration wifiTimeout;
  final Duration onlinePoll;
  final Duration onlineTimeout;
  final Duration nodeStale;
}

/// The phone's half of onboarding a node, as a state machine for the wizard.
///
/// The order matters and is the point of the design (see the onboarding plan
/// and firmware provisioning.h):
///
///  1. connect over BLE and have the user confirm on the node,
///  2. let the node *prove* the WiFi credentials work,
///  3. only then register the node with the backend (so a node with broken
///     WiFi never leaves a row behind),
///  4. hand the node the broker address the backend returned - after which it
///     restarts and publishes, and is already known to ingest.
///
/// Registration is idempotent per MAC, so repeating the flow for a node that
/// was added before (new WiFi, factory reset) reuses its device.
class OnboardingController extends ChangeNotifier {
  OnboardingController({
    required ProvisioningTransport transport,
    required this._devices,
    this.timings = const OnboardingTimings(),
  }) : _transport = SerialTransport(transport) {
    _disconnectSub = _transport.disconnects.listen((_) => _onDisconnected());
  }

  /// Every call to the node goes through this one, so they never overlap
  /// (see [SerialTransport]).
  final SerialTransport _transport;
  final DeviceStore _devices;
  late final NodeProtocolClient _node = NodeProtocolClient(_transport);
  final OnboardingTimings timings;

  OnboardingStep step = OnboardingStep.preparing;
  BleReadiness? readiness;

  final Map<String, DiscoveredNode> _found = {};

  /// When each node in [_found] was last heard, to drop the ones that stopped.
  final Map<String, DateTime> _heardAt = {};
  Timer? _sweep;
  DiscoveredNode? selected;
  NodeInfo? info;
  List<WifiNetwork> networks = const [];

  /// Why the last WiFi test failed, shown on the WiFi step.
  String? wifiError;

  /// The SSID of the last test, so the WiFi step comes back with that network
  /// still chosen after a failure (typically a mistyped password).
  String? lastSsid;

  /// Why the flow failed, shown on the failed step.
  String? error;

  /// The registered device, once step 3 is done.
  Device? device;

  /// The node came online within [OnboardingTimings.onlineTimeout].
  bool online = false;

  /// The link failed around the bn-cloud answer, so the app cannot tell
  /// whether the node got the broker address. It most likely did (the node
  /// stores before it answers); [online] settles it.
  bool handoverUncertain = false;

  OnboardingStep? _failedAt;
  DeviceRegistration? _registration;
  StreamSubscription<DiscoveredNode>? _scanSub;
  late final StreamSubscription<void> _disconnectSub;

  /// Bumped whenever the flow is restarted or torn down; every async step
  /// checks it after each await so a stale continuation never touches state.
  int _run = 0;
  bool _disposed = false;

  /// Nodes found so far, closest first.
  List<DiscoveredNode> get nodes =>
      _found.values.toList()..sort((a, b) => b.rssi.compareTo(a.rssi));

  /// Whether this node is already registered in the backend.
  bool isKnown(DiscoveredNode node) {
    final nodeId = node.advert?.nodeId;
    if (nodeId == null) return false;
    return _devices.devices.any((device) => device.nodeId == nodeId);
  }

  /// A failure the user can retry without pairing again (the BLE session is
  /// still up, only the backend call failed).
  bool get canRetryStep => _failedAt == OnboardingStep.registering;

  // --- flow ---------------------------------------------------------------

  Future<void> start() async {
    final run = _restart();
    _setStep(OnboardingStep.preparing);

    final BleReadiness ready;
    try {
      ready = await _transport.ensureReady();
    } on ProvisioningException catch (e) {
      if (_stale(run)) return;
      _fail('Bluetooth sa nedá použiť: ${e.message}');
      return;
    }
    if (_stale(run)) return;

    readiness = ready;
    if (ready != BleReadiness.ok) {
      _setStep(OnboardingStep.blocked);
      return;
    }
    _startScan();
  }

  void _startScan() {
    _found.clear();
    _heardAt.clear();
    _setStep(OnboardingStep.scanning);
    _scanSub?.cancel();
    _scanSub = _transport.scan().listen((node) {
      _found[node.address] = node;
      _heardAt[node.address] = DateTime.now();
      notifyListeners();
    }, onError: (Object e) => _fail('Hľadanie zariadení zlyhalo: $e'));
    _sweep?.cancel();
    _sweep = Timer.periodic(timings.nodeStale ~/ 4, (_) => _dropStaleNodes());
  }

  /// Forget nodes that stopped advertising: tapping one would only run into
  /// the connection timeout.
  void _dropStaleNodes() {
    final cutoff = DateTime.now().subtract(timings.nodeStale);
    final stale = [
      for (final MapEntry(:key, :value) in _heardAt.entries)
        if (value.isBefore(cutoff)) key,
    ];
    if (stale.isEmpty) return;
    for (final address in stale) {
      _found.remove(address);
      _heardAt.remove(address);
    }
    notifyListeners();
  }

  Future<void> select(DiscoveredNode node) async {
    // A second tap while the first one connects would start a second
    // connection the platform side refuses ("busy") and fail the flow.
    if (step != OnboardingStep.scanning) return;
    final run = _run;
    _stopScan();
    selected = node;
    _setStep(OnboardingStep.connecting);

    try {
      await _transport.connect(node.address);
      if (_stale(run)) return;

      _setStep(OnboardingStep.awaitingConfirm);
      if (!await _waitForConfirmation(run)) return;

      final nodeInfo = await _node.info();
      if (_stale(run)) return;
      if (nodeInfo.protocolVersion != 1) {
        _fail(
          'Zariadenie má nepodporovaný firmvér (protokol '
          '${nodeInfo.protocolVersion}). Aktualizuj aplikáciu.',
        );
        return;
      }
      info = nodeInfo;

      await _loadNetworks(run);
      if (_stale(run)) return;
      _setStep(OnboardingStep.selectingWifi);
    } on ProvisioningException catch (e) {
      if (_stale(run)) return;
      _fail('Spojenie so zariadením zlyhalo: ${e.message}');
    } on NodeProtocolException catch (e) {
      if (_stale(run)) return;
      _fail(_protocolError(e));
    }
  }

  /// Poll bn-confirm until the user clicks the node's button.
  Future<bool> _waitForConfirmation(int run) async {
    final deadline = DateTime.now().add(timings.confirmTimeout);
    while (true) {
      final confirmed = await _node.confirm();
      if (_stale(run)) return false;
      if (confirmed) return true;
      if (DateTime.now().isAfter(deadline)) {
        _fail(
          'Potvrdenie vypršalo. Podrž tlačidlo na zariadení 3 s a skús to '
          'znova - po pripojení ho krátko stlač.',
        );
        await _safeDisconnect();
        return false;
      }
      await Future<void>.delayed(timings.confirmPoll);
      if (_stale(run)) return false;
    }
  }

  Future<void> _loadNetworks(int run) async {
    try {
      networks = normalizeNetworks(await _transport.scanWifi());
    } on ProvisioningException catch (e) {
      // Not fatal: the SSID can still be typed in by hand.
      debugPrint('WiFi scan failed: $e');
      networks = const [];
    }
  }

  /// Ask the node to scan for networks again.
  Future<void> refreshNetworks() async {
    final run = _run;
    await _loadNetworks(run);
    if (_stale(run)) return;
    notifyListeners();
  }

  Future<void> submitWifi(String ssid, String password) async {
    final run = _run;
    wifiError = null;
    lastSsid = ssid;
    _setStep(OnboardingStep.testingWifi);

    try {
      await _node.sendWifi(ssid, password);
      if (_stale(run)) return;

      final result = await _waitForWifi(run);
      if (_stale(run) || result == null) return;

      if (result.state != WifiTestState.connected) {
        wifiError = _wifiError(result.failure);
        _setStep(OnboardingStep.selectingWifi);
        return;
      }
    } on ProvisioningException catch (e) {
      if (_stale(run)) return;
      _fail('Spojenie so zariadením zlyhalo: ${e.message}');
      return;
    } on NodeProtocolException catch (e) {
      if (_stale(run)) return;
      // Both leave the session intact: let the user try again from the list.
      final message = switch (e.error) {
        'invalid' =>
          'Neplatný názov siete alebo heslo (heslo má 8 až 63 znakov alebo '
              '64 hexadecimálnych číslic).',
        'busy' => 'Zariadenie teraz nevie spustiť test WiFi. Skús to znova.',
        _ => null,
      };
      if (message != null) {
        wifiError = message;
        _setStep(OnboardingStep.selectingWifi);
        return;
      }
      _fail(_protocolError(e));
      return;
    }

    await _registerAndFinish(run);
  }

  /// Poll bn-status until the test settles; null when it was cut short.
  Future<WifiStatus?> _waitForWifi(int run) async {
    final deadline = DateTime.now().add(timings.wifiTimeout);
    while (true) {
      await Future<void>.delayed(timings.statusPoll);
      if (_stale(run)) return null;

      final status = await _node.status();
      if (_stale(run)) return null;
      if (status.state == WifiTestState.connected ||
          status.state == WifiTestState.failed) {
        return status;
      }
      if (DateTime.now().isAfter(deadline)) {
        return const WifiStatus(
          state: WifiTestState.failed,
          failure: WifiFailure.timeout,
        );
      }
    }
  }

  Future<void> _registerAndFinish(int run) async {
    _setStep(OnboardingStep.registering);
    final DeviceRegistration registration;
    try {
      registration = await _devices.register(info!.mac);
    } on ApiException catch (e) {
      if (_stale(run)) return;
      _fail('Registrácia na serveri zlyhala: ${e.message}');
      return;
    }
    if (_stale(run)) return;
    _registration = registration;
    device = registration.device;

    _setStep(OnboardingStep.finishing);
    try {
      await _node.sendCloud(registration.brokerUri);
    } on ProvisioningException catch (e) {
      if (_stale(run)) return;
      if (e.code == 'not_connected') {
        // The link was gone before the request left: nothing was sent.
        _fail('Spojenie so zariadením zlyhalo: ${e.message}');
        return;
      }
      // The node stores the configuration before it answers, and restarts
      // right after, so a link that fails here has most likely lost only the
      // answer. Failing the flow would hide a node that is coming online;
      // the backend tells which it was.
      debugPrint('bn-cloud answer lost (${e.code}): $e');
      handoverUncertain = true;
    } on NodeProtocolException catch (e) {
      if (_stale(run)) return;
      _fail(_protocolError(e));
      return;
    }
    if (_stale(run)) return;

    // The node restarts on its own now; the BLE link is no longer needed.
    await _safeDisconnect();
    if (_stale(run)) return;

    online = await _waitForOnline(run, registration.device);
    if (_stale(run)) return;
    _setStep(OnboardingStep.done);
  }

  /// Poll the backend until the node has announced itself online.
  ///
  /// "Online" alone is not enough for a re-onboarded node, whose row may
  /// still carry an old state; last_seen_at moving is what proves this very
  /// boot reached the broker.
  Future<bool> _waitForOnline(int run, Device registered) async {
    final deadline = DateTime.now().add(timings.onlineTimeout);
    while (DateTime.now().isBefore(deadline)) {
      await Future<void>.delayed(timings.onlinePoll);
      if (_stale(run)) return false;
      try {
        final current = await _devices.fetch(registered.id);
        if (_stale(run)) return false;
        device = current;
        if (current.isOnline && current.lastSeenAt != registered.lastSeenAt) {
          return true;
        }
      } on ApiException catch (e) {
        debugPrint('Polling device ${registered.id} failed: $e');
      }
    }
    return false;
  }

  /// Retry after a failure: the backend call alone when the session is still
  /// up ([canRetryStep]), otherwise the whole flow from the scan.
  Future<void> retry() async {
    if (canRetryStep && info != null) {
      _failedAt = null;
      error = null;
      await _registerAndFinish(_run);
      return;
    }
    await _safeDisconnect();
    await start();
  }

  /// Leave the pairing step and go back to the node list.
  Future<void> backToScan() async {
    _restart();
    await _safeDisconnect();
    if (_disposed) return;
    _startScan();
  }

  // --- helpers ------------------------------------------------------------

  void _onDisconnected() {
    // The failed step may offer to retry only the backend call, which needs
    // this session; without it, retrying has to start over.
    if (step == OnboardingStep.failed) {
      _failedAt = null;
      notifyListeners();
      return;
    }
    // Expected once the node has its configuration: it restarts.
    const expected = {
      OnboardingStep.finishing,
      OnboardingStep.done,
      OnboardingStep.scanning,
      OnboardingStep.preparing,
      OnboardingStep.blocked,
    };
    if (expected.contains(step)) return;
    _restart();
    _fail(
      'Zariadenie ukončilo spojenie. Podrž jeho tlačidlo 3 s a skús to znova.',
    );
  }

  int _restart() {
    _run++;
    _failedAt = null;
    error = null;
    wifiError = null;
    lastSsid = null;
    selected = null;
    info = null;
    device = null;
    online = false;
    handoverUncertain = false;
    _registration = null;
    networks = const [];
    return _run;
  }

  bool _stale(int run) => _disposed || run != _run;

  void _setStep(OnboardingStep next) {
    if (_disposed) return;
    step = next;
    notifyListeners();
  }

  void _fail(String message) {
    if (_disposed) return;
    _failedAt = step;
    error = message;
    step = OnboardingStep.failed;
    notifyListeners();
  }

  /// Stop listening for advertisements.
  ///
  /// Not awaited on purpose: nothing depends on the platform side having
  /// stopped, and a broadcast subscription's cancel() future is bound to the
  /// root zone, so awaiting it would hang under a widget test's fake clock.
  void _stopScan() {
    unawaited(_scanSub?.cancel());
    _scanSub = null;
    _sweep?.cancel();
    _sweep = null;
  }

  Future<void> _safeDisconnect() async {
    try {
      await _transport.disconnect();
    } on ProvisioningException catch (e) {
      debugPrint('Disconnect failed: $e');
    }
  }

  String _wifiError(WifiFailure? failure) => switch (failure) {
    WifiFailure.auth => 'Nesprávne heslo k WiFi.',
    WifiFailure.noAp =>
      'Zariadenie túto sieť nevidí. Je v dosahu a na 2,4 GHz?',
    WifiFailure.timeout => 'Pripojenie k WiFi trvalo príliš dlho.',
    _ => 'Pripojenie k WiFi zlyhalo.',
  };

  String _protocolError(NodeProtocolException e) => switch (e.error) {
    'not_confirmed' => 'Zariadenie nebolo potvrdené tlačidlom. Skús to znova.',
    'wifi_not_connected' => 'Zariadenie stratilo WiFi pred dokončením.',
    'invalid_uri' => 'Server poslal neplatnú adresu brokera.',
    'storage' => 'Zariadenie nevedelo uložiť nastavenia.',
    _ => 'Zariadenie odpovedalo neočakávane (${e.error}).',
  };

  /// The registration result, for the finish screen ("new" vs "re-added").
  bool get created => _registration?.created ?? false;

  @override
  void dispose() {
    _disposed = true;
    _run++;
    _scanSub?.cancel();
    _sweep?.cancel();
    _disconnectSub.cancel();
    // Fire and forget: the screen is going away either way.
    unawaited(_safeDisconnect());
    super.dispose();
  }
}
