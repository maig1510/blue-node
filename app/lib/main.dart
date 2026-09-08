import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;

/// Override at run time so the same build works on emulator and phone:
///   flutter run --dart-define=API_BASE=http://10.0.2.2:8000
/// The default is this dev machine on the LAN (same IP the firmware uses for
/// the broker), which is what a real phone on the same WiFi needs.
const apiBase = String.fromEnvironment(
  'API_BASE',
  defaultValue: 'http://192.168.100.129:8000',
);

const pollInterval = Duration(seconds: 2);

void main() => runApp(const BlueNodeApp());

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
      home: const LatestScreen(),
    );
  }
}

class LatestScreen extends StatefulWidget {
  const LatestScreen({super.key});

  @override
  State<LatestScreen> createState() => _LatestScreenState();
}

class _LatestScreenState extends State<LatestScreen> {
  Timer? _timer;
  String? _deviceId;
  int? _distanceMm;
  String? _time;
  String? _error;

  @override
  void initState() {
    super.initState();
    _fetch();
    _timer = Timer.periodic(pollInterval, (_) => _fetch());
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  Future<void> _fetch() async {
    try {
      final res = await http
          .get(Uri.parse('$apiBase/latest'))
          .timeout(const Duration(seconds: 4));

      if (res.statusCode != 200) {
        throw Exception('HTTP ${res.statusCode}');
      }
      final data = jsonDecode(res.body) as Map<String, dynamic>;

      // A widget can be disposed while the request is in flight.
      if (!mounted) return;
      setState(() {
        _deviceId = data['device_id'] as String?;
        _distanceMm = data['distance_mm'] as int?;
        _time = data['time'] as String?;
        _error = data['error'] as String?;
      });
    } catch (e) {
      if (!mounted) return;
      // Keep the last good value on screen; the next tick retries.
      setState(() => _error = e.toString());
    }
  }

  @override
  Widget build(BuildContext context) {
    final ok = _error == null && _distanceMm != null;

    return Scaffold(
      appBar: AppBar(
        title: const Text('blue-node'),
        backgroundColor: Colors.blue.shade700,
        foregroundColor: Colors.white,
      ),
      body: Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Row(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                Icon(
                  Icons.circle,
                  size: 12,
                  color: ok ? Colors.green.shade600 : Colors.amber.shade700,
                ),
                const SizedBox(width: 8),
                Text(
                  _deviceId ?? 'no device',
                  style: TextStyle(fontSize: 18, color: Colors.blue.shade400),
                ),
              ],
            ),
            const SizedBox(height: 16),
            Text(
              _distanceMm?.toString() ?? '--',
              style: TextStyle(
                fontSize: 72,
                fontWeight: FontWeight.bold,
                color: Colors.blue.shade900,
              ),
            ),
            Text('mm', style: TextStyle(fontSize: 20, color: Colors.blue.shade400)),
            const SizedBox(height: 16),
            Text(
              _time ?? '',
              style: TextStyle(fontSize: 12, color: Colors.blue.shade300),
            ),
            if (_error != null)
              Padding(
                padding: const EdgeInsets.all(24),
                child: Text(
                  _error!,
                  textAlign: TextAlign.center,
                  style: const TextStyle(fontSize: 12, color: Colors.redAccent),
                ),
              ),
          ],
        ),
      ),
    );
  }
}
