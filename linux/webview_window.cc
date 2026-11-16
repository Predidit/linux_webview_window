//
// Created by boyan on 10/21/21.
//

#include "webview_window.h"

#include <libsoup/soup.h>

#include <utility>

// webkit_web_view_evaluate_javascript() replaced
// webkit_web_view_run_javascript() in WebKitGTK 2.40.
#if WEBKIT_MAJOR_VERSION < 2 || \
    (WEBKIT_MAJOR_VERSION == 2 && WEBKIT_MINOR_VERSION < 40)
#define WEBKIT_OLD_USED
#endif

namespace {

using Task = std::function<void()>;

gboolean run_task(gpointer user_data) {
  (*static_cast<Task *>(user_data))();
  return G_SOURCE_REMOVE;
}

void delete_task(gpointer user_data) { delete static_cast<Task *>(user_data); }

const char *non_null(const char *value) { return value ? value : ""; }

// Sends an event that carries nothing but the window id to dart.
void send_window_event(FlMethodChannel *channel, const char *method,
                       int64_t window_id) {
  g_autoptr(FlValue) args = fl_value_new_map();
  fl_value_set_string_take(args, "id", fl_value_new_int(window_id));
  fl_method_channel_invoke_method(channel, method, args, nullptr, nullptr,
                                  nullptr);
}

// Returning TRUE marks the failure as handled, which keeps WebKit from
// replacing the page with its TLS error page. The load itself is still
// aborted: certificate errors are not ignored.
gboolean on_load_failed_with_tls_errors(WebKitWebView *web_view,
                                        char *failing_uri,
                                        GTlsCertificate *certificate,
                                        GTlsCertificateFlags errors,
                                        gpointer user_data) {
  return TRUE;
}

// The web process is a separate process: when it dies the view simply stops
// responding. Log it so that this kind of freeze can be told apart from a
// problem in the plugin itself.
void on_web_process_terminated(WebKitWebView *web_view,
                               WebKitWebProcessTerminationReason reason,
                               gpointer user_data) {
  const char *reason_text = "terminated";
  switch (reason) {
    case WEBKIT_WEB_PROCESS_CRASHED:
      reason_text = "crashed";
      break;
    case WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT:
      reason_text = "exceeded memory limit";
      break;
    default:
      break;
  }
  g_warning("[webview_window] web process %s, uri: %s", reason_text,
            non_null(webkit_web_view_get_uri(web_view)));
}

FlValue *cookie_to_fl_value(SoupCookie *cookie) {
  FlValue *cookie_map = fl_value_new_map();

  fl_value_set_string_take(
      cookie_map, "name",
      fl_value_new_string(non_null(soup_cookie_get_name(cookie))));
  fl_value_set_string_take(
      cookie_map, "value",
      fl_value_new_string(non_null(soup_cookie_get_value(cookie))));
  fl_value_set_string_take(
      cookie_map, "domain",
      fl_value_new_string(non_null(soup_cookie_get_domain(cookie))));
  fl_value_set_string_take(
      cookie_map, "path",
      fl_value_new_string(non_null(soup_cookie_get_path(cookie))));

  // Session cookies have no expiry date.
  auto *expires = soup_cookie_get_expires(cookie);
  if (expires != nullptr) {
#if SOUP_CHECK_VERSION(3, 0, 0)
    auto expires_unix = static_cast<double>(g_date_time_to_unix(expires));
#else
    auto expires_unix = static_cast<double>(soup_date_to_time_t(expires));
#endif
    fl_value_set_string_take(cookie_map, "expires",
                             fl_value_new_float(expires_unix));
  } else {
    fl_value_set_string_take(cookie_map, "expires", fl_value_new_null());
  }

  fl_value_set_string_take(
      cookie_map, "httpOnly",
      fl_value_new_bool(soup_cookie_get_http_only(cookie)));
  fl_value_set_string_take(cookie_map, "secure",
                           fl_value_new_bool(soup_cookie_get_secure(cookie)));
  fl_value_set_string_take(cookie_map, "sessionOnly",
                           fl_value_new_bool(false));
  return cookie_map;
}

// |user_data| is the FlMethodCall to respond to, with a reference to release.
void on_get_cookies_finished(GObject *source, GAsyncResult *result,
                             gpointer user_data) {
  g_autoptr(FlMethodCall) call = FL_METHOD_CALL(user_data);

  GError *error = nullptr;
  GList *cookies = webkit_cookie_manager_get_cookies_finish(
      WEBKIT_COOKIE_MANAGER(source), result, &error);
  if (error != nullptr) {
    g_warning("[webview_window] failed to get cookies: %s", error->message);
    g_error_free(error);
  }

  g_autoptr(FlValue) cookie_list = fl_value_new_list();
  for (GList *l = cookies; l != nullptr; l = l->next) {
    fl_value_append_take(cookie_list,
                         cookie_to_fl_value(static_cast<SoupCookie *>(l->data)));
  }
  g_list_free_full(cookies, reinterpret_cast<GDestroyNotify>(soup_cookie_free));

  fl_method_call_respond_success(call, cookie_list, nullptr);
}

// |user_data| is the FlMethodCall to respond to, with a reference to release.
void on_evaluate_javascript_finished(GObject *object, GAsyncResult *result,
                                     gpointer user_data) {
  g_autoptr(FlMethodCall) call = FL_METHOD_CALL(user_data);

  GError *error = nullptr;
#ifdef WEBKIT_OLD_USED
  WebKitJavascriptResult *js_result = webkit_web_view_run_javascript_finish(
      WEBKIT_WEB_VIEW(object), result, &error);
  JSCValue *js_value =
      js_result ? webkit_javascript_result_get_js_value(js_result) : nullptr;
#else
  JSCValue *js_value = webkit_web_view_evaluate_javascript_finish(
      WEBKIT_WEB_VIEW(object), result, &error);
#endif

  if (js_value == nullptr) {
    fl_method_call_respond_error(call, "failed to evaluate javascript.",
                                 error ? error->message : "unknown error",
                                 nullptr, nullptr);
  } else {
    g_autofree gchar *json = jsc_value_to_json(js_value, 0);
    g_autoptr(FlValue) response = json ? fl_value_new_string(json) : nullptr;
    fl_method_call_respond_success(call, response, nullptr);
  }

  g_clear_error(&error);
#ifdef WEBKIT_OLD_USED
  if (js_result != nullptr) {
    webkit_javascript_result_unref(js_result);
  }
#else
  if (js_value != nullptr) {
    g_object_unref(js_value);
  }
#endif
}

}  // namespace

WebviewWindow::WebviewWindow(FlMethodChannel *method_channel, int64_t window_id,
                             std::function<void()> on_close_callback,
                             const std::string &title, int width, int height,
                             bool headless,
                             const std::vector<UserScript> &user_scripts,
                             const char *proxy_url)
    : method_channel_(method_channel),
      window_id_(window_id),
      on_close_callback_(std::move(on_close_callback)) {
  g_object_ref(method_channel_);

  window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  g_signal_connect(window_, "destroy", G_CALLBACK(OnWindowDestroy), this);
  gtk_window_set_title(GTK_WINDOW(window_), title.c_str());
  gtk_window_set_default_size(GTK_WINDOW(window_), width, height);
  gtk_window_set_position(GTK_WINDOW(window_), GTK_WIN_POS_CENTER);

  content_manager_ = webkit_user_content_manager_new();
  for (const auto &script : user_scripts) {
    auto *user_script = webkit_user_script_new(
        script.source.c_str(),
        script.for_all_frames ? WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES
                              : WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        script.injection_time == 0
            ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START
            : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
        nullptr, nullptr);
    webkit_user_content_manager_add_script(content_manager_, user_script);
    webkit_user_script_unref(user_script);
  }

  // Receives window.webkit.messageHandlers.msgToNative.postMessage(value).
  g_signal_connect(content_manager_, "script-message-received::msgToNative",
                   G_CALLBACK(OnScriptMessage), this);
  webkit_user_content_manager_register_script_message_handler(content_manager_,
                                                              "msgToNative");

  // The proxy is a property of the shared web context, so it applies to every
  // web view of the application.
  auto *context = webkit_web_context_get_default();
  auto *data_manager = webkit_web_context_get_website_data_manager(context);
  if (proxy_url != nullptr) {
    auto *proxy_settings = webkit_network_proxy_settings_new(proxy_url, nullptr);
    webkit_website_data_manager_set_network_proxy_settings(
        data_manager, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, proxy_settings);
    webkit_network_proxy_settings_free(proxy_settings);
  } else {
    webkit_website_data_manager_set_network_proxy_settings(
        data_manager, WEBKIT_NETWORK_PROXY_MODE_DEFAULT, nullptr);
  }

  // Keep our own reference: the pointer stays valid until the destructor, no
  // matter when GTK tears the widget tree down.
  webview_ = GTK_WIDGET(g_object_ref_sink(g_object_new(
      WEBKIT_TYPE_WEB_VIEW,
      "web-context", context,
      "user-content-manager", content_manager_,
      nullptr)));
  g_signal_connect(webview_, "load-failed-with-tls-errors",
                   G_CALLBACK(on_load_failed_with_tls_errors), nullptr);
  g_signal_connect(webview_, "web-process-terminated",
                   G_CALLBACK(on_web_process_terminated), nullptr);
  g_signal_connect(webview_, "create", G_CALLBACK(OnCreate), this);
  g_signal_connect(webview_, "load-changed", G_CALLBACK(OnLoadChanged), this);
  g_signal_connect(webview_, "decide-policy", G_CALLBACK(OnDecidePolicy), this);

  auto *settings = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(webview_));
  webkit_settings_set_javascript_can_open_windows_automatically(settings, true);
  default_user_agent_ = webkit_settings_get_user_agent(settings);
  gtk_container_add(GTK_CONTAINER(window_), webview_);

  if (!headless) {
    gtk_widget_show_all(window_);
    gtk_widget_grab_focus(webview_);
  }
}

WebviewWindow::~WebviewWindow() {
  // Queued tasks must not run against a dead object.
  *alive_ = false;

  // No signal may reach this object from now on.
  g_signal_handlers_disconnect_by_data(content_manager_, this);
  g_signal_handlers_disconnect_by_data(webview_, this);
  g_signal_handlers_disconnect_by_data(window_, this);

  if (!window_destroyed_) {
    gtk_widget_destroy(window_);
  }

  g_object_unref(webview_);
  g_object_unref(content_manager_);
  g_object_unref(method_channel_);
}

void WebviewWindow::PostTask(std::function<void()> task) {
  std::weak_ptr<bool> weak_alive = alive_;
  auto *guarded_task = new Task([weak_alive, task = std::move(task)]() {
    // Holding the lock keeps the flag itself valid even if the task ends up
    // deleting the window (see Close()).
    auto alive = weak_alive.lock();
    if (alive && *alive) {
      task();
    }
  });
  // G_PRIORITY_DEFAULT instead of the idle priority: idle sources only run
  // when nothing else is pending, so a busy main loop (rendering, input)
  // could postpone navigation or closing for an arbitrarily long time.
  g_idle_add_full(G_PRIORITY_DEFAULT, run_task, guarded_task, delete_task);
}

void WebviewWindow::OnWindowDestroy(GtkWidget *widget, gpointer user_data) {
  auto *self = static_cast<WebviewWindow *>(user_data);
  self->window_destroyed_ = true;

  int64_t window_id = self->window_id_;
  FlMethodChannel *channel =
      FL_METHOD_CHANNEL(g_object_ref(self->method_channel_));

  // The callback deletes |self|. Move it out first, so the std::function is
  // not destroyed while it is still running.
  auto on_close_callback = std::move(self->on_close_callback_);
  if (on_close_callback) {
    on_close_callback();
  }
  // |self| is gone. Only locals may be used below.

  send_window_event(channel, "onWindowClose", window_id);
  g_object_unref(channel);
}

void WebviewWindow::OnScriptMessage(WebKitUserContentManager *manager,
                                    WebKitJavascriptResult *js_result,
                                    gpointer user_data) {
  auto *self = static_cast<WebviewWindow *>(user_data);

  JSCValue *value = webkit_javascript_result_get_js_value(js_result);
  if (!jsc_value_is_string(value)) {
    return;
  }
  g_autofree gchar *message = jsc_value_to_string(value);

  g_autoptr(FlValue) args = fl_value_new_map();
  fl_value_set_string_take(args, "id", fl_value_new_int(self->window_id_));
  fl_value_set_string_take(args, "message",
                           fl_value_new_string(non_null(message)));
  fl_method_channel_invoke_method(self->method_channel_,
                                  "onJavascriptWebMessageReceived", args,
                                  nullptr, nullptr, nullptr);
}

// Handles window.open() and target="_blank" links.
//
// The handler of "create" must return either a brand new web view that is
// related to |web_view|, or null. Returning |web_view| itself makes WebKit
// treat the opener page as its own popup, which corrupts the page state and
// ends up freezing or crashing the web view. Since this plugin has a single
// view per window, open the requested url in the current view instead.
GtkWidget *WebviewWindow::OnCreate(WebKitWebView *web_view,
                                   WebKitNavigationAction *navigation_action,
                                   gpointer user_data) {
  auto *self = static_cast<WebviewWindow *>(user_data);

  // Popups opened by scripts without any user interaction (mostly ads) are
  // dropped, so they can not hijack the current page.
  if (!webkit_navigation_action_is_user_gesture(navigation_action)) {
    return nullptr;
  }

  auto *request = webkit_navigation_action_get_request(navigation_action);
  const gchar *uri = request ? webkit_uri_request_get_uri(request) : nullptr;
  if (uri != nullptr && *uri != '\0' && g_strcmp0(uri, "about:blank") != 0) {
    // Navigate() is deferred to the main loop, so the load does not start
    // while WebKit is still emitting this signal.
    self->Navigate(uri);
  }
  return nullptr;
}

void WebviewWindow::OnLoadChanged(WebKitWebView *web_view,
                                  WebKitLoadEvent load_event,
                                  gpointer user_data) {
  auto *self = static_cast<WebviewWindow *>(user_data);

  g_autoptr(FlValue) args = fl_value_new_map();
  fl_value_set_string_take(args, "id", fl_value_new_int(self->window_id_));
  fl_value_set_string_take(
      args, "canGoBack", fl_value_new_bool(webkit_web_view_can_go_back(web_view)));
  fl_value_set_string_take(
      args, "canGoForward",
      fl_value_new_bool(webkit_web_view_can_go_forward(web_view)));
  fl_method_channel_invoke_method(self->method_channel_, "onHistoryChanged",
                                  args, nullptr, nullptr, nullptr);

  switch (load_event) {
    case WEBKIT_LOAD_STARTED:
      send_window_event(self->method_channel_, "onNavigationStarted",
                        self->window_id_);
      break;
    case WEBKIT_LOAD_FINISHED:
      send_window_event(self->method_channel_, "onNavigationCompleted",
                        self->window_id_);
      break;
    default:
      break;
  }
}

// Reports navigations to dart. The decision itself is left to WebKit.
gboolean WebviewWindow::OnDecidePolicy(WebKitWebView *web_view,
                                       WebKitPolicyDecision *decision,
                                       WebKitPolicyDecisionType type,
                                       gpointer user_data) {
  auto *self = static_cast<WebviewWindow *>(user_data);

  if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
    auto *navigation_action =
        webkit_navigation_policy_decision_get_navigation_action(
            WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    auto *request = webkit_navigation_action_get_request(navigation_action);

    g_autoptr(FlValue) args = fl_value_new_map();
    fl_value_set_string_take(args, "id", fl_value_new_int(self->window_id_));
    fl_value_set_string_take(
        args, "url",
        fl_value_new_string(non_null(webkit_uri_request_get_uri(request))));
    fl_method_channel_invoke_method(self->method_channel_, "onUrlRequested",
                                    args, nullptr, nullptr, nullptr);
  }
  return FALSE;
}

void WebviewWindow::Navigate(const char *url) {
  std::string url_str(url);
  PostTask([this, url_str]() {
    webkit_web_view_load_uri(WEBKIT_WEB_VIEW(webview_), url_str.c_str());
  });
}

void WebviewWindow::AddScriptToExecuteOnDocumentCreated(
    const char *java_script) {
  std::string script_str(java_script);
  PostTask([this, script_str]() {
    auto *user_script = webkit_user_script_new(
        script_str.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(content_manager_, user_script);
    webkit_user_script_unref(user_script);
  });
}

void WebviewWindow::SetApplicationNameForUserAgent(
    const std::string &app_name) {
  auto *settings = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(webview_));
  webkit_settings_set_user_agent(settings,
                                 (default_user_agent_ + app_name).c_str());
}

void WebviewWindow::Close() {
  PostTask([this]() {
    if (!window_destroyed_) {
      // Emits "destroy", which deletes |this| and notifies dart.
      gtk_widget_destroy(window_);
    }
  });
}

void WebviewWindow::GoForward() {
  PostTask([this]() { webkit_web_view_go_forward(WEBKIT_WEB_VIEW(webview_)); });
}

void WebviewWindow::GoBack() {
  PostTask([this]() { webkit_web_view_go_back(WEBKIT_WEB_VIEW(webview_)); });
}

void WebviewWindow::Reload() {
  PostTask([this]() { webkit_web_view_reload(WEBKIT_WEB_VIEW(webview_)); });
}

void WebviewWindow::StopLoading() {
  PostTask(
      [this]() { webkit_web_view_stop_loading(WEBKIT_WEB_VIEW(webview_)); });
}

void WebviewWindow::GetAllCookies(FlMethodCall *call) {
  const gchar *uri = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(webview_));
  if (uri == nullptr || *uri == '\0') {
    // Nothing has been loaded yet, so there is no page to get cookies for.
    g_autoptr(FlValue) empty = fl_value_new_list();
    fl_method_call_respond_success(call, empty, nullptr);
    return;
  }

  auto *cookie_manager = webkit_web_context_get_cookie_manager(
      webkit_web_view_get_context(WEBKIT_WEB_VIEW(webview_)));
  // The method call is answered from the callback. Blocking here in a nested
  // main loop would re-enter the flutter engine and the other handlers of
  // this plugin, and would never return if the request is dropped.
  webkit_cookie_manager_get_cookies(cookie_manager, uri, nullptr,
                                    on_get_cookies_finished,
                                    g_object_ref(call));
}

void WebviewWindow::EvaluateJavaScript(const char *java_script,
                                       FlMethodCall *call) {
#ifdef WEBKIT_OLD_USED
  webkit_web_view_run_javascript(WEBKIT_WEB_VIEW(webview_), java_script,
                                 nullptr, on_evaluate_javascript_finished,
                                 g_object_ref(call));
#else
  webkit_web_view_evaluate_javascript(WEBKIT_WEB_VIEW(webview_), java_script,
                                      -1, nullptr, nullptr, nullptr,
                                      on_evaluate_javascript_finished,
                                      g_object_ref(call));
#endif
}
