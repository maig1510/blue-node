import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../../provisioning/provisioning_transport.dart';
import '../../state/device_store.dart';
import '../../state/onboarding_controller.dart';
import 'finish_step.dart';
import 'scan_step.dart';
import 'step_layout.dart';
import 'wifi_step.dart';

/// The add-device wizard: one screen whose body follows
/// [OnboardingController.step].
///
/// A single route rather than one per step, because the steps are not places
/// to navigate between: the node and the BLE session dictate the order, and
/// "back" means "give up on this pairing", which closing the screen does.
class AddDeviceScreen extends StatefulWidget {
  const AddDeviceScreen({super.key, this.timings = const OnboardingTimings()});

  /// Tests shorten the waits.
  final OnboardingTimings timings;

  @override
  State<AddDeviceScreen> createState() => _AddDeviceScreenState();
}

class _AddDeviceScreenState extends State<AddDeviceScreen> {
  late final OnboardingController _controller = OnboardingController(
    transport: context.read<ProvisioningTransport>(),
    devices: context.read<DeviceStore>(),
    timings: widget.timings,
  );

  @override
  void initState() {
    super.initState();
    _controller.start();
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Pridať zariadenie')),
      body: ListenableBuilder(
        listenable: _controller,
        builder: (context, _) => _body(context),
      ),
    );
  }

  Widget _body(BuildContext context) {
    final c = _controller;
    final nodeName = c.selected?.name ?? 'zariadeniu';

    return switch (c.step) {
      OnboardingStep.preparing => const StepLayout(
        icon: Icons.bluetooth,
        title: 'Pripravujem Bluetooth…',
        busy: true,
      ),
      OnboardingStep.blocked => _blocked(c.readiness),
      OnboardingStep.scanning => ScanStep(controller: c),
      OnboardingStep.connecting => StepLayout(
        icon: Icons.bluetooth_connected,
        title: 'Pripájam sa k $nodeName…',
        busy: true,
      ),
      OnboardingStep.awaitingConfirm => StepLayout(
        icon: Icons.touch_app_outlined,
        title: 'Krátko stlač tlačidlo na zariadení',
        message:
            'LED na zariadení rýchlo bliká namodro. Stlačením potvrdíš, že ho '
            'pridávaš ty - cudzí telefón v dosahu to bez tlačidla nedokáže.',
        busy: true,
        actions: [
          TextButton(onPressed: c.backToScan, child: const Text('Zrušiť')),
        ],
      ),
      OnboardingStep.selectingWifi => WifiStep(controller: c),
      OnboardingStep.testingWifi => const StepLayout(
        icon: Icons.wifi_find,
        title: 'Zariadenie sa skúša pripojiť na WiFi…',
        message: 'Môže to trvať až pol minúty.',
        busy: true,
      ),
      OnboardingStep.registering => const StepLayout(
        icon: Icons.cloud_upload_outlined,
        title: 'Registrujem zariadenie na serveri…',
        busy: true,
      ),
      OnboardingStep.finishing => const StepLayout(
        icon: Icons.restart_alt,
        title: 'Zariadenie sa reštartuje a pripája…',
        message: 'Čakám, kým pošle prvú správu na server.',
        busy: true,
      ),
      OnboardingStep.done => FinishStep(controller: c),
      OnboardingStep.failed => StepLayout(
        icon: Icons.error_outline,
        iconColor: Theme.of(context).colorScheme.error,
        title: 'Nepodarilo sa',
        message: c.error,
        actions: [
          FilledButton(onPressed: c.retry, child: const Text('Skúsiť znova')),
          TextButton(
            onPressed: () => Navigator.of(context).pop(),
            child: const Text('Zavrieť'),
          ),
        ],
      ),
    };
  }

  Widget _blocked(BleReadiness? readiness) {
    final (title, message) = switch (readiness) {
      BleReadiness.bluetoothOff => (
        'Zapni Bluetooth',
        'Zariadenie sa pridáva cez Bluetooth. Zapni ho a skús to znova.',
      ),
      BleReadiness.unsupported => (
        'Telefón nepodporuje Bluetooth LE',
        'Bez neho sa zariadenie pridať nedá.',
      ),
      _ => (
        'Chýba povolenie Bluetooth',
        'Aplikácia ho potrebuje, aby našla zariadenie v okolí. Povoľ ho a '
            'skús to znova, prípadne v nastaveniach telefónu.',
      ),
    };

    return StepLayout(
      icon: Icons.bluetooth_disabled,
      title: title,
      message: message,
      actions: [
        if (readiness != BleReadiness.unsupported)
          FilledButton(
            onPressed: _controller.start,
            child: const Text('Skúsiť znova'),
          ),
      ],
    );
  }
}
