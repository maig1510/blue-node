import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../api/api_client.dart';
import '../main.dart';
import '../models/tank.dart';
import '../models/tank_shape.dart';
import '../widgets/async_view.dart';

/// Create or edit a tank.
///
/// One screen for both: the fields are identical, and the only difference is
/// whether saving POSTs or PATCHes. Pass [tank] to edit.
class TankFormScreen extends StatefulWidget {
  const TankFormScreen({super.key, this.tank});

  final Tank? tank;

  @override
  State<TankFormScreen> createState() => _TankFormScreenState();
}

class _TankFormScreenState extends State<TankFormScreen> {
  final _formKey = GlobalKey<FormState>();
  final _nameController = TextEditingController();
  final _heightController = TextEditingController();

  /// One controller per dimension key. Kept across shape changes so that
  /// flipping to another shape and back does not lose what was typed.
  final Map<String, TextEditingController> _dimensions = {};

  late TankShape _shape;

  /// Which device should watch this tank. The backend models the assignment on
  /// the device (devices.tank_id), so this becomes a second request after the
  /// tank itself is saved.
  int? _deviceId;

  bool _saving = false;

  bool get _isEdit => widget.tank != null;

  @override
  void initState() {
    super.initState();

    final tank = widget.tank;
    _shape = tank == null ? TankShape.all.first : tank.shapeInfo;

    if (tank != null) {
      _nameController.text = tank.name;
      _heightController.text = _trim(tank.heightCm);
      tank.dimensions.forEach((key, value) {
        _dimensions[key] = TextEditingController(text: _trim(value));
      });
      _deviceId = AppScope.of(context).devices.forTank(tank.id).firstOrNull?.id;
    }
  }

  @override
  void dispose() {
    _nameController.dispose();
    _heightController.dispose();
    for (final controller in _dimensions.values) {
      controller.dispose();
    }
    super.dispose();
  }

  TextEditingController _controllerFor(String key) =>
      _dimensions.putIfAbsent(key, TextEditingController.new);

  Future<void> _save() async {
    if (!_formKey.currentState!.validate()) return;

    setState(() => _saving = true);
    final scope = AppScope.of(context);

    // Only the fields of the selected shape: a leftover diameter_cm on a box
    // would be rejected by the API, which forbids unknown keys.
    final dimensions = {
      for (final field in _shape.fields)
        field.key: double.parse(_controllerFor(field.key).text.replaceAll(',', '.')),
    };

    final tank = Tank(
      id: widget.tank?.id ?? 0,
      name: _nameController.text.trim(),
      shape: _shape.value,
      heightCm: double.parse(_heightController.text.replaceAll(',', '.')),
      dimensions: dimensions,
      createdAt: DateTime.now(),
      updatedAt: DateTime.now(),
    );

    try {
      final saved = _isEdit
          ? await scope.tanks.update(widget.tank!.id, tank)
          : await scope.tanks.create(tank);

      await _applyDeviceAssignment(scope, saved.id);

      if (!mounted) return;
      Navigator.of(context).pop();
    } on ApiException catch (e) {
      if (!mounted) return;
      setState(() => _saving = false);
      showMessage(context, e.message, isError: true);
    }
  }

  /// Point the chosen device at this tank, and release the previous one.
  ///
  /// Saved separately from the tank, so this is the one place a half-success
  /// is possible: the tank exists but the sensor is not attached. That has to
  /// be said out loud rather than swallowed, hence its own try/catch.
  Future<void> _applyDeviceAssignment(AppScope scope, int tankId) async {
    final previous = scope.devices.forTank(tankId).firstOrNull?.id;
    if (previous == _deviceId) return;

    try {
      if (previous != null) {
        await scope.devices.assignToTank(previous, null);
      }
      if (_deviceId != null) {
        await scope.devices.assignToTank(_deviceId!, tankId);
      }
    } on ApiException catch (e) {
      if (!mounted) return;
      showMessage(
        context,
        'Nádrž uložená, ale priradenie senzora zlyhalo: ${e.message}',
        isError: true,
      );
    }
  }

  @override
  Widget build(BuildContext context) {
    final devices = AppScope.of(context).devices.devices;

    return Scaffold(
      appBar: AppBar(title: Text(_isEdit ? 'Upraviť nádrž' : 'Nová nádrž')),
      body: Form(
        key: _formKey,
        child: ListView(
          padding: const EdgeInsets.all(16),
          children: [
            TextFormField(
              controller: _nameController,
              decoration: const InputDecoration(
                labelText: 'Názov',
                border: OutlineInputBorder(),
              ),
              textCapitalization: TextCapitalization.sentences,
              validator: (value) =>
                  (value == null || value.trim().isEmpty) ? 'Zadaj názov' : null,
            ),
            const SizedBox(height: 16),

            DropdownButtonFormField<TankShape>(
              initialValue: _shape,
              decoration: const InputDecoration(
                labelText: 'Tvar',
                border: OutlineInputBorder(),
              ),
              items: [
                for (final shape in TankShape.all)
                  DropdownMenuItem(value: shape, child: Text(shape.label)),
              ],
              onChanged: (shape) {
                if (shape != null) setState(() => _shape = shape);
              },
            ),
            const SizedBox(height: 16),

            // Rebuilt whenever the shape changes - this is the whole reason
            // TankShape carries its field list.
            for (final field in _shape.fields) ...[
              _NumberField(
                controller: _controllerFor(field.key),
                label: field.label,
                helper: field.hint,
              ),
              const SizedBox(height: 16),
            ],

            _NumberField(
              controller: _heightController,
              label: 'Výška nádrže',
              helper: 'Zvislá výška od dna po hornú hranu',
            ),
            const SizedBox(height: 16),

            DropdownButtonFormField<int?>(
              initialValue: _deviceId,
              decoration: const InputDecoration(
                labelText: 'Senzor',
                border: OutlineInputBorder(),
                helperText: 'Zariadenie, ktoré meria hladinu tejto nádrže',
              ),
              items: [
                const DropdownMenuItem(value: null, child: Text('Žiadny')),
                for (final device in devices)
                  DropdownMenuItem(value: device.id, child: Text(device.label)),
              ],
              onChanged: (id) => setState(() => _deviceId = id),
            ),
            const SizedBox(height: 32),

            FilledButton.icon(
              onPressed: _saving ? null : _save,
              icon: _saving
                  ? const SizedBox(
                      width: 18,
                      height: 18,
                      child: CircularProgressIndicator(strokeWidth: 2),
                    )
                  : const Icon(Icons.save_outlined),
              label: Text(_isEdit ? 'Uložiť zmeny' : 'Vytvoriť nádrž'),
            ),
          ],
        ),
      ),
    );
  }
}

/// A centimetre measurement. Everything a tank is made of is a positive number
/// of centimetres, so the validation is the same every time.
class _NumberField extends StatelessWidget {
  const _NumberField({
    required this.controller,
    required this.label,
    this.helper,
  });

  final TextEditingController controller;
  final String label;
  final String? helper;

  @override
  Widget build(BuildContext context) {
    return TextFormField(
      controller: controller,
      decoration: InputDecoration(
        labelText: label,
        helperText: helper,
        helperMaxLines: 2,
        suffixText: 'cm',
        border: const OutlineInputBorder(),
      ),
      keyboardType: const TextInputType.numberWithOptions(decimal: true),
      inputFormatters: [FilteringTextInputFormatter.allow(RegExp(r'[0-9.,]'))],
      validator: (value) {
        // The comma is what a Slovak keyboard offers for a decimal point.
        final number = double.tryParse((value ?? '').replaceAll(',', '.'));
        if (number == null) return 'Zadaj číslo';
        if (number <= 0) return 'Musí byť väčšie ako 0';
        return null;
      },
    );
  }
}

String _trim(double value) {
  return value == value.roundToDouble()
      ? value.toStringAsFixed(0)
      : value.toStringAsFixed(1);
}
