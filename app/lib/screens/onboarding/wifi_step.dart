import 'dart:convert';

import 'package:flutter/material.dart';

import '../../provisioning/provisioning_transport.dart';
import '../../state/onboarding_controller.dart';
import '../../theme.dart';
import 'step_layout.dart';

/// Step 3: choose the WiFi the node should join.
///
/// The list comes from the node's own scan, so it shows what the node can
/// actually reach - which is not always what the phone sees (2.4 GHz only,
/// different spot in the house).
class WifiStep extends StatefulWidget {
  const WifiStep({super.key, required this.controller});

  final OnboardingController controller;

  @override
  State<WifiStep> createState() => _WifiStepState();
}

class _WifiStepState extends State<WifiStep> {
  final _ssidController = TextEditingController();
  final _passwordController = TextEditingController();

  WifiNetwork? _selected;

  /// The user is typing an SSID that is not in the list (hidden network).
  bool _manual = false;
  bool _showPassword = false;
  bool _refreshing = false;

  @override
  void initState() {
    super.initState();
    // Back from a failed test: keep the network, ask only for the password.
    final last = widget.controller.lastSsid;
    if (last == null) return;
    final listed = widget.controller.networks.where((n) => n.ssid == last);
    if (listed.isNotEmpty) {
      _selected = listed.first;
    } else {
      _manual = true;
      _ssidController.text = last;
    }
  }

  @override
  void dispose() {
    _ssidController.dispose();
    _passwordController.dispose();
    super.dispose();
  }

  /// Not trimmed: spaces at either end are legal in an SSID, and stripping
  /// them would send the node looking for a network that does not exist.
  String get _ssid => _manual ? _ssidController.text : (_selected?.ssid ?? '');

  /// Open networks need no password; a typed-in SSID might be either.
  bool get _needsPassword => _manual || (_selected?.secured ?? true);

  bool get _canSubmit {
    if (!isValidSsid(_ssid)) return false;
    if (!_needsPassword) return true;
    final password = _passwordController.text;
    // Empty only for an open network typed in by hand.
    if (password.isEmpty) return _manual;
    return isValidWifiPassword(password);
  }

  /// Explains a password that can never work, rather than just greying out
  /// the button. Too short is left alone: that is still being typed.
  String? get _passwordProblem {
    final password = _passwordController.text;
    if (password.isEmpty || isValidWifiPassword(password)) return null;
    if (utf8.encode(password).length < 8) return null;
    return 'Heslo môže mať najviac 63 znakov (alebo 64 hexadecimálnych číslic). '
        'Znaky s diakritikou sa rátajú dvakrát.';
  }

  /// Same idea for a typed-in SSID over the 32-byte limit.
  String? get _ssidProblem =>
      _manual && utf8.encode(_ssidController.text).length > 32
      ? 'Názov siete môže mať najviac 32 znakov (s diakritikou menej).'
      : null;

  Future<void> _refresh() async {
    setState(() => _refreshing = true);
    await widget.controller.refreshNetworks();
    if (mounted) setState(() => _refreshing = false);
  }

  void _submit() {
    if (!_canSubmit) return;
    FocusScope.of(context).unfocus();
    widget.controller.submitWifi(
      _ssid,
      _needsPassword ? _passwordController.text : '',
    );
  }

  @override
  Widget build(BuildContext context) {
    final controller = widget.controller;
    final theme = Theme.of(context);
    final error = controller.wifiError;

    return StepLayout(
      icon: Icons.wifi,
      title: 'Vyber WiFi pre zariadenie',
      message: 'Zariadenie sa pripája len na siete 2,4 GHz.',
      content: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          if (error != null) ...[
            Container(
              padding: const EdgeInsets.all(12),
              decoration: BoxDecoration(
                color: context.colors.warningContainer,
                borderRadius: BorderRadius.circular(12),
              ),
              child: Row(
                children: [
                  Icon(
                    Icons.error_outline,
                    color: context.colors.onWarningContainer,
                  ),
                  const SizedBox(width: 12),
                  Expanded(
                    child: Text(
                      error,
                      style: TextStyle(
                        color: context.colors.onWarningContainer,
                      ),
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 16),
          ],
          Row(
            children: [
              Text(
                'Siete v dosahu zariadenia',
                style: theme.textTheme.titleSmall,
              ),
              const Spacer(),
              IconButton(
                onPressed: _refreshing ? null : _refresh,
                icon: _refreshing
                    ? const SizedBox(
                        width: 18,
                        height: 18,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      )
                    : const Icon(Icons.refresh),
                tooltip: 'Hľadať znova',
              ),
            ],
          ),
          Card(
            clipBehavior: Clip.antiAlias,
            child: Column(
              children: [
                for (final network in controller.networks)
                  ListTile(
                    leading: Icon(_signalIcon(network.rssi)),
                    title: Text(network.ssid),
                    trailing: network.secured
                        ? const Icon(Icons.lock_outline, size: 18)
                        : null,
                    selected: !_manual && _selected?.ssid == network.ssid,
                    onTap: () => setState(() {
                      _manual = false;
                      _selected = network;
                    }),
                  ),
                ListTile(
                  leading: const Icon(Icons.edit_outlined),
                  title: const Text('Iná sieť…'),
                  selected: _manual,
                  onTap: () => setState(() {
                    _manual = true;
                    _selected = null;
                  }),
                ),
              ],
            ),
          ),
          const SizedBox(height: 16),
          if (_manual) ...[
            TextField(
              controller: _ssidController,
              autocorrect: false,
              enableSuggestions: false,
              decoration: InputDecoration(
                labelText: 'Názov siete (SSID)',
                errorText: _ssidProblem,
                border: const OutlineInputBorder(),
              ),
              onChanged: (_) => setState(() {}),
            ),
            const SizedBox(height: 16),
          ],
          if (_needsPassword && (_manual || _selected != null))
            TextField(
              controller: _passwordController,
              obscureText: !_showPassword,
              autocorrect: false,
              enableSuggestions: false,
              decoration: InputDecoration(
                labelText: 'Heslo',
                helperText: _manual ? 'Prázdne pre otvorenú sieť' : null,
                errorText: _passwordProblem,
                errorMaxLines: 3,
                border: const OutlineInputBorder(),
                suffixIcon: IconButton(
                  onPressed: () =>
                      setState(() => _showPassword = !_showPassword),
                  icon: Icon(
                    _showPassword ? Icons.visibility_off : Icons.visibility,
                  ),
                  tooltip: _showPassword ? 'Skryť heslo' : 'Zobraziť heslo',
                ),
              ),
              onChanged: (_) => setState(() {}),
              onSubmitted: (_) => _submit(),
            ),
        ],
      ),
      actions: [
        FilledButton(
          onPressed: _canSubmit ? _submit : null,
          child: const Text('Pripojiť'),
        ),
        TextButton(
          onPressed: controller.backToScan,
          child: const Text('Späť na zoznam zariadení'),
        ),
      ],
    );
  }

  static IconData _signalIcon(int rssi) => rssi >= -60
      ? Icons.network_wifi
      : rssi >= -75
      ? Icons.network_wifi_3_bar
      : Icons.network_wifi_1_bar;
}
