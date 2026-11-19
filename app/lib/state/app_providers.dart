import 'package:flutter/widgets.dart';
import 'package:provider/provider.dart';

import '../api/api_client.dart';
import 'device_store.dart';
import 'tank_store.dart';

/// Puts the API client and both stores above the whole app.
///
/// Screens read them with `context.read<T>()` for one-off calls (handlers,
/// initState, polling) and rebuild on change with `Consumer`/`context.watch`.
/// Both stores are app-lifetime: they are created on first use and disposed
/// together with the app.
class AppProviders extends StatelessWidget {
  const AppProviders({super.key, this.api, required this.child});

  /// Supply a client from outside (tests). When null, one is created here and
  /// closed when the providers are disposed; a supplied one is left to its
  /// owner.
  final ApiClient? api;

  final Widget child;

  @override
  Widget build(BuildContext context) {
    final api = this.api;

    return MultiProvider(
      providers: [
        if (api != null)
          Provider<ApiClient>.value(value: api)
        else
          Provider<ApiClient>(
            create: (_) => ApiClient(),
            dispose: (_, api) => api.close(),
          ),
        // Both lists load at startup (lazy: false): the tank screen names the
        // device watching each tank and the device screen names each device's
        // tank, so whichever tab opens first needs the other's data anyway.
        //
        // The load starts inside create on purpose. refresh() notifies
        // synchronously, and create runs before the provider subscribes, so
        // that first notification reaches nobody. Started from a widget's
        // initState instead, it would land on the provider mid-build.
        ChangeNotifierProvider<TankStore>(
          lazy: false,
          create: (context) => TankStore(context.read<ApiClient>())..refresh(),
        ),
        ChangeNotifierProvider<DeviceStore>(
          lazy: false,
          create: (context) =>
              DeviceStore(context.read<ApiClient>())..refresh(),
        ),
      ],
      child: child,
    );
  }
}
