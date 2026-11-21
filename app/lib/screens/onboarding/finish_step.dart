import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

import '../../api/api_client.dart';
import '../../models/device.dart';
import '../../state/device_store.dart';
import '../../state/onboarding_controller.dart';
import '../../state/tank_store.dart';
import '../../theme.dart';
import '../../widgets/async_view.dart';
import 'step_layout.dart';

/// Last step: the node is registered. Name it, put it on a tank, and set how
/// high above the rim it hangs - the same three fields the device screen has,
/// asked here so a new node is useful the moment the wizard closes.
class FinishStep extends StatefulWidget {
  const FinishStep({super.key, required this.controller});

  final OnboardingController controller;

  @override
  State<FinishStep> createState() => _FinishStepState();
}

class _FinishStepState extends State<FinishStep> {
  late final Device _device = widget.controller.device!;
  late final _nameController = TextEditingController(text: _device.name ?? '');
  late final _offsetController = TextEditingController(
    text: _device.sensorOffsetMm.toString(),
  );
  late int? _tankId = _device.tankId;
  bool _saving = false;

  @override
  void dispose() {
    _nameController.dispose();
    _offsetController.dispose();
    super.dispose();
  }

  Future<void> _save() async {
    final offset = int.tryParse(_offsetController.text.trim());
    if (offset == null || offset < 0) {
      showMessage(
        context,
        'Posun senzora musí byť celé číslo ≥ 0',
        isError: true,
      );
      return;
    }
    final name = _nameController.text.trim();
    final update = DeviceUpdate()
      ..setName(name.isEmpty ? null : name)
      ..setTankId(_tankId)
      ..setSensorOffsetMm(offset);

    setState(() => _saving = true);
    try {
      await context.read<DeviceStore>().update(_device.id, update);
      if (!mounted) return;
      Navigator.of(context).pop();
    } on ApiException catch (e) {
      if (!mounted) return;
      showMessage(context, e.message, isError: true);
    } finally {
      if (mounted) setState(() => _saving = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final controller = widget.controller;
    final tanks = context.watch<TankStore>().tanks;

    final String title;
    final String message;
    if (controller.online) {
      title = 'Zariadenie je online';
      message = controller.created
          ? 'Prvé merania už idú na server.'
          : 'Zariadenie už bolo pridané predtým - jeho názov, nádrž a '
                'história zostali. Zmenila sa len WiFi.';
    } else if (controller.handoverUncertain) {
      title = 'Zariadenie je pridané';
      message =
          'Spojenie sa prerušilo pri odovzdávaní nastavení a zariadenie sa '
          'zatiaľ neozvalo. Ak sa neobjaví ako online do pár minút, pridaj ho '
          'znova - jeho názov a nádrž zostanú.';
    } else {
      title = 'Zariadenie je pridané';
      message =
          'Na server sa ale zatiaľ neozvalo. Ak sa neobjaví ako online do '
          'pár minút, skontroluj, či beží broker a či je zariadenie v dosahu WiFi.';
    }

    return StepLayout(
      icon: controller.online ? Icons.check_circle_outline : Icons.schedule,
      iconColor: controller.online ? context.colors.onGoodContainer : null,
      title: title,
      message: message,
      content: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          TextField(
            controller: _nameController,
            decoration: InputDecoration(
              labelText: 'Názov',
              helperText: 'Prázdne pole zobrazí ${_device.nodeId}',
              border: const OutlineInputBorder(),
            ),
            textCapitalization: TextCapitalization.sentences,
          ),
          const SizedBox(height: 16),
          DropdownButtonFormField<int?>(
            initialValue: _tankId,
            decoration: const InputDecoration(
              labelText: 'Nádrž',
              border: OutlineInputBorder(),
            ),
            items: [
              const DropdownMenuItem(value: null, child: Text('Žiadna')),
              for (final tank in tanks)
                DropdownMenuItem(value: tank.id, child: Text(tank.name)),
            ],
            onChanged: (tankId) => setState(() => _tankId = tankId),
          ),
          const SizedBox(height: 16),
          TextField(
            controller: _offsetController,
            decoration: const InputDecoration(
              labelText: 'Posun senzora',
              helperText: 'Koľko mm nad hornou hranou nádrže je čelo senzora',
              helperMaxLines: 2,
              suffixText: 'mm',
              border: OutlineInputBorder(),
            ),
            keyboardType: TextInputType.number,
            inputFormatters: [FilteringTextInputFormatter.digitsOnly],
          ),
        ],
      ),
      actions: [
        FilledButton(
          onPressed: _saving ? null : _save,
          child: _saving
              ? const SizedBox(
                  width: 18,
                  height: 18,
                  child: CircularProgressIndicator(strokeWidth: 2),
                )
              : const Text('Uložiť a dokončiť'),
        ),
        TextButton(
          onPressed: _saving ? null : () => Navigator.of(context).pop(),
          child: const Text('Nastavím neskôr'),
        ),
      ],
    );
  }
}
