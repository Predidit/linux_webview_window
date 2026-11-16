//
// Created by boyan on 10/21/21.
//

#ifndef WEBVIEW_WINDOW_LINUX_WEBVIEW_WINDOW_H_
#define WEBVIEW_WINDOW_LINUX_WEBVIEW_WINDOW_H_

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct UserScript {
  std::string source;
  int injection_time;
  bool for_all_frames;
};

// A GTK window hosting a single WebKit web view.
//
// All methods must be called on the GTK main thread.
class WebviewWindow final {
 public:
  // |on_close_callback| runs when the GTK window is destroyed and is expected
  // to delete this object. |proxy_url| may be null.
  WebviewWindow(FlMethodChannel *method_channel, int64_t window_id,
                std::function<void()> on_close_callback,
                const std::string &title, int width, int height,
                bool headless,
                const std::vector<UserScript> &user_scripts,
                const char *proxy_url);

  ~WebviewWindow();

  WebviewWindow(const WebviewWindow &) = delete;
  WebviewWindow &operator=(const WebviewWindow &) = delete;

  void Navigate(const char *url);

  void AddScriptToExecuteOnDocumentCreated(const char *java_script);

  // Destroys the window, which in turn runs the close callback.
  void Close();

  void SetApplicationNameForUserAgent(const std::string &app_name);

  void GoBack();

  void GoForward();

  void Reload();

  void StopLoading();

  // Responds to |call| asynchronously with the cookies of the current page.
  void GetAllCookies(FlMethodCall *call);

  // Responds to |call| asynchronously with the JSON encoded result.
  void EvaluateJavaScript(const char *java_script, FlMethodCall *call);

 private:
  // GTK / WebKit signal handlers. |user_data| is the WebviewWindow.
  static void OnWindowDestroy(GtkWidget *widget, gpointer user_data);
  static void OnScriptMessage(WebKitUserContentManager *manager,
                              WebKitJavascriptResult *js_result,
                              gpointer user_data);
  static GtkWidget *OnCreate(WebKitWebView *web_view,
                             WebKitNavigationAction *navigation_action,
                             gpointer user_data);
  static void OnLoadChanged(WebKitWebView *web_view,
                            WebKitLoadEvent load_event, gpointer user_data);
  static gboolean OnDecidePolicy(WebKitWebView *web_view,
                                 WebKitPolicyDecision *decision,
                                 WebKitPolicyDecisionType type,
                                 gpointer user_data);

  // Runs |task| on the GTK main loop, unless this object has been destroyed
  // in the meantime. Tasks may safely touch |window_| and |webview_|.
  void PostTask(std::function<void()> task);

  FlMethodChannel *method_channel_;
  int64_t window_id_;
  std::function<void()> on_close_callback_;

  std::string default_user_agent_;

  GtkWidget *window_ = nullptr;
  // Strong reference, released in the destructor.
  GtkWidget *webview_ = nullptr;
  // Strong reference, released in the destructor.
  WebKitUserContentManager *content_manager_ = nullptr;

  // True once GTK has started destroying |window_|.
  bool window_destroyed_ = false;

  // Flipped to false in the destructor so that queued tasks become no-ops.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

#endif  // WEBVIEW_WINDOW_LINUX_WEBVIEW_WINDOW_H_
