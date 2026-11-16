#include "include/desktop_webview_window/desktop_webview_window_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "webview_window.h"

#define WEBVIEW_WINDOW_PLUGIN(obj)                                     \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), webview_window_plugin_get_type(), \
                              WebviewWindowPlugin))

struct _WebviewWindowPlugin {
  GObject parent_instance;
  FlMethodChannel *method_channel;
  std::map<int64_t, std::unique_ptr<WebviewWindow>> *windows;
  int64_t next_window_id;
};

G_DEFINE_TYPE(WebviewWindowPlugin, webview_window_plugin, g_object_get_type())

namespace {

const char kErrorCode[] = "0";

const char *string_arg(FlValue *args, const char *key) {
  return fl_value_get_string(fl_value_lookup_string(args, key));
}

// A method addressed to an existing window through its "viewId" argument.
struct WindowMethod {
  const char *name;
  // Either responds to |call| itself, or returns true to have it answered
  // with an empty success.
  bool (*invoke)(WebviewWindow *window, FlValue *args, FlMethodCall *call);
};

const WindowMethod kWindowMethods[] = {
    {"launch",
     [](WebviewWindow *window, FlValue *args, FlMethodCall *call) {
       window->Navigate(string_arg(args, "url"));
       return true;
     }},
    {"addScriptToExecuteOnDocumentCreated",
     [](WebviewWindow *window, FlValue *args, FlMethodCall *call) {
       window->AddScriptToExecuteOnDocumentCreated(
           string_arg(args, "javaScript"));
       return true;
     }},
    {"setApplicationNameForUserAgent",
     [](WebviewWindow *window, FlValue *args, FlMethodCall *call) {
       window->SetApplicationNameForUserAgent(
           string_arg(args, "applicationName"));
       return true;
     }},
    {"back",
     [](WebviewWindow *window, FlValue *args, FlMethodCall *call) {
       window->GoBack();
       return true;
     }},
    {"forward",
     [](WebviewWindow *window, FlValue *args, FlMethodCall *call) {
       window->GoForward();
       return true;
     }},
    {"reload",
     [](WebviewWindow *window, FlValue *args, FlMethodCall *call) {
       window->Reload();
       return true;
     }},
    {"stop",
     [](WebviewWindow *window, FlValue *args, FlMethodCall *call) {
       window->StopLoading();
       return true;
     }},
    {"close",
     [](WebviewWindow *window, FlValue *args, FlMethodCall *call) {
       window->Close();
       return true;
     }},
    {"getAllCookies",
     [](WebviewWindow *window, FlValue *args, FlMethodCall *call) {
       window->GetAllCookies(call);
       return false;
     }},
    {"evaluateJavaScript",
     [](WebviewWindow *window, FlValue *args, FlMethodCall *call) {
       window->EvaluateJavaScript(string_arg(args, "javaScriptString"), call);
       return false;
     }},
};

const WindowMethod *find_window_method(const gchar *name) {
  for (const auto &method : kWindowMethods) {
    if (strcmp(method.name, name) == 0) {
      return &method;
    }
  }
  return nullptr;
}

std::vector<UserScript> parse_user_scripts(FlValue *value) {
  std::vector<UserScript> user_scripts;
  if (value == nullptr || fl_value_get_type(value) != FL_VALUE_TYPE_LIST) {
    return user_scripts;
  }
  for (size_t i = 0; i < fl_value_get_length(value); ++i) {
    auto *script = fl_value_get_list_value(value, i);
    if (fl_value_get_type(script) != FL_VALUE_TYPE_MAP) {
      continue;
    }
    user_scripts.push_back(
        {string_arg(script, "source"),
         static_cast<int>(
             fl_value_get_int(fl_value_lookup_string(script, "injectionTime"))),
         fl_value_get_bool(fl_value_lookup_string(script, "forAllFrames"))});
  }
  return user_scripts;
}

void create_window(WebviewWindowPlugin *self, FlValue *args,
                   FlMethodCall *method_call) {
  auto width = fl_value_get_int(fl_value_lookup_string(args, "windowWidth"));
  auto height = fl_value_get_int(fl_value_lookup_string(args, "windowHeight"));
  auto *title = string_arg(args, "title");

  auto *headless_value = fl_value_lookup_string(args, "headless");
  bool headless = headless_value != nullptr &&
                  fl_value_get_type(headless_value) == FL_VALUE_TYPE_BOOL &&
                  fl_value_get_bool(headless_value);

  g_autofree gchar *proxy_url = nullptr;
  auto *proxy = fl_value_lookup_string(args, "proxy");
  if (proxy != nullptr && fl_value_get_type(proxy) == FL_VALUE_TYPE_MAP) {
    proxy_url = g_strdup_printf(
        "http://%s:%" G_GINT64_FORMAT, string_arg(proxy, "host"),
        fl_value_get_int(fl_value_lookup_string(proxy, "port")));
  }

  auto window_id = self->next_window_id++;
  // Every window keeps the plugin alive until it has been closed.
  g_object_ref(self);
  self->windows->emplace(
      window_id,
      std::make_unique<WebviewWindow>(
          self->method_channel, window_id,
          [self, window_id]() {
            self->windows->erase(window_id);
            g_object_unref(self);
          },
          title, width, height, headless,
          parse_user_scripts(fl_value_lookup_string(args, "userScripts")),
          proxy_url));

  g_autoptr(FlValue) result = fl_value_new_int(window_id);
  fl_method_call_respond_success(method_call, result, nullptr);
}

// Closes every window and wipes all cookies and website data.
void clear_all(WebviewWindowPlugin *self) {
  for (const auto &item : *self->windows) {
    item.second->Close();
  }
  // If application didn't create a webview, but we called
  // webkit_website_data_manager_clear, there will be a segment fault. To
  // avoid crash, we create a fake webview first and then clear all data.
  // It is released once the clear has finished.
  auto *web_view = GTK_WIDGET(g_object_ref_sink(webkit_web_view_new()));
  auto *context = webkit_web_view_get_context(WEBKIT_WEB_VIEW(web_view));
  webkit_website_data_manager_clear(
      webkit_web_context_get_website_data_manager(context),
      WEBKIT_WEBSITE_DATA_ALL, 0, nullptr,
      [](GObject *source, GAsyncResult *result, gpointer user_data) {
        GError *error = nullptr;
        if (!webkit_website_data_manager_clear_finish(
                WEBKIT_WEBSITE_DATA_MANAGER(source), result, &error)) {
          g_warning("[webview_window] failed to clear website data: %s",
                    error ? error->message : "unknown error");
        }
        g_clear_error(&error);
        g_object_unref(user_data);
      },
      web_view);
}

// Called when a method call is received from Flutter.
void handle_method_call(WebviewWindowPlugin *self, FlMethodCall *method_call) {
  const gchar *method = fl_method_call_get_name(method_call);
  FlValue *args = fl_method_call_get_args(method_call);

  if (strcmp(method, "clearAll") == 0) {
    clear_all(self);
    fl_method_call_respond_success(method_call, nullptr, nullptr);
    return;
  }

  bool is_create = strcmp(method, "create") == 0;
  const WindowMethod *window_method = find_window_method(method);
  if (!is_create && window_method == nullptr) {
    fl_method_call_respond_not_implemented(method_call, nullptr);
    return;
  }

  if (fl_value_get_type(args) != FL_VALUE_TYPE_MAP) {
    fl_method_call_respond_error(method_call, kErrorCode, "args is not map",
                                 nullptr, nullptr);
    return;
  }

  if (is_create) {
    create_window(self, args, method_call);
    return;
  }

  auto it = self->windows->find(
      fl_value_get_int(fl_value_lookup_string(args, "viewId")));
  if (it == self->windows->end()) {
    // Closing a window that is already gone (e.g. closed by the user a moment
    // ago) is not an error.
    if (strcmp(method, "close") == 0) {
      fl_method_call_respond_success(method_call, nullptr, nullptr);
    } else {
      fl_method_call_respond_error(method_call, kErrorCode,
                                   "can not found webview for viewId", nullptr,
                                   nullptr);
    }
    return;
  }

  if (window_method->invoke(it->second.get(), args, method_call)) {
    fl_method_call_respond_success(method_call, nullptr, nullptr);
  }
}

void method_call_cb(FlMethodChannel *channel, FlMethodCall *method_call,
                    gpointer user_data) {
  handle_method_call(WEBVIEW_WINDOW_PLUGIN(user_data), method_call);
}

}  // namespace

static void webview_window_plugin_dispose(GObject *object) {
  auto *self = WEBVIEW_WINDOW_PLUGIN(object);
  // dispose() may run more than once.
  delete self->windows;
  self->windows = nullptr;
  g_clear_object(&self->method_channel);
  G_OBJECT_CLASS(webview_window_plugin_parent_class)->dispose(object);
}

static void webview_window_plugin_class_init(WebviewWindowPluginClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = webview_window_plugin_dispose;
}

static void webview_window_plugin_init(WebviewWindowPlugin *self) {
  self->windows = new std::map<int64_t, std::unique_ptr<WebviewWindow>>();
}

void desktop_webview_window_plugin_register_with_registrar(
    FlPluginRegistrar *registrar) {
  WebviewWindowPlugin *plugin = WEBVIEW_WINDOW_PLUGIN(
      g_object_new(webview_window_plugin_get_type(), nullptr));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  plugin->method_channel =
      fl_method_channel_new(fl_plugin_registrar_get_messenger(registrar),
                            "webview_window", FL_METHOD_CODEC(codec));
  // The channel owns the reference handed over here, which keeps the plugin
  // alive for as long as the handler is installed.
  fl_method_channel_set_method_call_handler(plugin->method_channel,
                                            method_call_cb, plugin,
                                            g_object_unref);
}
