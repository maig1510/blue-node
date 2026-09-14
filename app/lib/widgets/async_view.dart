import 'package:flutter/material.dart';

import 'status_chip.dart';

/// The three states every list screen has to render besides its content:
/// still loading, failed, or loaded but empty.
class AsyncView extends StatelessWidget {
  const AsyncView({
    super.key,
    required this.loading,
    required this.loaded,
    required this.error,
    required this.isEmpty,
    required this.emptyMessage,
    required this.onRetry,
    required this.child,
  });

  final bool loading;
  final bool loaded;
  final String? error;
  final bool isEmpty;
  final String emptyMessage;
  final VoidCallback onRetry;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    // Only block the screen on the very first load; a refresh keeps the old
    // content on screen underneath instead of flashing a spinner.
    if (loading && !loaded) {
      return const Center(child: CircularProgressIndicator());
    }

    if (error != null && isEmpty) {
      return _Message(
        icon: Icons.cloud_off,
        text: error!,
        action: FilledButton.tonal(
          onPressed: onRetry,
          child: const Text('Skúsiť znova'),
        ),
      );
    }

    if (isEmpty) {
      return _Message(icon: Icons.inbox_outlined, text: emptyMessage);
    }

    return child;
  }
}

/// A centred icon + text, used for both the empty and the failed state.
class _Message extends StatelessWidget {
  const _Message({required this.icon, required this.text, this.action});

  final IconData icon;
  final String text;
  final Widget? action;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return Center(
      child: Padding(
        padding: const EdgeInsets.all(32),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            IconBadge(icon: icon, size: 72),
            const SizedBox(height: 20),
            Text(
              text,
              textAlign: TextAlign.center,
              style: theme.textTheme.bodyMedium?.copyWith(
                color: theme.colorScheme.onSurfaceVariant,
                height: 1.4,
              ),
            ),
            if (action != null) ...[const SizedBox(height: 20), action!],
          ],
        ),
      ),
    );
  }
}

/// Show a short message at the bottom of the screen.
///
/// Guards against a disposed context itself so callers do not have to repeat
/// the `mounted` dance after every await.
void showMessage(BuildContext context, String message, {bool isError = false}) {
  if (!context.mounted) return;
  final theme = Theme.of(context);

  ScaffoldMessenger.of(context).showSnackBar(
    SnackBar(
      content: Text(message),
      backgroundColor: isError ? theme.colorScheme.error : null,
    ),
  );
}
