import 'package:flutter/foundation.dart';

import 'cookie.dart';

typedef OnHistoryChangedCallback = void Function(
    bool canGoBack, bool canGoForward);

/// Callback when WebView start to load a URL.
/// [url] is the URL string.
///
/// The returned value is not used: the navigation always proceeds.
typedef OnUrlRequestCallback = bool Function(String url);

/// Callback when WebView receives a web message
/// [message] constains the webmessage
typedef OnWebMessageReceivedCallback = void Function(String message);

abstract class Webview {
  /// Completes when the webview window has been closed, either by [close] or
  /// by the user.
  Future<void> get onClose;

  ///  true if the webview is currently loading a page.
  ValueListenable<bool> get isNavigating;

  /// Navigates to the given URL.
  void launch(String url);

  /// Adds a script that runs in the top frame of every page, before the page
  /// starts to load.
  void addScriptToExecuteOnDocumentCreated(String javaScript);

  /// Append a string to the webview's user-agent.
  Future<void> setApplicationNameForUserAgent(String applicationName);

  /// Navigate to the previous page in the history.
  Future<void> back();

  /// Navigate to the next page in the history.
  Future<void> forward();

  /// Reload the current page.
  Future<void> reload();

  /// Stop all navigations and pending resource fetches.
  Future<void> stop();

  /// Register a callback that will be invoked when the webview history changes.
  void setOnHistoryChangedCallback(OnHistoryChangedCallback? callback);

  void setOnUrlRequestCallback(OnUrlRequestCallback? callback);

  /// Register a callback for messages posted by the page through
  /// `window.webkit.messageHandlers.msgToNative.postMessage(message)`.
  void addOnWebMessageReceivedCallback(OnWebMessageReceivedCallback callback);

  void removeOnWebMessageReceivedCallback(
      OnWebMessageReceivedCallback callback);

  /// Close the web view window.
  void close();

  /// Evaluates JavaScript in the web view and returns the result encoded as
  /// JSON.
  Future<String?> evaluateJavaScript(String javaScript);

  /// Returns the cookies that apply to the current page.
  Future<List<WebviewCookie>> getAllCookies();
}
