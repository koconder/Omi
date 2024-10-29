import 'package:flutter/services.dart';
import 'package:friend_private/services/logger_service.dart';
import 'package:friend_private/utils/enums.dart';

class WatchManager {
  static const MethodChannel _channel = MethodChannel('com.friend.watch');
  static final WatchManager _instance = WatchManager._internal();
  final _logger = LoggerService();

  bool _isRecording = false;
  bool get isRecording => _isRecording;

  factory WatchManager() => _instance;
  WatchManager._internal() {
    _setupMethodCallHandler();
  }

  void _setupMethodCallHandler() {
    _channel.setMethodCallHandler((call) async {
      switch (call.method) {
        case 'audioDataReceived':
          if (call.arguments is Uint8List) {
            await _handleAudioData(call.arguments as Uint8List);
          }
          break;
        case 'recordingStatus':
          _isRecording = call.arguments as bool;
          _notifyRecordingStateChanged();
          break;
        case 'walSyncStatus':
          // Handle WAL sync completion
          break;
      }
    });
  }

  Future<void> _handleAudioData(Uint8List audioData) async {
    try {
      // Process audio data using your existing pipeline
      if (_captureProvider?.transcriptServiceReady ?? false) {
        await _captureProvider?.processRawAudioData(audioData);
      }
    } catch (e) {
      _logger.error('Error processing watch audio data', e);
    }
  }

  Future<bool> isWatchAvailable() async {
    try {
      final bool available = await _channel.invokeMethod('isWatchAvailable');
      return available;
    } catch (e) {
      _logger.error('Error checking watch availability', e);
      return false;
    }
  }

  Future<void> startRecording() async {
    try {
      await _channel.invokeMethod('startWatchRecording');
    } catch (e) {
      _logger.error('Error starting watch recording', e);
    }
  }

  Future<void> stopRecording() async {
    try {
      await _channel.invokeMethod('stopWatchRecording');
    } catch (e) {
      _logger.error('Error stopping watch recording', e);
    }
  }

  void _notifyRecordingStateChanged() {
    // Notify your state management system
  }
}
