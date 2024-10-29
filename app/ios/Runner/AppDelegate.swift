import UIKit
import Flutter
import UserNotifications
import WatchConnectivity

@main
@objc class AppDelegate: FlutterAppDelegate {
  private var methodChannel: FlutterMethodChannel?
  private var watchSession: WCSession?

  private var notificationTitleOnKill: String?
  private var notificationBodyOnKill: String?

  override func application(
    _ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?
  ) -> Bool {
    GeneratedPluginRegistrant.register(with: self)
    //Creates a method channel to handle notifications on kill
    let controller = window?.rootViewController as? FlutterViewController
    methodChannel = FlutterMethodChannel(name: "com.friend.ios/notifyOnKill", binaryMessenger: controller!.binaryMessenger)
    methodChannel?.setMethodCallHandler { [weak self] (call, result) in
      self?.handleMethodCall(call, result: result)
    }

    // here, Without this code the task will not work.
    SwiftFlutterForegroundTaskPlugin.setPluginRegistrantCallback(registerPlugins)
    if #available(iOS 10.0, *) {
      UNUserNotificationCenter.current().delegate = self as? UNUserNotificationCenterDelegate
    }

    setupMethodChannel()
    setupWatchConnectivity()

    return super.application(application, didFinishLaunchingWithOptions: launchOptions)
  }

  private func handleMethodCall(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    switch call.method {
      case "setNotificationOnKillService":
        handleSetNotificationOnKillService(call: call)
      default:
        result(FlutterMethodNotImplemented)
    }
  }

  private func handleSetNotificationOnKillService(call: FlutterMethodCall) {
    NSLog("handleMethodCall: setNotificationOnKillService")

    if let args = call.arguments as? Dictionary<String, Any> {
      notificationTitleOnKill = args["title"] as? String
      notificationBodyOnKill = args["description"] as? String
    }

  }


  override func applicationWillTerminate(_ application: UIApplication) {
    // If title and body are nil, then we don't need to show notification.
    if notificationTitleOnKill == nil || notificationBodyOnKill == nil {
      return
    }

    let content = UNMutableNotificationContent()
    content.title = notificationTitleOnKill!
    content.body = notificationBodyOnKill!
    let trigger = UNTimeIntervalNotificationTrigger(timeInterval: 1, repeats: false)
    let request = UNNotificationRequest(identifier: "notification on app kill", content: content, trigger: trigger)

    NSLog("Running applicationWillTerminate")

    UNUserNotificationCenter.current().add(request) { (error) in
      if let error = error {
        NSLog("Failed to show notification on kill service => error: \(error.localizedDescription)")
      } else {
        NSLog("Show notification on kill now")
      }
    }
  }

  private func setupMethodChannel() {
    guard let controller = window?.rootViewController as? FlutterViewController else {
      return
    }

    methodChannel = FlutterMethodChannel(
      name: "com.friend.watch",
      binaryMessenger: controller.binaryMessenger)
  }

  private func setupWatchConnectivity() {
    if WCSession.isSupported() {
      watchSession = WCSession.default
      watchSession?.delegate = self
      watchSession?.activate()
    }
  }
}

extension AppDelegate: WCSessionDelegate {
  func session(_ session: WCSession, activationDidCompleteWith activationState: WCSessionActivationState, error: Error?) {
    print("Watch session activation completed: \(activationState.rawValue)")
  }

  func sessionDidBecomeInactive(_ session: WCSession) {
    print("Watch session became inactive")
  }

  func sessionDidDeactivate(_ session: WCSession) {
    print("Watch session deactivated")
  }

  func session(_ session: WCSession, didReceiveMessage message: [String : Any]) {
    if let audioData = message["audioData"] as? Data {
      DispatchQueue.main.async {
        self.methodChannel?.invokeMethod(
          "audioDataReceived",
          arguments: FlutterStandardTypedData(bytes: audioData)
        )
      }
    } else if let status = message["status"] as? String {
      switch status {
      case "recording_started":
        DispatchQueue.main.async {
          self.methodChannel?.invokeMethod("recordingStatus", arguments: true)
        }
      case "recording_stopped":
        DispatchQueue.main.async {
          self.methodChannel?.invokeMethod("recordingStatus", arguments: false)
        }
      case "wal_sync_complete":
        DispatchQueue.main.async {
          self.methodChannel?.invokeMethod("walSyncStatus", arguments: true)
        }
      default:
        break
      }
    }
  }
}

// here
func registerPlugins(registry: FlutterPluginRegistry) {
  GeneratedPluginRegistrant.register(with: registry)
}
