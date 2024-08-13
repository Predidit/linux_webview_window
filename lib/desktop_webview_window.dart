// You have generated a new plugin project without
// specifying the `--platforms` flag. A plugin project supports no platforms is generated.
// To add platforms, run `flutter create -t plugin --platforms <platforms> .` under the same
// directory. You can also find a detailed instruction on how to add platforms in the `pubspec.yaml` at https://flutter.dev/docs/development/packages-and-plugins/developing-packages#plugin-platforms.

import 'dart:async';
import 'dart:io';
import 'dart:ffi';
import 'dart:math';
import 'package:ffi/ffi.dart';

import 'package:flutter/services.dart';
import 'package:path/path.dart' as p;

import 'src/create_configuration.dart';
import 'src/message_channel.dart';
import 'src/webview.dart';
import 'src/webview_impl.dart';
import 'package:flutter/material.dart';
import 'package:texture_rgba_renderer/texture_rgba_renderer.dart';

export 'src/create_configuration.dart';
export 'src/title_bar.dart';
export 'src/webview.dart';

final List<WebviewImpl> _webviews = [];

class WebviewWindow {
  static const MethodChannel _channel = MethodChannel('webview_window');

  static const _otherIsolateMessageHandler = ClientMessageChannel();

  static bool _inited = false;

  static void _init() {
    if (_inited) {
      return;
    }
    _inited = true;
    _channel.setMethodCallHandler((call) async {
      try {
        return await _handleMethodCall(call);
      } catch (e, s) {
        debugPrint("method: ${call.method} args: ${call.arguments}");
        debugPrint('handleMethodCall error: $e $s');
      }
    });
    _otherIsolateMessageHandler.setMessageHandler((call) async {
      try {
        return await _handleOtherIsolateMethodCall(call);
      } catch (e, s) {
        debugPrint('_handleOtherIsolateMethodCall error: $e $s');
      }
    });
  }

  /// Check if WebView runtime is available on the current devices.
  static Future<bool> isWebviewAvailable() async {
    if (Platform.isWindows) {
      final ret = await _channel.invokeMethod<bool>('isWebviewAvailable');
      return ret == true;
    }
    return true;
  }

  static Future<Webview> create({
    CreateConfiguration? configuration,
  }) async {
    configuration ??= CreateConfiguration.platform();
    _init();
    final viewId = await _channel.invokeMethod(
      "create",
      configuration.toMap(),
    ) as int;
    final webview = WebviewImpl(viewId, _channel);
    _webviews.add(webview);
    return webview;
  }

  static Future<dynamic> _handleOtherIsolateMethodCall(MethodCall call) async {
    final webViewId = call.arguments['webViewId'] as int;
    final webView = _webviews
        .cast<WebviewImpl?>()
        .firstWhere((w) => w?.viewId == webViewId, orElse: () => null);
    if (webView == null) {
      return;
    }
    switch (call.method) {
      case 'onBackPressed':
        await webView.back();
        break;
      case 'onForwardPressed':
        await webView.forward();
        break;
      case 'onRefreshPressed':
        await webView.reload();
        break;
      case 'onStopPressed':
        await webView.stop();
        break;
      case 'onClosePressed':
        webView.close();
        break;
    }
  }

  static Future<dynamic> _handleMethodCall(MethodCall call) async {
    final args = call.arguments as Map;
    final viewId = args['id'] as int;
    final webview = _webviews
        .cast<WebviewImpl?>()
        .firstWhere((e) => e?.viewId == viewId, orElse: () => null);
    assert(webview != null);
    if (webview == null) {
      return;
    }
    switch (call.method) {
      case "onWindowClose":
        _webviews.remove(webview);
        webview.onClosed();
        break;
      case "onJavaScriptMessage":
        webview.onJavaScriptMessage(args['name'], args['body']);
        break;
      case "runJavaScriptTextInputPanelWithPrompt":
        return webview.onRunJavaScriptTextInputPanelWithPrompt(
          args['prompt'],
          args['defaultText'],
        );
      case "onHistoryChanged":
        webview.onHistoryChanged(args['canGoBack'], args['canGoForward']);
        await _otherIsolateMessageHandler.invokeMethod('onHistoryChanged', {
          'webViewId': viewId,
          'canGoBack': args['canGoBack'] as bool,
          'canGoForward': args['canGoForward'] as bool,
        });
        break;
      case "onNavigationStarted":
        webview.onNavigationStarted();
        await _otherIsolateMessageHandler.invokeMethod('onNavigationStarted', {
          'webViewId': viewId,
        });
        break;
      case "onUrlRequested":
        final url = args['url'] as String;
        final ret = webview.notifyUrlChanged(url);
        await _otherIsolateMessageHandler.invokeMethod('onUrlRequested', {
          'webViewId': viewId,
          'url': url,
        });
        return ret;
      case "onWebMessageReceived":
        final message = args['message'] as String;
        webview.notifyWebMessageReceived(message);
        await _otherIsolateMessageHandler.invokeMethod('onWebMessageReceived', {
          'webViewId': viewId,
          'message': message,
        });
        break;
      case "onJavascriptWebMessageReceived":
        final message = args['message'] as String;
        webview.notifyWebMessageReceived(message);
        break;
      case "onNavigationCompleted":
        webview.onNavigationCompleted();
        await _otherIsolateMessageHandler
            .invokeMethod('onNavigationCompleted', {
          'webViewId': viewId,
        });
        break;
      default:
        return;
    }
  }

  /// Clear all cookies and storage.
  static Future<void> clearAll({
    String userDataFolderWindows = 'webview_window_WebView2',
  }) async {
    await _channel.invokeMethod('clearAll');

    // FIXME(boyan01) Move the logic to windows platform if WebView2 provider a way to clean caches.
    // https://docs.microsoft.com/en-us/microsoft-edge/webview2/concepts/user-data-folder#create-user-data-folders
    if (Platform.isWindows) {
      final Directory webview2Dir;
      if (p.isAbsolute(userDataFolderWindows)) {
        webview2Dir = Directory(userDataFolderWindows);
      } else {
        webview2Dir = Directory(p.join(
            p.dirname(Platform.resolvedExecutable), userDataFolderWindows));
      }

      if (await (webview2Dir.exists())) {
        for (var i = 0; i <= 4; i++) {
          try {
            await webview2Dir.delete(recursive: true);
            break;
          } catch (e) {
            debugPrint("delete cache failed. retring.... $e");
          }
          // wait to ensure all web window has been closed and file handle has been release.
          await Future.delayed(const Duration(seconds: 1));
        }
      }
    }
  }
}

class WebviewTexture extends StatefulWidget {
  const WebviewTexture({super.key});

  @override
  State<WebviewTexture> createState() => _WebviewTextureState(); 
}

class _WebviewTextureState extends State<WebviewTexture> {
  final _textureRgbaRendererPlugin = TextureRgbaRenderer(); // 创建纹理渲染插件实例
  int textureId = -1; // 初始化纹理 ID
  int height = 768; // 初始化高度
  int width = 1377; // 初始化宽度
  int cnt = 0; // 初始化计数器
  var key = 0; // 初始化键值
  int texturePtr = 0; // 初始化纹理指针
  final random = Random(); // 创建随机数生成器实例
  Uint8List? data; // 初始化数据
  Timer? _timer; // 初始化定时器
  int time = 0; // 初始化时间
  int method = 0; // 初始化方法
  final strideAlign = Platform.isMacOS ? 64 : 1; // 根据平台设置步幅对齐

  @override
  void initState() {
    super.initState();
    // 创建纹理并获取纹理 ID
    _textureRgbaRendererPlugin.createTexture(key).then((textureId) {
      if (textureId != -1) {
        debugPrint("Texture register success, textureId=$textureId");
        // 获取纹理指针
        _textureRgbaRendererPlugin.getTexturePtr(key).then((value) {
          debugPrint("texture ptr: ${value.toRadixString(16)}");
          setState(() {
            texturePtr = value; // 更新纹理指针
          });
        });
        setState(() {
          this.textureId = textureId; // 更新纹理 ID
        });
      } else {
        return;
      }
    });
  }

  void start(int methodId) {
    debugPrint("start mockPic");
    method = methodId; // 设置方法 ID
    final rowBytes = (width * 4 + strideAlign - 1) & (~(strideAlign - 1)); // 计算每行字节数
    final picDataLength = rowBytes * height; // 计算图片数据长度
    debugPrint('REMOVE ME =============================== rowBytes $rowBytes');
    _timer?.cancel(); // 取消之前的定时器
    // 60 fps
    _timer = Timer.periodic(const Duration(milliseconds: 1000 ~/ 60), (timer) async {
      if (methodId == 0) {
        // 方法1：使用 MethodChannel
        data = mockPicture(width, height, rowBytes, picDataLength); // 生成模拟图片数据
        final t1 = DateTime.now().microsecondsSinceEpoch; // 获取当前时间戳
        final res = await _textureRgbaRendererPlugin.onRgba(
            key, data!, height, width, strideAlign); // 通过插件渲染图片
        final t2 = DateTime.now().microsecondsSinceEpoch; // 获取当前时间戳
        setState(() {
          time = t2 - t1; // 计算渲染时间
        });
        if (!res) {
          debugPrint("WARN: render failed"); // 渲染失败警告
        }
      } else {
        final dataPtr = mockPicturePtr(width, height, rowBytes, picDataLength); // 生成模拟图片指针
        // 方法2：使用本地 FFI
        final t1 = DateTime.now().microsecondsSinceEpoch; // 获取当前时间戳
        Native.instance.onRgba(Pointer.fromAddress(texturePtr).cast<Void>(),
            dataPtr, picDataLength, width, height, strideAlign); // 通过本地代码渲染图片
        final t2 = DateTime.now().microsecondsSinceEpoch; // 获取当前时间戳
        setState(() {
          time = t2 - t1; // 计算渲染时间
        });
        malloc.free(dataPtr); // 释放内存
      }
    });
  }

  @override
  void dispose() {
    _timer?.cancel(); // 取消定时器
    if (key != -1) {
      _textureRgbaRendererPlugin.closeTexture(key); // 关闭纹理
    }
    super.dispose();
  }

  Uint8List mockPicture(int width, int height, int rowBytes, int length) {
    // 生成模拟图片数据
    final pic = List.generate(length, (index) {
      final r = index / rowBytes; // 计算行
      final c = (index % rowBytes) / 4; // 计算列
      final p = index & 0x03; // 计算像素位置
      if (c > 20 && c < 30) {
        if (r > 20 && r < 25) {
          if (p == 0 || p == 3) {
            return 255; // 设置红色和透明通道
          } else {
            return 0; // 设置绿色和蓝色通道
          }
        }
        if (r > 40 && r < 45) {
          if (p == 1 || p == 3) {
            return 255; // 设置绿色和透明通道
          } else {
            return 0; // 设置红色和蓝色通道
          }
        }
        if (r > 60 && r < 65) {
          if (p == 2 || p == 3) {
            return 255; // 设置蓝色和透明通道
          } else {
            return 0; // 设置红色和绿色通道
          }
        }
      }
      return 255; // 默认设置为白色
    });
    return Uint8List.fromList(pic); // 返回无符号字节列表
  }

  Pointer<Uint8> mockPicturePtr(int width, int height, int rowBytes, int length) {
    // 生成模拟图片指针
    final pic = List.generate(length, (index) {
      final r = index / rowBytes; // 计算行
      final c = (index % rowBytes) / 4; // 计算列
      final p = index & 0x03; // 计算像素位置
      final edgeH = (c >= 0 && c < 10) || ((c >= width - 10) && c < width); // 判断水平边缘
      final edgeW = (r >= 0 && r < 10) || ((r >= height - 10) && r < height); // 判断垂直边缘
      if (edgeH || edgeW) {
        if (p == 0 || p == 3) {
          return 255; // 设置红色和透明通道
        } else {
          return 0; // 设置绿色和蓝色通道
        }
      }
      return 255; // 默认设置为白色
    });
    final picAddr = malloc.allocate(pic.length).cast<Uint8>(); // 分配内存
    final list = picAddr.asTypedList(pic.length); // 转换为类型化列表
    list.setRange(0, pic.length, pic); // 设置范围
    return picAddr; // 返回指针
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        body: Column(
          crossAxisAlignment: CrossAxisAlignment.center,
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Expanded(
              child: textureId == -1
                  ? const Offstage() // 如果纹理 ID 为 -1，则隐藏
                  : Padding(
                      padding: const EdgeInsets.all(8.0),
                      child: Container(
                          alignment: Alignment.center,
                          decoration: const BoxDecoration(color: Colors.blue), // 设置背景颜色
                          child: Texture(textureId: textureId)), // 显示纹理
                    ),
            ),
            Text(
                "texture id: $textureId, texture memory address: ${texturePtr.toRadixString(16)}"), // 显示纹理 ID 和内存地址
            TextButton.icon(
              label: const Text("play with texture (method channel API)"),
              icon: const Icon(Icons.play_arrow),
              onPressed: () => start(0), // 使用方法通道 API 播放纹理
            ),
            TextButton.icon(
              label: const Text("play with texture (native API, faster)"),
              icon: const Icon(Icons.play_arrow),
              onPressed: () => start(1), // 使用本地 API 播放纹理
            ),
            Text(
                "Current mode: ${method == 0 ? 'Method Channel API' : 'Native API'}"), // 显示当前模式
            time != 0 ? Text("FPS: ${1000000 ~/ time} fps") : const Offstage() // 显示 FPS
          ],
        ),
      ),
    );
  }
}
