import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'cookie.dart';
import 'webview.dart';

class WebviewImpl extends Webview {
  WebviewImpl(this.viewId, this._channel);

  final int viewId;

  final MethodChannel _channel;

  bool _closed = false;

  final _closeCompleter = Completer<void>();

  final ValueNotifier<bool> _isNavigating = ValueNotifier<bool>(false);

  OnHistoryChangedCallback? _onHistoryChanged;

  OnUrlRequestCallback? _onUrlRequestCallback;

  final Set<OnWebMessageReceivedCallback> _onWebMessageReceivedCallbacks = {};

  // Events sent by the native window.

  void onClosed() {
    if (_closed) {
      return;
    }
    _closed = true;
    _closeCompleter.complete();
  }

  void onHistoryChanged(bool canGoBack, bool canGoForward) {
    _onHistoryChanged?.call(canGoBack, canGoForward);
  }

  void onNavigationStarted() {
    _isNavigating.value = true;
  }

  void onNavigationCompleted() {
    _isNavigating.value = false;
  }

  void onUrlRequested(String url) {
    _onUrlRequestCallback?.call(url);
  }

  void onWebMessageReceived(String message) {
    for (final callback in _onWebMessageReceivedCallbacks) {
      callback(message);
    }
  }

  // Webview interface.

  @override
  Future<void> get onClose => _closeCompleter.future;

  @override
  ValueListenable<bool> get isNavigating => _isNavigating;

  @override
  void launch(String url) {
    _channel.invokeMethod("launch", {
      "viewId": viewId,
      "url": url,
    });
  }

  @override
  void addScriptToExecuteOnDocumentCreated(String javaScript) {
    assert(javaScript.trim().isNotEmpty);
    _channel.invokeMethod("addScriptToExecuteOnDocumentCreated", {
      "viewId": viewId,
      "javaScript": javaScript,
    });
  }

  @override
  Future<void> setApplicationNameForUserAgent(String applicationName) {
    return _channel.invokeMethod("setApplicationNameForUserAgent", {
      "viewId": viewId,
      "applicationName": applicationName,
    });
  }

  @override
  Future<void> back() {
    return _channel.invokeMethod("back", {"viewId": viewId});
  }

  @override
  Future<void> forward() {
    return _channel.invokeMethod("forward", {"viewId": viewId});
  }

  @override
  Future<void> reload() {
    return _channel.invokeMethod("reload", {"viewId": viewId});
  }

  @override
  Future<void> stop() {
    return _channel.invokeMethod("stop", {"viewId": viewId});
  }

  @override
  void setOnHistoryChangedCallback(OnHistoryChangedCallback? callback) {
    _onHistoryChanged = callback;
  }

  @override
  void setOnUrlRequestCallback(OnUrlRequestCallback? callback) {
    _onUrlRequestCallback = callback;
  }

  @override
  void addOnWebMessageReceivedCallback(OnWebMessageReceivedCallback callback) {
    _onWebMessageReceivedCallbacks.add(callback);
  }

  @override
  void removeOnWebMessageReceivedCallback(
      OnWebMessageReceivedCallback callback) {
    _onWebMessageReceivedCallbacks.remove(callback);
  }

  @override
  void close() {
    if (_closed) {
      return;
    }
    _channel.invokeMethod("close", {"viewId": viewId});
  }

  @override
  Future<String?> evaluateJavaScript(String javaScript) {
    return _channel.invokeMethod<String>("evaluateJavaScript", {
      "viewId": viewId,
      "javaScriptString": javaScript,
    });
  }

  @override
  Future<List<WebviewCookie>> getAllCookies() async {
    final result = await _channel.invokeListMethod<Map>("getAllCookies", {
      "viewId": viewId,
    });

    return result
            ?.map((e) => WebviewCookie.fromJson(e.cast<String, dynamic>()))
            .toList() ??
        [];
  }
}
