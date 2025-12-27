import 'dart:async';
import 'dart:convert';

import 'package:desktop_webview_window/desktop_webview_window.dart';
import 'package:flutter/material.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

void main(List<String> args) {
  debugPrint('args: $args');
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const MyApp());
}

class MyApp extends StatefulWidget {
  const MyApp({Key? key}) : super(key: key);

  @override
  State<MyApp> createState() => _MyAppState();
}

class _WebviewEntry {
  _WebviewEntry({required this.id, required this.controller, required this.createdAt, required this.headless}) : isClosed = false;

  final int id;
  final dynamic controller;
  final DateTime createdAt;
  final bool headless;
  String? title;
  String? initialUrl;
  bool isClosed;
}

class _MyAppState extends State<MyApp> {
  final TextEditingController _urlController = TextEditingController(text: 'https://bing.com');
  final TextEditingController _jsController = TextEditingController(text: '1 + 1');

  bool _headless = false;
  bool _enableHardwareAcceleration = true;
  bool? _webviewAvailable;

  final List<_WebviewEntry> _entries = [];
  final List<String> _logs = [];
  int _nextId = 1;
  int? _selectedId;

  @override
  void initState() {
    super.initState();
    WebviewWindow.isWebviewAvailable().then((value) {
      setState(() {
        _webviewAvailable = value;
      });
    });
  }

  @override
  void dispose() {
    _urlController.dispose();
    _jsController.dispose();
    super.dispose();
  }

  void _log(String text) {
    setState(() {
      _logs.insert(0, '${DateTime.now().toIso8601String()} - $text');
    });
  }

  Future<String> _getWebViewPath() async {
    final document = await getApplicationDocumentsDirectory();
    return p.join(document.path, 'desktop_webview_window');
  }

  Future<void> _createWebview({String? url}) async {
    try {
      final controller = await WebviewWindow.create(
        configuration: CreateConfiguration(
          headless: _headless,
          enableHardwareAcceleration: _enableHardwareAcceleration,
          userDataFolderWindows: await _getWebViewPath(),
        ),
      );

      final id = _nextId++;
      final entry = _WebviewEntry(id: id, controller: controller, createdAt: DateTime.now(), headless: _headless)
        ..initialUrl = url ?? _urlController.text
        ..title = 'Webview #$id';

      setState(() {
        _entries.add(entry);
        _selectedId = entry.id;
      });

      _log('Created webview ${entry.id}');

      controller
        ..setApplicationNameForUserAgent(' WebviewExample/1.0.0')
        ..addOnWebMessageReceivedCallback((message) {
          _log('onWebMessage received from ${entry.id}: $message');
        })
        ..setOnUrlRequestCallback((url) {
          _log('onUrlRequest from ${entry.id}: $url');
          return true;
        })
        ..launch(entry.initialUrl ?? url ?? _urlController.text)
        ..onClose.whenComplete(() {
          _log('webview ${entry.id} closed');
          setState(() {
            entry.isClosed = true;
            _entries.removeWhere((e) => e.id == entry.id);
            if (_selectedId == entry.id) _selectedId = _entries.isEmpty ? null : _entries.first.id;
          });
        });
    } catch (e, st) {
      _log('Error creating webview: $e');
      debugPrintStack(stackTrace: st);
    }
  }

  Future<void> _closeWebview(int id) async {
    final entry = _entries.firstWhere((e) => e.id == id, orElse: () => throw 'not found');
    try {
      await entry.controller.close();
    } catch (e) {
      _log('Error closing webview $id: $e');
    }
  }

  Future<void> _evaluateOnSelected() async {
    if (_selectedId == null) {
      _log('No webview selected');
      return;
    }
    final entry = _entries.firstWhere((e) => e.id == _selectedId, orElse: () => throw 'not found');
    final js = _jsController.text;
    try {
      final result = await entry.controller.evaluateJavaScript(js);
      _log('Eval on ${entry.id}: $js => ${jsonEncode(result)}');
    } catch (e) {
      _log('Eval error on ${entry.id}: $e');
    }
  }

  Future<void> _closeAll() async {
    final copy = List<_WebviewEntry>.from(_entries);
    for (final e in copy) {
      try {
        await e.controller.close();
      } catch (err) {
        _log('Error closing ${e.id}: $err');
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      home: Scaffold(
        appBar: AppBar(
          title: const Text('Plugin example app'),
        ),
        body: Padding(
          padding: const EdgeInsets.all(12),
          child: Column(
            children: [
              Row(
                children: [
                  Expanded(
                    child: TextField(
                      controller: _urlController,
                      decoration: const InputDecoration(labelText: 'URL to open'),
                    ),
                  ),
                  const SizedBox(width: 8),
                  ElevatedButton(
                    onPressed: _webviewAvailable == true ? () => _createWebview() : null,
                    child: const Text('Open'),
                  ),
                  const SizedBox(width: 8),
                  ElevatedButton(
                    onPressed: _entries.isEmpty ? null : _closeAll,
                    child: const Text('Close All'),
                  ),
                ],
              ),
              const SizedBox(height: 8),
              Row(
                children: [
                  Switch(
                    value: _headless,
                    onChanged: (v) => setState(() => _headless = v),
                  ),
                  const Text('Headless'),
                  const SizedBox(width: 16),
                  Switch(
                    value: _enableHardwareAcceleration,
                    onChanged: (v) => setState(() => _enableHardwareAcceleration = v),
                  ),
                  const Text('Hardware Acceleration'),
                  const Spacer(),
                  Text('Active: ${_entries.length}'),
                ],
              ),
              const Divider(),
              Expanded(
                child: Row(
                  children: [
                    Flexible(
                      flex: 3,
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          const Text('Webviews', style: TextStyle(fontWeight: FontWeight.bold)),
                          const SizedBox(height: 8),
                          Expanded(
                            child: ListView.builder(
                              itemCount: _entries.length,
                              itemBuilder: (context, idx) {
                                final e = _entries[idx];
                                return Card(
                                  child: ListTile(
                                    title: Text(e.title ?? 'Webview ${e.id}'),
                                    subtitle: Text('${e.initialUrl ?? ''}${e.headless ? ' (headless)' : ''}'),
                                    leading: Radio<int?>(value: e.id, groupValue: _selectedId, onChanged: (v) => setState(() => _selectedId = v)),
                                    trailing: Row(mainAxisSize: MainAxisSize.min, children: [
                                      IconButton(
                                        tooltip: 'Close',
                                        icon: const Icon(Icons.close),
                                        onPressed: () => _closeWebview(e.id),
                                      ),
                                    ]),
                                  ),
                                );
                              },
                            ),
                          ),
                        ],
                      ),
                    ),
                    const SizedBox(width: 12),
                    Flexible(
                      flex: 2,
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          const Text('JavaScript / Controls', style: TextStyle(fontWeight: FontWeight.bold)),
                          const SizedBox(height: 8),
                          TextField(
                            controller: _jsController,
                            maxLines: 4,
                            decoration: const InputDecoration(border: OutlineInputBorder(), labelText: 'JavaScript to evaluate'),
                          ),
                          const SizedBox(height: 8),
                          Row(children: [
                            ElevatedButton(onPressed: _evaluateOnSelected, child: const Text('Run on Selected')),
                            const SizedBox(width: 8),
                            ElevatedButton(
                              onPressed: () async {
                                for (final e in List<_WebviewEntry>.from(_entries)) {
                                  try {
                                    final res = await e.controller.evaluateJavaScript(_jsController.text);
                                    _log('Eval on ${e.id}: ${jsonEncode(res)}');
                                  } catch (err) {
                                    _log('Error eval on ${e.id}: $err');
                                  }
                                }
                              },
                              child: const Text('Run on All'),
                            ),
                          ]),
                          const SizedBox(height: 12),
                          const Text('Logs', style: TextStyle(fontWeight: FontWeight.bold)),
                          const SizedBox(height: 8),
                          Expanded(
                            child: Container(
                              decoration: BoxDecoration(border: Border.all(color: Colors.black12), borderRadius: BorderRadius.circular(6)),
                              child: ListView.builder(
                                reverse: true,
                                itemCount: _logs.length,
                                itemBuilder: (context, i) => Padding(
                                  padding: const EdgeInsets.all(6.0),
                                  child: Text(_logs[i], style: const TextStyle(fontSize: 12, fontFamily: 'monospace')),
                                ),
                              ),
                            ),
                          ),
                        ],
                      ),
                    ),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

