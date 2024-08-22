import 'dart:async';
import 'dart:io';

import 'package:collection/collection.dart';
import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:flutter_foreground_task/flutter_foreground_task.dart';
import 'package:friend_private/backend/http/api/plugins.dart';
import 'package:friend_private/backend/http/api/speech_profile.dart';
import 'package:friend_private/backend/http/cloud_storage.dart';
import 'package:friend_private/backend/preferences.dart';
import 'package:friend_private/backend/schema/bt_device.dart';
import 'package:friend_private/backend/schema/memory.dart';
import 'package:friend_private/backend/schema/message.dart';
import 'package:friend_private/backend/schema/plugin.dart';
import 'package:friend_private/main.dart';
import 'package:friend_private/pages/capture/connect.dart';
import 'package:friend_private/pages/capture/page.dart';
import 'package:friend_private/pages/chat/page.dart';
import 'package:friend_private/pages/home/device.dart';
import 'package:friend_private/pages/memories/page.dart';
import 'package:friend_private/pages/plugins/page.dart';
import 'package:friend_private/pages/settings/page.dart';
import 'package:friend_private/providers/home_provider.dart';
import 'package:friend_private/providers/message_provider.dart';
import 'package:friend_private/providers/memory_provider.dart' as mp;
import 'package:friend_private/providers/plugin_provider.dart';
import 'package:friend_private/scripts.dart';
import 'package:friend_private/services/notification_service.dart';
import 'package:friend_private/utils/analytics/mixpanel.dart';
import 'package:friend_private/utils/audio/foreground.dart';
import 'package:friend_private/utils/ble/communication.dart';
import 'package:friend_private/utils/ble/connected.dart';
import 'package:friend_private/utils/ble/scan.dart';
import 'package:friend_private/utils/connectivity_controller.dart';
import 'package:friend_private/utils/other/temp.dart';
import 'package:friend_private/widgets/upgrade_alert.dart';
import 'package:gradient_borders/gradient_borders.dart';
import 'package:instabug_flutter/instabug_flutter.dart';
import 'package:provider/provider.dart';
import 'package:upgrader/upgrader.dart';

class HomePageWrapper extends StatelessWidget {
  const HomePageWrapper({super.key});

  @override
  Widget build(BuildContext context) {
    return ChangeNotifierProvider(
      create: (context) => HomeProvider(),
      child: const HomePage(),
    );
  }
}

class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> with WidgetsBindingObserver, TickerProviderStateMixin {
  ForegroundUtil foregroundUtil = ForegroundUtil();
  TabController? _controller;
  List<Widget> screens = [Container(), const SizedBox(), const SizedBox()];

  FocusNode chatTextFieldFocusNode = FocusNode(canRequestFocus: true);
  FocusNode memoriesTextFieldFocusNode = FocusNode(canRequestFocus: true);

  GlobalKey<CapturePageState> capturePageKey = GlobalKey();
  GlobalKey<ChatPageState> chatPageKey = GlobalKey();
  StreamSubscription<OnConnectionStateChangedEvent>? _connectionStateListener;
  StreamSubscription<List<int>>? _bleBatteryLevelListener;

  int batteryLevel = -1;
  BTDeviceStruct? _device;

  List<Plugin> plugins = [];
  final _upgrader = MyUpgrader(debugLogging: false, debugDisplayOnce: false);

  bool scriptsInProgress = false;

  Future<void> _initiatePlugins() async {
    context.read<PluginProvider>().getPlugins();
    plugins = SharedPreferencesUtil().pluginsList;
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    super.didChangeAppLifecycleState(state);
    String event = '';
    if (state == AppLifecycleState.paused) {
      event = 'App is paused';
    } else if (state == AppLifecycleState.resumed) {
      event = 'App is resumed';
    } else if (state == AppLifecycleState.hidden) {
      event = 'App is hidden';
    } else if (state == AppLifecycleState.detached) {
      event = 'App is detached';
    } else {
      return;
    }
    debugPrint(event);
    InstabugLog.logInfo(event);
  }

  _migrationScripts() async {
    setState(() => scriptsInProgress = true);
    await scriptMigrateMemoriesToBack();
    if (mounted) {
      await context.read<mp.MemoryProvider>().getInitialMemories();
    }
    setState(() => scriptsInProgress = false);
  }

  ///Screens with respect to subpage
  final Map<String, Widget> screensWithRespectToPath = {
    '/settings': const SettingsPage(),
  };
  ConnectivityController connectivityController = ConnectivityController();
  bool? previousConnection;

  @override
  void initState() {
    // TODO: Being triggered multiple times during navigation. It ideally shouldn't
    connectivityController.init();
    _controller = TabController(
      length: 3,
      vsync: this,
      initialIndex: SharedPreferencesUtil().pageToShowFromNotification,
    );
    SharedPreferencesUtil().pageToShowFromNotification = 1;
    SharedPreferencesUtil().onboardingCompleted = true;

    WidgetsBinding.instance.addObserver(this);
    WidgetsBinding.instance.addPostFrameCallback((_) async {
      ForegroundUtil.requestPermissions();
      await ForegroundUtil.initializeForegroundService();
      ForegroundUtil.startForegroundTask();
      if (mounted) {
        await context.read<MessageProvider>().refreshMessages();
        await context.read<HomeProvider>().setupHasSpeakerProfile();
      }
    });

    _initiatePlugins();

    //TODO: Should this run everytime?
    // _migrationScripts();

    authenticateGCP();
    if (SharedPreferencesUtil().btDeviceStruct.id.isNotEmpty) {
      scanAndConnectDevice().then(_onConnected);
    }

    _listenToMessagesFromNotification();
    if (SharedPreferencesUtil().subPageToShowFromNotification != '') {
      final subPageRoute = SharedPreferencesUtil().subPageToShowFromNotification;
      WidgetsBinding.instance.addPostFrameCallback((_) async {
        MyApp.navigatorKey.currentState?.push(
          MaterialPageRoute(
            builder: (context) => screensWithRespectToPath[subPageRoute] as Widget,
          ),
        );
      });
      SharedPreferencesUtil().subPageToShowFromNotification = '';
    }
    super.initState();
  }

  void _listenToMessagesFromNotification() {
    NotificationService.instance.listenForServerMessages.listen((message) {
      context.read<MessageProvider>().addMessage(message);
      chatPageKey.currentState?.scrollToBottom();
    });
  }

  Timer? _disconnectNotificationTimer;

  _initiateConnectionListener() async {
    if (_connectionStateListener != null) return;
    _connectionStateListener?.cancel();
    _connectionStateListener = getConnectionStateListener(
        deviceId: _device!.id,
        onDisconnected: () {
          debugPrint('onDisconnected');
          capturePageKey.currentState?.resetState(restartBytesProcessing: false);
          setState(() => _device = null);
          InstabugLog.logInfo('Friend Device Disconnected');
          _disconnectNotificationTimer?.cancel();
          _disconnectNotificationTimer = Timer(const Duration(seconds: 30), () {
            NotificationService.instance.createNotification(
              title: 'Friend Device Disconnected',
              body: 'Please reconnect to continue using your Friend.',
            );
          });
          MixpanelManager().deviceDisconnected();
        },
        onConnected: ((d) => _onConnected(d, initiateConnectionListener: false)));
  }

  _onConnected(BTDeviceStruct? connectedDevice, {bool initiateConnectionListener = true}) {
    debugPrint('_onConnected: $connectedDevice');
    if (connectedDevice == null) return;
    _disconnectNotificationTimer?.cancel();
    NotificationService.instance.clearNotification(1);
    _device = connectedDevice;
    if (initiateConnectionListener) _initiateConnectionListener();
    _initiateBleBatteryListener();
    capturePageKey.currentState?.resetState(restartBytesProcessing: true, btDevice: connectedDevice);
    MixpanelManager().deviceConnected();
    SharedPreferencesUtil().btDeviceStruct = _device!;
    SharedPreferencesUtil().deviceName = _device!.name;
    if (mounted) {
      setState(() {});
    }
  }

  _initiateBleBatteryListener() async {
    _bleBatteryLevelListener?.cancel();
    _bleBatteryLevelListener = await getBleBatteryLevelListener(
      _device!.id,
      onBatteryLevelChange: (int value) {
        setState(() {
          batteryLevel = value;
        });
      },
    );
  }

  _tabChange(int index) {
    MixpanelManager().bottomNavigationTabClicked(['Memories', 'Device', 'Chat'][index]);
    FocusScope.of(context).unfocus();
    setState(() {
      _controller!.index = index;
    });
  }

  @override
  Widget build(BuildContext context) {
    return WithForegroundTask(
        child: MyUpgradeAlert(
      upgrader: _upgrader,
      dialogStyle: Platform.isIOS ? UpgradeDialogStyle.cupertino : UpgradeDialogStyle.material,
      child: ValueListenableBuilder(
          valueListenable: connectivityController.isConnected,
          builder: (ctx, isConnected, child) {
            previousConnection ??= true;
            if (previousConnection != isConnected) {
              previousConnection = isConnected;
              if (!isConnected) {
                Future.delayed(Duration.zero, () {
                  ScaffoldMessenger.of(ctx).showMaterialBanner(
                    MaterialBanner(
                      content: const Text('No internet connection. Please check your connection.'),
                      backgroundColor: Colors.red,
                      actions: [
                        TextButton(
                          onPressed: () {
                            ScaffoldMessenger.of(ctx).hideCurrentMaterialBanner();
                          },
                          child: const Text('Dismiss'),
                        ),
                      ],
                    ),
                  );
                });
              } else {
                Future.delayed(Duration.zero, () {
                  ScaffoldMessenger.of(ctx).hideCurrentMaterialBanner();
                  ScaffoldMessenger.of(ctx).showMaterialBanner(
                    MaterialBanner(
                      content: const Text('Internet connection is restored.'),
                      backgroundColor: Colors.green,
                      actions: [
                        TextButton(
                          onPressed: () {
                            ScaffoldMessenger.of(ctx).hideCurrentMaterialBanner();
                          },
                          child: const Text('Dismiss'),
                        ),
                      ],
                      onVisible: () => Future.delayed(const Duration(seconds: 3), () {
                        ScaffoldMessenger.of(ctx).hideCurrentMaterialBanner();
                      }),
                    ),
                  );

                  WidgetsBinding.instance.addPostFrameCallback((_) async {
                    if (mounted) {
                      if (context.read<mp.MemoryProvider>().memories.isEmpty) {
                        await context.read<mp.MemoryProvider>().getInitialMemories();
                      }
                      if (context.read<MessageProvider>().messages.isEmpty) {
                        await context.read<MessageProvider>().refreshMessages();
                      }
                    }
                  });
                });
              }
            }

            return Scaffold(
              backgroundColor: Theme.of(context).colorScheme.primary,
              body: GestureDetector(
                onTap: () {
                  FocusScope.of(context).unfocus();
                  chatTextFieldFocusNode.unfocus();
                  memoriesTextFieldFocusNode.unfocus();
                },
                child: Consumer2<HomeProvider, mp.MemoryProvider>(builder: (context, provider, memProvider, child) {
                  return Stack(
                    children: [
                      Center(
                        child: TabBarView(
                          controller: _controller,
                          physics: const NeverScrollableScrollPhysics(),
                          children: [
                            MemoriesPage(
                              textFieldFocusNode: memoriesTextFieldFocusNode,
                            ),
                            CapturePage(
                              key: capturePageKey,
                              device: _device,
                              addMemory: (ServerMemory memory) {
                                memProvider.addMemory(memory);
                              },
                              addMessage: (ServerMessage message) {
                                context.read<MessageProvider>().addMessage(message);
                                chatPageKey.currentState?.scrollToBottom();
                              },
                              updateMemory: (ServerMemory memory) {
                                memProvider.updateMemory(memory);
                              },
                            ),
                            ChatPage(
                              key: chatPageKey,
                              textFieldFocusNode: chatTextFieldFocusNode,
                              updateMemory: (ServerMemory memory) {
                                memProvider.updateMemory(memory);
                              },
                            ),
                          ],
                        ),
                      ),
                      if (chatTextFieldFocusNode.hasFocus || memoriesTextFieldFocusNode.hasFocus)
                        const SizedBox.shrink()
                      else
                        Align(
                          alignment: Alignment.bottomCenter,
                          child: Container(
                            margin: const EdgeInsets.fromLTRB(16, 16, 16, 40),
                            decoration: const BoxDecoration(
                              color: Colors.black,
                              borderRadius: BorderRadius.all(Radius.circular(16)),
                              border: GradientBoxBorder(
                                gradient: LinearGradient(colors: [
                                  Color.fromARGB(127, 208, 208, 208),
                                  Color.fromARGB(127, 188, 99, 121),
                                  Color.fromARGB(127, 86, 101, 182),
                                  Color.fromARGB(127, 126, 190, 236)
                                ]),
                                width: 2,
                              ),
                              shape: BoxShape.rectangle,
                            ),
                            child: Row(
                              mainAxisAlignment: MainAxisAlignment.spaceBetween,
                              children: [
                                Expanded(
                                  child: MaterialButton(
                                    onPressed: () => _tabChange(0),
                                    child: Padding(
                                      padding: const EdgeInsets.only(top: 20, bottom: 20),
                                      child: Text('Memories',
                                          maxLines: 1,
                                          overflow: TextOverflow.ellipsis,
                                          style: TextStyle(
                                              color: _controller!.index == 0 ? Colors.white : Colors.grey,
                                              fontSize: 16)),
                                    ),
                                  ),
                                ),
                                Expanded(
                                  child: MaterialButton(
                                    onPressed: () => _tabChange(1),
                                    child: Padding(
                                      padding: const EdgeInsets.only(
                                        top: 20,
                                        bottom: 20,
                                      ),
                                      child: Text('Capture',
                                          maxLines: 1,
                                          overflow: TextOverflow.ellipsis,
                                          style: TextStyle(
                                              color: _controller!.index == 1 ? Colors.white : Colors.grey,
                                              fontSize: 16)),
                                    ),
                                  ),
                                ),
                                Expanded(
                                  child: MaterialButton(
                                    onPressed: () => _tabChange(2),
                                    child: Padding(
                                      padding: const EdgeInsets.only(top: 20, bottom: 20),
                                      child: Text('Chat',
                                          maxLines: 1,
                                          overflow: TextOverflow.ellipsis,
                                          style: TextStyle(
                                              color: _controller!.index == 2 ? Colors.white : Colors.grey,
                                              fontSize: 16)),
                                    ),
                                  ),
                                ),
                              ],
                            ),
                          ),
                        ),
                      if (scriptsInProgress)
                        Center(
                          child: Container(
                            height: 150,
                            width: 250,
                            decoration: BoxDecoration(
                              color: Colors.black,
                              borderRadius: BorderRadius.circular(16),
                              border: Border.all(color: Colors.white, width: 2),
                            ),
                            child: const Column(
                              crossAxisAlignment: CrossAxisAlignment.center,
                              mainAxisAlignment: MainAxisAlignment.center,
                              children: [
                                CircularProgressIndicator(
                                  valueColor: AlwaysStoppedAnimation<Color>(Colors.white),
                                ),
                                SizedBox(height: 16),
                                Center(
                                    child: Text(
                                  'Running migration, please wait! 🚨',
                                  style: TextStyle(color: Colors.white, fontSize: 16),
                                  textAlign: TextAlign.center,
                                )),
                              ],
                            ),
                          ),
                        )
                      else
                        const SizedBox.shrink(),
                    ],
                  );
                }),
              ),
              appBar: AppBar(
                automaticallyImplyLeading: false,
                backgroundColor: Theme.of(context).colorScheme.surface,
                title: Row(
                  mainAxisAlignment: MainAxisAlignment.spaceBetween,
                  crossAxisAlignment: CrossAxisAlignment.center,
                  children: [
                    _device != null && batteryLevel != -1
                        ? GestureDetector(
                            onTap: _device == null
                                ? null
                                : () {
                                    Navigator.of(context).push(MaterialPageRoute(
                                        builder: (c) => ConnectedDevice(
                                              device: _device!,
                                              batteryLevel: batteryLevel,
                                            )));
                                    MixpanelManager().batteryIndicatorClicked();
                                  },
                            child: Container(
                                padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
                                decoration: BoxDecoration(
                                  color: Colors.transparent,
                                  borderRadius: BorderRadius.circular(10),
                                  border: Border.all(
                                    color: Colors.grey,
                                    width: 1,
                                  ),
                                ),
                                child: Row(
                                  mainAxisSize: MainAxisSize.min,
                                  children: [
                                    Container(
                                      width: 10,
                                      height: 10,
                                      decoration: BoxDecoration(
                                        color: batteryLevel > 75
                                            ? const Color.fromARGB(255, 0, 255, 8)
                                            : batteryLevel > 20
                                                ? Colors.yellow.shade700
                                                : Colors.red,
                                        shape: BoxShape.circle,
                                      ),
                                    ),
                                    const SizedBox(width: 8.0),
                                    Text(
                                      '${batteryLevel.toString()}%',
                                      style: const TextStyle(
                                        color: Colors.white,
                                        fontSize: 12,
                                        fontWeight: FontWeight.bold,
                                      ),
                                    ),
                                  ],
                                )),
                          )
                        : TextButton(
                            onPressed: () async {
                              if (SharedPreferencesUtil().btDeviceStruct.id.isEmpty) {
                                routeToPage(context, const ConnectDevicePage());
                                MixpanelManager().connectFriendClicked();
                              } else {
                                await routeToPage(context, const ConnectedDevice(device: null, batteryLevel: 0));
                              }
                              setState(() {});
                            },
                            style: TextButton.styleFrom(
                              padding: EdgeInsets.zero,
                              backgroundColor: Colors.transparent,
                              shape: RoundedRectangleBorder(
                                borderRadius: BorderRadius.circular(10),
                                side: const BorderSide(color: Colors.white, width: 1),
                              ),
                            ),
                            child: Image.asset('assets/images/logo_transparent.png', width: 25, height: 25),
                          ),
                    _controller!.index == 2
                        ? Padding(
                            padding: const EdgeInsets.only(left: 0),
                            child: Container(
                              // decoration: BoxDecoration(
                              //   border: Border.all(color: Colors.grey),
                              //   borderRadius: BorderRadius.circular(30),
                              // ),
                              padding: const EdgeInsets.symmetric(horizontal: 16),
                              child: DropdownButton<String>(
                                menuMaxHeight: 350,
                                value: SharedPreferencesUtil().selectedChatPluginId,
                                onChanged: (s) async {
                                  if ((s == 'no_selected' && plugins.where((p) => p.enabled).isEmpty) ||
                                      s == 'enable') {
                                    await routeToPage(context, const PluginsPage(filterChatOnly: true));
                                    plugins = SharedPreferencesUtil().pluginsList;
                                    setState(() {});
                                    return;
                                  }
                                  print('Selected: $s prefs: ${SharedPreferencesUtil().selectedChatPluginId}');
                                  if (s == null || s == SharedPreferencesUtil().selectedChatPluginId) return;

                                  SharedPreferencesUtil().selectedChatPluginId = s;
                                  var plugin = plugins.firstWhereOrNull((p) => p.id == s);
                                  chatPageKey.currentState?.sendInitialPluginMessage(plugin);
                                  setState(() {});
                                },
                                icon: Container(),
                                alignment: Alignment.center,
                                dropdownColor: Colors.black,
                                style: const TextStyle(color: Colors.white, fontSize: 16),
                                underline: Container(height: 0, color: Colors.transparent),
                                isExpanded: false,
                                itemHeight: 48,
                                padding: EdgeInsets.zero,
                                items: _getPluginsDropdownItems(context),
                              ),
                            ),
                          )
                        : const SizedBox(width: 16),
                    IconButton(
                      icon: const Icon(Icons.settings, color: Colors.white, size: 30),
                      onPressed: () async {
                        MixpanelManager().settingsOpened();
                        String language = SharedPreferencesUtil().recordingsLanguage;
                        bool hasSpeech = SharedPreferencesUtil().hasSpeakerProfile;
                        await routeToPage(context, const SettingsPage());
                        // TODO: this fails like 10 times, connects reconnects, until it finally works.
                        if (language != SharedPreferencesUtil().recordingsLanguage ||
                            hasSpeech != SharedPreferencesUtil().hasSpeakerProfile) {
                          capturePageKey.currentState?.restartWebSocket();
                        }
                        plugins = SharedPreferencesUtil().pluginsList;
                        setState(() {});
                      },
                    )
                  ],
                ),
                elevation: 0,
                centerTitle: true,
              ),
            );
          }),
    ));
  }

  _getPluginsDropdownItems(BuildContext context) {
    var items = [
          DropdownMenuItem<String>(
            value: 'no_selected',
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.center,
              children: [
                const Icon(size: 20, Icons.chat, color: Colors.white),
                const SizedBox(width: 10),
                Text(
                  plugins.where((p) => p.enabled).isEmpty ? 'Enable Plugins   ' : 'Select a plugin',
                  style: const TextStyle(color: Colors.white, fontWeight: FontWeight.w500, fontSize: 16),
                )
              ],
            ),
          )
        ] +
        plugins.where((p) => p.enabled && p.worksWithChat()).map<DropdownMenuItem<String>>((Plugin plugin) {
          return DropdownMenuItem<String>(
            value: plugin.id,
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.end,
              mainAxisAlignment: MainAxisAlignment.start,
              children: [
                CircleAvatar(
                  backgroundColor: Colors.white,
                  maxRadius: 12,
                  backgroundImage: NetworkImage(plugin.getImageUrl()),
                ),
                const SizedBox(width: 8),
                Text(
                  plugin.name.length > 18
                      ? '${plugin.name.substring(0, 18)}...'
                      : plugin.name + ' ' * (18 - plugin.name.length),
                  style: const TextStyle(color: Colors.white, fontWeight: FontWeight.w500, fontSize: 16),
                )
              ],
            ),
          );
        }).toList();
    if (plugins.where((p) => p.enabled).isNotEmpty) {
      items.add(const DropdownMenuItem<String>(
        value: 'enable',
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.center,
          children: [
            CircleAvatar(
              backgroundColor: Colors.transparent,
              maxRadius: 12,
              child: Icon(Icons.star, color: Colors.purpleAccent),
            ),
            SizedBox(width: 8),
            Text('Enable Plugins   ', style: TextStyle(color: Colors.white, fontWeight: FontWeight.w500, fontSize: 16))
          ],
        ),
      ));
    }
    return items;
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _connectionStateListener?.cancel();
    _bleBatteryLevelListener?.cancel();
    connectivityController.isConnected.dispose();
    _controller?.dispose();
    ForegroundUtil.stopForegroundTask();
    super.dispose();
  }
}
