import 'dart:async';

import 'package:flutter/widgets.dart';

/// Re-runs [poll] on a timer for as long as the screen is alive.
///
/// The detail screens all want the same thing: fetch now, fetch again every
/// few seconds, stop on dispose, and never call setState after the widget is
/// gone. A request can still be in flight when the user navigates away, so
/// [poll] must check `mounted` before touching state - the timer stopping is
/// not enough on its own.
mixin PollingState<T extends StatefulWidget> on State<T> {
  Timer? _timer;

  /// How often to re-run [poll].
  Duration get pollInterval => const Duration(seconds: 3);

  /// Fetch and apply. Called immediately, then once per [pollInterval].
  Future<void> poll();

  void startPolling() {
    poll();
    _timer ??= Timer.periodic(pollInterval, (_) => poll());
  }

  void stopPolling() {
    _timer?.cancel();
    _timer = null;
  }

  @override
  void initState() {
    super.initState();
    startPolling();
  }

  @override
  void dispose() {
    stopPolling();
    super.dispose();
  }
}
