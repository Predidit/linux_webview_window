//
// Created by boyan on 10/21/21.
//

#include "webview_window.h"

#include <cmath>
#include <utility>

#if WEBKIT_MAJOR_VERSION < 2 || \
    (WEBKIT_MAJOR_VERSION == 2 && WEBKIT_MINOR_VERSION < 40)
#define WEBKIT_OLD_USED
#endif

struct UserData {
    int64_t window_id;
    FlMethodChannel* method_channel_;
};

void handle_script_message(WebKitUserContentManager *manager, WebKitJavascriptResult *js_result, gpointer user_data) {
  JSCValue *value = webkit_javascript_result_get_js_value(js_result);
  if (jsc_value_is_string(value)) {
      char *message = jsc_value_to_string(value);
      auto *args = fl_value_new_map();
      UserData* r_user_data = static_cast<UserData*>(user_data);
      int64_t window_id = r_user_data->window_id;
      FlMethodChannel* method_channel_ = r_user_data->method_channel_;
      fl_value_set(args, fl_value_new_string("id"), fl_value_new_int(window_id));
      fl_value_set(args, fl_value_new_string("message"), fl_value_new_string(message));
      fl_method_channel_invoke_method(FL_METHOD_CHANNEL(method_channel_),
                                      "onJavascriptWebMessageReceived", args, nullptr,
                                      nullptr, nullptr);
      fl_value_unref(args);
      g_free(message);
  } else {
      g_print("Received non-string message\n");
  }
}

void get_cookies_callback(WebKitCookieManager *manager, GAsyncResult *res,
                          gpointer user_data) {
  CookieData *data = (CookieData *)user_data;
  GError *error = NULL;

  GList *cookies =
      webkit_cookie_manager_get_cookies_finish(manager, res, &error);
  if (error != NULL) {
    g_print("Error getting cookies: %s\n", error->message);
    g_error_free(error);
    data->cookies = NULL;
  } else {
    data->cookies = cookies;
  }

  g_main_loop_quit(data->loop);
}

GList *get_cookies_sync(WebKitWebView *web_view) {
  WebKitCookieManager *cookie_manager;
  GMainLoop *loop;
  CookieData data = {0};

  cookie_manager = webkit_web_context_get_cookie_manager(
      webkit_web_view_get_context(web_view));
  loop = g_main_loop_new(NULL, FALSE);
  data.loop = loop;

  const gchar *uri = webkit_web_view_get_uri(web_view);

  // Start the asynchronous operation
  webkit_cookie_manager_get_cookies(cookie_manager, uri, NULL,
                                    (GAsyncReadyCallback)get_cookies_callback,
                                    &data);

  // Run the main loop until the callback is called
  g_main_loop_run(loop);

  g_main_loop_unref(loop);

  return data.cookies;
}

namespace {

// Ingore all cerficate error
gboolean on_load_failed_with_tls_errors(WebKitWebView *web_view,
                                        char *failing_uri,
                                        GTlsCertificate *certificate,
                                        GTlsCertificateFlags errors,
                                        gpointer user_data) {
  return TRUE;
}

GtkWidget *on_create(WebKitWebView *web_view,
                     WebKitNavigationAction *navigation_action,
                     gpointer user_data) {
  return GTK_WIDGET(web_view);
}

void on_load_changed(WebKitWebView *web_view, WebKitLoadEvent load_event,
                     gpointer user_data) {
  auto *window = static_cast<WebviewWindow *>(user_data);
  window->OnLoadChanged(load_event);
}

gboolean decide_policy_cb(WebKitWebView *web_view,
                          WebKitPolicyDecision *decision,
                          WebKitPolicyDecisionType type, gpointer user_data) {
  auto *window = static_cast<WebviewWindow *>(user_data);
  return window->DecidePolicy(decision, type);
}

}  // namespace

WebviewWindow::WebviewWindow(FlMethodChannel *method_channel, int64_t window_id,
                             std::function<void()> on_close_callback,
                             const std::string &title, int width, int height,
                             bool headless,
                             const std::vector<UserScript> &user_scripts)
    : method_channel_(method_channel),
      window_id_(window_id),
      on_close_callback_(std::move(on_close_callback)),
      default_user_agent_(),
      is_valid_(std::make_shared<std::atomic<bool>>(true)) {
  g_object_ref(method_channel_);

  window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  g_signal_connect(G_OBJECT(window_), "destroy",
                   G_CALLBACK(+[](GtkWidget *, gpointer arg) {
                     auto *window = static_cast<WebviewWindow *>(arg);
                     if (window->on_close_callback_) {
                       window->on_close_callback_();
                     }
                     auto *args = fl_value_new_map();
                     fl_value_set(args, fl_value_new_string("id"),
                                  fl_value_new_int(window->window_id_));
                     fl_method_channel_invoke_method(
                         FL_METHOD_CHANNEL(window->method_channel_),
                         "onWindowClose", args, nullptr, nullptr, nullptr);
                   }),
                   this);
  gtk_window_set_title(GTK_WINDOW(window_), title.c_str());
  gtk_window_set_default_size(GTK_WINDOW(window_), width, height);
  gtk_window_set_position(GTK_WINDOW(window_), GTK_WIN_POS_CENTER);

  // initial web_view
  auto *manager = webkit_user_content_manager_new();
  for (const auto &script : user_scripts) {
    // g_print("checkpoint: inject time: %d\n", script.injection_time);
    // g_print("checkPoint: inject all iframe: %d\n", script.for_all_frames);
    webkit_user_content_manager_add_script(
        manager, webkit_user_script_new(
                     script.source.c_str(),
                     script.for_all_frames
                         ? WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES
                         : WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                     script.injection_time == 0
                         ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START
                         : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
                     nullptr, nullptr));
  }

  // 注册 window.webkit.messageHandlers.msgToNative.postMessage(value) 的回调函数
  UserData* user_data = new UserData{window_id, method_channel_};
  g_signal_connect (manager, "script-message-received::msgToNative",
                  G_CALLBACK (handle_script_message), user_data);
  webkit_user_content_manager_register_script_message_handler (manager, "msgToNative");

  webview_ = webkit_web_view_new_with_user_content_manager(manager);
  g_signal_connect(G_OBJECT(webview_), "load-failed-with-tls-errors",
                   G_CALLBACK(on_load_failed_with_tls_errors), this);
  g_signal_connect(G_OBJECT(webview_), "create", G_CALLBACK(on_create), this);
  g_signal_connect(G_OBJECT(webview_), "load-changed",
                   G_CALLBACK(on_load_changed), this);
  g_signal_connect(G_OBJECT(webview_), "decide-policy",
                   G_CALLBACK(decide_policy_cb), this);

  auto settings = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(webview_));
  webkit_settings_set_javascript_can_open_windows_automatically(settings, true);
  default_user_agent_ = webkit_settings_get_user_agent(settings);
  gtk_container_add(GTK_CONTAINER(window_), webview_);

  if (!headless) {
    gtk_widget_show_all(GTK_WIDGET(window_));
    gtk_widget_grab_focus(GTK_WIDGET(webview_));
  }
}

WebviewWindow::~WebviewWindow() {
  // Mark as invalid to prevent callbacks from accessing this object
  is_valid_->store(false);
  
  // Clear pending JavaScript requests
  {
    std::lock_guard<std::mutex> lock(js_queue_mutex_);
    while (!js_queue_.empty()) {
      auto request = js_queue_.front();
      js_queue_.pop();
      
      // Respond with error for pending requests
      g_idle_add([](gpointer user_data) -> gboolean {
        auto *call = static_cast<FlMethodCall *>(user_data);
        fl_method_call_respond_error(call, "window_closed", 
                                     "WebView window was closed", nullptr, nullptr);
        g_object_unref(call);
        return G_SOURCE_REMOVE;
      }, request.call);
    }
  }
  
  g_object_unref(method_channel_);
  printf("~WebviewWindow\n");
}

void WebviewWindow::Navigate(const char *url) {
  webkit_web_view_load_uri(WEBKIT_WEB_VIEW(webview_), url);
}

void WebviewWindow::RunJavaScriptWhenContentReady(const char *java_script) {
  auto *manager =
      webkit_web_view_get_user_content_manager(WEBKIT_WEB_VIEW(webview_));
  webkit_user_content_manager_add_script(
      manager,
      webkit_user_script_new(java_script, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                             WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                             nullptr, nullptr));
}

void WebviewWindow::SetApplicationNameForUserAgent(
    const std::string &app_name) {
  auto *setting = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(webview_));
  webkit_settings_set_user_agent(setting,
                                 (default_user_agent_ + app_name).c_str());
}

void WebviewWindow::Close() { gtk_widget_destroy(GTK_WIDGET(window_)); }

void WebviewWindow::OnLoadChanged(WebKitLoadEvent load_event) {
  // notify history changed event.
  {
    auto can_go_back = webkit_web_view_can_go_back(WEBKIT_WEB_VIEW(webview_));
    auto can_go_forward =
        webkit_web_view_can_go_forward(WEBKIT_WEB_VIEW(webview_));
    auto *args = fl_value_new_map();
    fl_value_set(args, fl_value_new_string("id"), fl_value_new_int(window_id_));
    fl_value_set(args, fl_value_new_string("canGoBack"),
                 fl_value_new_bool(can_go_back));
    fl_value_set(args, fl_value_new_string("canGoForward"),
                 fl_value_new_bool(can_go_forward));
    fl_method_channel_invoke_method(FL_METHOD_CHANNEL(method_channel_),
                                    "onHistoryChanged", args, nullptr, nullptr,
                                    nullptr);
  }

  // notify load start/finished event.
  switch (load_event) {
    case WEBKIT_LOAD_STARTED: {
      auto *args = fl_value_new_map();
      fl_value_set(args, fl_value_new_string("id"),
                   fl_value_new_int(window_id_));
      fl_method_channel_invoke_method(FL_METHOD_CHANNEL(method_channel_),
                                      "onNavigationStarted", args, nullptr,
                                      nullptr, nullptr);
      break;
    }
    case WEBKIT_LOAD_FINISHED: {
      auto *args = fl_value_new_map();
      fl_value_set(args, fl_value_new_string("id"),
                   fl_value_new_int(window_id_));
      fl_method_channel_invoke_method(FL_METHOD_CHANNEL(method_channel_),
                                      "onNavigationCompleted", args, nullptr,
                                      nullptr, nullptr);
      break;
    }
    default:
      break;
  }
}

void WebviewWindow::GoForward() {
  webkit_web_view_go_forward(WEBKIT_WEB_VIEW(webview_));
}

void WebviewWindow::GoBack() {
  webkit_web_view_go_back(WEBKIT_WEB_VIEW(webview_));
}

void WebviewWindow::Reload() {
  webkit_web_view_reload(WEBKIT_WEB_VIEW(webview_));
}

void WebviewWindow::StopLoading() {
  webkit_web_view_stop_loading(WEBKIT_WEB_VIEW(webview_));
}

FlValue *WebviewWindow::GetAllCookies() {
  GList *cookies = get_cookies_sync(WEBKIT_WEB_VIEW(webview_));

  g_autoptr(FlValue) fl_cookie_list = fl_value_new_list();

  FlValue* cookie_list = fl_value_ref(fl_cookie_list);

  for (GList *l = cookies; l; l = l->next) {
    SoupCookie *cookie = (SoupCookie *)l->data;
    g_autoptr(FlValue) cookie_map = fl_value_new_map();

    fl_value_set_string_take(cookie_map, "name",
                             fl_value_new_string(soup_cookie_get_name(cookie)));
    fl_value_set_string_take(
        cookie_map, "value",
        fl_value_new_string(soup_cookie_get_value(cookie)));
    fl_value_set_string_take(
        cookie_map, "domain",
        fl_value_new_string(soup_cookie_get_domain(cookie)));
    fl_value_set_string_take(cookie_map, "path",
                             fl_value_new_string(soup_cookie_get_path(cookie)));

    gdouble expires = g_date_time_get_seconds(soup_cookie_get_expires(cookie));

    if (expires >= 0) {
      fl_value_set_string_take(cookie_map, "expires",
                               fl_value_new_float(expires));
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

    fl_value_append(cookie_list, cookie_map);
    soup_cookie_free(cookie);
  }

  g_free(cookies);

  return cookie_list;
}

gboolean WebviewWindow::DecidePolicy(WebKitPolicyDecision *decision,
                                     WebKitPolicyDecisionType type) {
  if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
    auto *navigation_decision = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
    auto *navigation_action =
        webkit_navigation_policy_decision_get_navigation_action(
            navigation_decision);
    auto *request = webkit_navigation_action_get_request(navigation_action);
    auto *uri = webkit_uri_request_get_uri(request);
    auto *args = fl_value_new_map();
    fl_value_set(args, fl_value_new_string("id"), fl_value_new_int(window_id_));
    fl_value_set(args, fl_value_new_string("url"), fl_value_new_string(uri));
    fl_method_channel_invoke_method(FL_METHOD_CHANNEL(method_channel_),
                                    "onUrlRequested", args, nullptr, nullptr,
                                    nullptr);
  }
  return false;
}

struct EvalJSResponse {
  FlMethodCall *call;
  gchar *result_string;
  gchar *error_code;
  gchar *error_message;
  gboolean is_error;
  WebviewWindow *window;
  std::shared_ptr<std::atomic<bool>> window_valid;
};

static gboolean send_eval_response_on_main_thread(gpointer user_data) {
  auto *response = static_cast<EvalJSResponse *>(user_data);
  
  // Send response synchronously
  if (response->is_error) {
    fl_method_call_respond_error(
        response->call, 
        response->error_code ? response->error_code : "eval_error",
        response->error_message ? response->error_message : "unknown error",
        nullptr, 
        nullptr);
  } else {
    if (response->result_string) {
      g_autoptr(FlValue) fl_result = fl_value_new_string(response->result_string);
      fl_method_call_respond_success(response->call, fl_result, nullptr);
    } else {
      fl_method_call_respond_success(response->call, nullptr, nullptr);
    }
  }
  
  g_object_unref(response->call);
  g_free(response->result_string);
  g_free(response->error_code);
  g_free(response->error_message);
  
  auto *window = response->window;
  auto window_valid = response->window_valid;
  delete response;
  
  // Trigger next request processing
  if (window && window_valid && window_valid->load()) {
    window->ProcessNextJSRequest();
  }
  
  return G_SOURCE_REMOVE;
}

void WebviewWindow::EvaluateJavaScript(const char *java_script,
                                       FlMethodCall *call) {
  std::lock_guard<std::mutex> lock(js_queue_mutex_);
  
  // Add request to queue
  js_queue_.push({std::string(java_script), call});
  g_object_ref(call);
  
  // If not currently executing, start processing
  if (!js_executing_) {
    js_executing_ = true;
    // Use g_idle_add to process on main thread
    g_idle_add([](gpointer user_data) -> gboolean {
      auto *window = static_cast<WebviewWindow *>(user_data);
      window->ProcessNextJSRequest();
      return G_SOURCE_REMOVE;
    }, this);
  }
}

void WebviewWindow::ProcessNextJSRequest() {
  JSRequest request;
  bool has_request = false;
  
  {
    std::lock_guard<std::mutex> lock(js_queue_mutex_);
    if (js_queue_.empty()) {
      js_executing_ = false;
      return;
    }
    request = js_queue_.front();
    js_queue_.pop();
    has_request = true;
  }
  
  if (has_request) {
    ExecuteJavaScriptInternal(request.script.c_str(), request.call);
  }
}

void WebviewWindow::ExecuteJavaScriptInternal(const char *java_script,
                                              FlMethodCall *call) {
  // Create a context struct with lifecycle management
  struct CallbackContext {
    FlMethodCall *call;
    WebviewWindow *window;
    std::shared_ptr<std::atomic<bool>> is_valid;
  };
  auto *context = new CallbackContext{call, this, is_valid_};
  
#ifdef WEBKIT_OLD_USED
  webkit_web_view_run_javascript(
#else
  webkit_web_view_evaluate_javascript(
#endif
      WEBKIT_WEB_VIEW(webview_), java_script,
#ifndef WEBKIT_OLD_USED
      -1, nullptr, nullptr,
#endif
      nullptr,
      [](GObject *object, GAsyncResult *result, gpointer user_data) {
        auto *context = static_cast<CallbackContext *>(user_data);
        auto *call = context->call;
        auto *window = context->window;
        auto is_valid = context->is_valid;
        delete context;
        
        // Check if window is still valid
        if (!is_valid || !is_valid->load()) {
          g_warning("EvaluateJavaScript callback: window destroyed");
          if (call) {
            g_idle_add([](gpointer user_data) -> gboolean {
              auto *call = static_cast<FlMethodCall *>(user_data);
              fl_method_call_respond_error(call, "window_destroyed", 
                                           "WebView window was destroyed", nullptr, nullptr);
              g_object_unref(call);
              return G_SOURCE_REMOVE;
            }, call);
          }
          return;
        }
        
        GError *error = nullptr;
        gchar *result_string = nullptr;
        gboolean is_error = FALSE;
        gchar *error_code = nullptr;
        gchar *error_message = nullptr;
        
        if (!call) {
          g_warning("EvaluateJavaScript callback: call is null");
          if (window && is_valid->load()) {
            window->ProcessNextJSRequest();
          }
          return;
        }
        
#ifdef WEBKIT_OLD_USED
        auto *js_result = webkit_web_view_run_javascript_finish(
            WEBKIT_WEB_VIEW(object), result, &error);
        if (!js_result) {
          is_error = TRUE;
          error_code = g_strdup("eval_failed");
          error_message = g_strdup(error ? error->message : "JavaScript execution failed");
          if (error) g_error_free(error);
        } else {
          JSCValue *value = webkit_javascript_result_get_js_value(js_result);
#else
        JSCValue *value = nullptr;
        
        // Try to get the result, handle potential errors
        value = webkit_web_view_evaluate_javascript_finish(
            WEBKIT_WEB_VIEW(object), result, &error);
            
        if (error) {
          is_error = TRUE;
          error_code = g_strdup("eval_failed");
          error_message = g_strdup(error->message ? error->message : "JavaScript execution failed");
          g_warning("JavaScript execution error: %s", error->message ? error->message : "unknown");
          g_error_free(error);
        } else if (!value) {
          is_error = TRUE;
          error_code = g_strdup("eval_failed");
          error_message = g_strdup("JavaScript execution returned null value");
          g_warning("JavaScript execution returned null value");
        } else {
#endif
          // Process the JavaScript value with try-catch protection
          if (!value) {
            result_string = g_strdup("null");
          } else {
            // Use exception handling for type checks
            JSCException *exception = jsc_context_get_exception(jsc_value_get_context(value));
            if (exception) {
              is_error = TRUE;
              error_code = g_strdup("eval_failed");
              char *exc_msg = jsc_exception_to_string(exception);
              error_message = g_strdup(exc_msg ? exc_msg : "JavaScript exception");
              g_warning("JavaScript exception: %s", exc_msg ? exc_msg : "unknown");
              g_free(exc_msg);
              jsc_context_clear_exception(jsc_value_get_context(value));
            } else if (jsc_value_is_null(value)) {
              result_string = g_strdup("null");
            } else if (jsc_value_is_undefined(value)) {
              result_string = g_strdup("undefined");
            } else if (jsc_value_is_boolean(value)) {
              result_string = g_strdup(jsc_value_to_boolean(value) ? "true" : "false");
            } else if (jsc_value_is_number(value)) {
              gdouble num = jsc_value_to_double(value);
              if (std::isnan(num)) {
                result_string = g_strdup("NaN");
              } else if (std::isinf(num)) {
                result_string = g_strdup(num > 0 ? "Infinity" : "-Infinity");
              } else {
                result_string = g_strdup_printf("%g", num);
              }
            } else if (jsc_value_is_string(value)) {
              char *str = jsc_value_to_string(value);
              if (str) {
                result_string = g_strdup_printf("\"%s\"", str);
                g_free(str);
              } else {
                result_string = g_strdup("\"\"");
              }
            } else if (jsc_value_is_object(value) || jsc_value_is_array(value)) {
              // Try JSON serialization first
              result_string = jsc_value_to_json(value, 0);
              if (!result_string) {
                // If JSON conversion fails, try toString as fallback
                char *str = jsc_value_to_string(value);
                result_string = g_strdup(str ? str : "[Object]");
                if (str) g_free(str);
              }
            } else {
              // For functions, symbols, and other unsupported types
              char *str = jsc_value_to_string(value);
              result_string = g_strdup(str ? str : "undefined");
              if (str) g_free(str);
            }
          }
          
#ifdef WEBKIT_OLD_USED
          webkit_javascript_result_unref(js_result);
#endif
        }
        
        if (!is_error && !result_string) {
          result_string = g_strdup("null");
        }
        
        if (is_error && !error_code) {
          error_code = g_strdup("eval_failed");
        }
        
        if (is_error && !error_message) {
          error_message = g_strdup("Unknown JavaScript execution error");
        }
        
        auto *response = new EvalJSResponse{
            call,
            result_string,
            error_code,
            error_message,
            is_error,
            window,
            is_valid
        };
        
        g_idle_add(send_eval_response_on_main_thread, response);
      },
      context);
}