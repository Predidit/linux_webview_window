import 'user_script.dart';

/// An HTTP proxy. It is applied to every webview of the application, not
/// only to the one it is passed to.
class ProxyConfiguration {
  final String host;
  final int port;

  const ProxyConfiguration({required this.host, required this.port});

  Map<String, dynamic> toMap() => {'host': host, 'port': port};
}

class CreateConfiguration {
  final int windowWidth;
  final int windowHeight;

  /// the title of window
  final String title;

  /// Scripts that are injected into every page loaded by the webview.
  final List<UserScript> userScripts;

  /// Whether to create the webview without showing its window.
  final bool headless;

  final ProxyConfiguration? proxy;

  const CreateConfiguration({
    this.windowWidth = 1280,
    this.windowHeight = 720,
    this.title = "",
    this.userScripts = const [],
    this.headless = false,
    this.proxy,
  });

  Map<String, dynamic> toMap() => {
        "windowWidth": windowWidth,
        "windowHeight": windowHeight,
        "title": title,
        "userScripts": userScripts.map((e) => e.toMap()).toList(),
        "headless": headless,
        "proxy": proxy?.toMap(),
      };
}
