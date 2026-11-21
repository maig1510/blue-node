import 'package:flutter/material.dart';

import '../../widgets/status_chip.dart';

/// The common frame of every wizard step: a badge, a title, an explanation,
/// optional content, and the step's actions at the bottom.
class StepLayout extends StatelessWidget {
  const StepLayout({
    super.key,
    required this.icon,
    required this.title,
    this.message,
    this.busy = false,
    this.content,
    this.actions = const [],
    this.iconColor,
  });

  final IconData icon;
  final String title;
  final String? message;

  /// Show an indeterminate progress bar under the title: the step is waiting
  /// on the node or the server, not on the user.
  final bool busy;

  /// Step-specific content between the explanation and the actions; scrolls
  /// with the rest when it does not fit.
  final Widget? content;
  final List<Widget> actions;
  final Color? iconColor;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return SafeArea(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Expanded(
            child: ListView(
              padding: const EdgeInsets.fromLTRB(24, 32, 24, 16),
              children: [
                Center(
                  child: IconBadge(icon: icon, size: 72, color: iconColor),
                ),
                const SizedBox(height: 20),
                Text(
                  title,
                  textAlign: TextAlign.center,
                  style: theme.textTheme.titleLarge,
                ),
                if (busy) ...[
                  const SizedBox(height: 16),
                  const LinearProgressIndicator(),
                ],
                if (message != null) ...[
                  const SizedBox(height: 12),
                  Text(
                    message!,
                    textAlign: TextAlign.center,
                    style: theme.textTheme.bodyMedium?.copyWith(
                      color: theme.colorScheme.onSurfaceVariant,
                      height: 1.4,
                    ),
                  ),
                ],
                if (content != null) ...[const SizedBox(height: 24), content!],
              ],
            ),
          ),
          if (actions.isNotEmpty)
            Padding(
              padding: const EdgeInsets.fromLTRB(24, 8, 24, 24),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  for (final (index, action) in actions.indexed) ...[
                    if (index > 0) const SizedBox(height: 8),
                    action,
                  ],
                ],
              ),
            ),
        ],
      ),
    );
  }
}
