import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'src/create_configuration.dart';
import 'src/webview.dart';
import 'src/webview_impl.dart';

export 'src/cookie.dart';
export 'src/create_configuration.dart';
export 'src/user_script.dart';
export 'src/user_script_injection_time.dart';
export 'src/webview.dart';

class WebviewWindow {
  static const MethodChannel _channel = MethodChannel('webview_window');

  /// The open webviews, by view id.
  static final Map<int, WebviewImpl> _webviews = {};

  static bool _inited = false;

  static void _init() {
    if (_inited) {
      return;
    }
    _inited = true;
    _channel.setMethodCallHandler((call) async {
      try {
        _handleMethodCall(call);
      } catch (e, s) {
        debugPrint("method: ${call.method} args: ${call.arguments}");
        debugPrint('handleMethodCall error: $e $s');
      }
    });
  }

  /// Check if WebView runtime is available on the current devices.
  ///
  /// WebKitGTK is linked into the plugin, so this is always true.
  static Future<bool> isWebviewAvailable() async => true;

  static Future<Webview> create({
    CreateConfiguration configuration = const CreateConfiguration(),
  }) async {
    _init();
    final viewId = await _channel.invokeMethod(
      "create",
      configuration.toMap(),
    ) as int;
    final webview = WebviewImpl(viewId, _channel);
    _webviews[viewId] = webview;
    return webview;
  }

  static void _handleMethodCall(MethodCall call) {
    final args = call.arguments as Map;
    final webview = _webviews[args['id'] as int];
    // Events may still arrive for a webview that has just been closed.
    if (webview == null) {
      return;
    }
    switch (call.method) {
      case "onWindowClose":
        _webviews.remove(webview.viewId);
        webview.onClosed();
        break;
      case "onHistoryChanged":
        webview.onHistoryChanged(args['canGoBack'], args['canGoForward']);
        break;
      case "onNavigationStarted":
        webview.onNavigationStarted();
        break;
      case "onNavigationCompleted":
        webview.onNavigationCompleted();
        break;
      case "onUrlRequested":
        webview.onUrlRequested(args['url'] as String);
        break;
      case "onJavascriptWebMessageReceived":
        webview.onWebMessageReceived(args['message'] as String);
        break;
      case "onWebProcessTerminated":
        webview.onWebProcessTerminated(_parseTerminationReason(args['reason']));
        break;
    }
  }

  static WebProcessTerminationReason _parseTerminationReason(Object? reason) {
    switch (reason) {
      case "crashed":
        return WebProcessTerminationReason.crashed;
      case "exceededMemoryLimit":
        return WebProcessTerminationReason.exceededMemoryLimit;
      default:
        return WebProcessTerminationReason.other;
    }
  }

  /// Close all webview windows, then clear all cookies and storage.
  static Future<void> clearAll() async {
    await _channel.invokeMethod('clearAll');
  }
}
