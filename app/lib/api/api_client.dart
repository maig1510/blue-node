import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:http/http.dart' as http;

import '../models/device.dart';
import '../models/tank.dart';
import '../models/tank_level.dart';

/// Override at run time so the same build works on emulator and phone:
///   flutter run --dart-define=API_BASE=http://10.0.2.2:8000
/// The default is this dev machine on the LAN (same IP the firmware uses for
/// the broker), which is what a real phone on the same WiFi needs.
const apiBase = String.fromEnvironment(
  'API_BASE',
  defaultValue: 'http://192.168.100.129:8000',
);

const _timeout = Duration(seconds: 4);

/// A request the API refused, or could not be made at all.
class ApiException implements Exception {
  ApiException(this.message, {this.statusCode});

  final String message;

  /// Null when the request never reached the server (no network, timeout).
  final int? statusCode;

  /// True when the server says the thing is simply not there. Callers use this
  /// to tell an empty state apart from a failure - most importantly on
  /// /tanks/{id}/level, where 404 means "no sensor yet", not "something broke".
  bool get isNotFound => statusCode == 404;

  @override
  String toString() => message;
}

class ApiClient {
  ApiClient({http.Client? httpClient, this.baseUrl = apiBase})
    : _http = httpClient ?? http.Client();

  final http.Client _http;
  final String baseUrl;

  // Tanks

  Future<List<Tank>> listTanks() async {
    final body = await _send('GET', '/tanks');
    return (body as List)
        .map((json) => Tank.fromJson(json as Map<String, dynamic>))
        .toList();
  }

  Future<Tank> getTank(int id) async =>
      Tank.fromJson(await _send('GET', '/tanks/$id') as Map<String, dynamic>);

  Future<Tank> createTank(Tank tank) async => Tank.fromJson(
    await _send('POST', '/tanks', body: tank.toJson()) as Map<String, dynamic>,
  );

  Future<Tank> updateTank(int id, Tank tank) async => Tank.fromJson(
    await _send('PATCH', '/tanks/$id', body: tank.toJson())
        as Map<String, dynamic>,
  );

  Future<void> deleteTank(int id) => _send('DELETE', '/tanks/$id');

  Future<TankLevel> tankLevel(int id) async => TankLevel.fromJson(
    await _send('GET', '/tanks/$id/level') as Map<String, dynamic>,
  );

  // Devices

  Future<List<Device>> listDevices() async {
    final body = await _send('GET', '/devices');
    return (body as List)
        .map((json) => Device.fromJson(json as Map<String, dynamic>))
        .toList();
  }

  /// Register a node by the MAC read from it over BLE (onboarding).
  ///
  /// Idempotent per MAC: a node registered before comes back as it was.
  Future<DeviceRegistration> registerDevice(String macAddress) async {
    final (status, body) = await _request(
      'POST',
      '/devices',
      body: {'mac_address': macAddress},
    );
    return DeviceRegistration.fromJson(
      body as Map<String, dynamic>,
      created: status == 201,
    );
  }

  Future<Device> getDevice(int id) async => Device.fromJson(
    await _send('GET', '/devices/$id') as Map<String, dynamic>,
  );

  Future<Device> updateDevice(int id, DeviceUpdate update) async =>
      Device.fromJson(
        await _send('PATCH', '/devices/$id', body: update.toJson())
            as Map<String, dynamic>,
      );

  Future<Reading> deviceLatest(int id) async => Reading.fromJson(
    await _send('GET', '/devices/$id/latest') as Map<String, dynamic>,
  );

  void close() => _http.close();

  /// Perform one request and decode its body.
  ///
  /// Returns null for responses that legitimately have no body (204).
  Future<Object?> _send(
    String method,
    String path, {
    Map<String, dynamic>? body,
  }) async => (await _request(method, path, body: body)).$2;

  /// Like [_send], for the callers that also need the success status code.
  Future<(int, Object?)> _request(
    String method,
    String path, {
    Map<String, dynamic>? body,
  }) async {
    final request = http.Request(method, Uri.parse('$baseUrl$path'));
    if (body != null) {
      request.headers['content-type'] = 'application/json';
      request.body = jsonEncode(body);
    }

    final http.Response response;
    try {
      final streamed = await _http.send(request).timeout(_timeout);
      response = await http.Response.fromStream(streamed);
    } catch (e) {
      // Never reached the server: no WiFi, wrong API_BASE, backend down. The
      // raw SocketException is several lines of errno and port numbers, so it
      // goes to the log; what is worth showing is the address being dialled,
      // because a wrong API_BASE is the usual cause.
      debugPrint('$method $path failed: $e');
      throw ApiException('Server $baseUrl je nedostupný');
    }

    if (response.statusCode >= 400) {
      throw ApiException(
        _errorMessage(response),
        statusCode: response.statusCode,
      );
    }

    // DELETE answers 204 with an empty body, and jsonDecode('') throws.
    if (response.statusCode == 204 || response.body.isEmpty) {
      return (response.statusCode, null);
    }
    return (response.statusCode, jsonDecode(response.body));
  }

  /// Turn a FastAPI error body into something worth showing a person.
  ///
  /// `detail` is a plain string for the errors we raise ourselves (404, 503)
  /// but a list of objects for Pydantic's 422s. Without handling both, a
  /// validation error renders as "Instance of '_Map'".
  String _errorMessage(http.Response response) {
    try {
      final detail = (jsonDecode(response.body) as Map)['detail'];

      if (detail is String) return detail;

      if (detail is List) {
        return detail
            .map((e) {
              final field = (e['loc'] as List?)?.last;
              final message = e['msg'];
              return field == null ? '$message' : '$field: $message';
            })
            .join('\n');
      }
    } catch (_) {
      // Not JSON, or not shaped the way we expect - fall through.
    }
    return 'HTTP ${response.statusCode}';
  }
}
