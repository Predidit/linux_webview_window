import 'package:desktop_webview_window/src/user_script_injection_time.dart';

class UserScript {
  const UserScript({
    required this.source,
    this.injectionTime = UserScriptInjectionTime.documentEnd,
    this.forAllFrames = true,
  });

  /// The script source code.
  final String source;

  /// The injection time.
  final UserScriptInjectionTime injectionTime;

  /// Whether the script should be injected into all frames.
  final bool forAllFrames;

  Map<String, dynamic> toMap() {
    return {
      'source': source,
      'injectionTime': injectionTime.index,
      'forAllFrames': forAllFrames,
    };
  }
}
