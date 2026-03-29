#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include <ctime>
#include <atomic>
#include <deque>
#include <functional>
#include <ixwebsocket/IXWebSocket.h>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <queue>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

struct MessagePart {
  std::string text;
  uint32_t color = 0xFFFFFFFF; // RGBA (default white)
  int player_id = -1;          // For dynamic highlighting
};

struct RichMessage {
  double timestamp;
  std::vector<MessagePart> parts;
  int sender_slot = -1;
  int receiver_slot = -1;
  std::string source_slot; // Name of the slot that received this
  int64_t item_id = -1;
  int item_flags = 0;
  bool is_reconciled = false;
  std::string type;     // Message type (e.g., "Goal", "Hint", "Chat")
  struct tm local_time; // Cached local time to avoid redundant syscalls

  void populate_local_time() {
    time_t t = (time_t)timestamp;
#ifdef _WIN32
    localtime_s(&local_time, &t);
#else
    localtime_r(&t, &local_time);
#endif
  }
};

struct Hint {
  int64_t item_id;
  int64_t location_id;
  std::string entrance_name;
  int receiver_slot;
  int finder_slot;
  bool found;
  int item_flags;
  int status = 0;
  std::string source_slot;
};

struct MultiworldStats {
  int total_games = 0;
  std::set<int> completed_slots;
  int total_locations = 0;
  int checked_locations = 0;
};

struct ServerMetadata {
  std::map<std::string, std::map<int64_t, std::string>> item_names;
  std::map<std::string, std::map<int64_t, std::string>> location_names;
  std::map<std::string, std::map<std::string, int64_t>> location_name_to_id;
  std::map<std::string, std::map<int64_t, std::string>> entrance_names;
  std::map<int, std::string> player_names;
  std::map<int, std::string> slot_to_game;
  std::map<std::string, std::string> datapackage_checksums;
  bool data_package_received = false;
};

class ArchipelagoNetwork;

class ArchipelagoSession {
  friend class ArchipelagoNetwork;
public:
  enum class State { Disconnected, Connecting, Connected };

  ArchipelagoSession(ArchipelagoNetwork *manager, const std::string &name);
  ~ArchipelagoSession();

  void Connect(const std::string &url, const std::string &password);
  void Disconnect();
  bool Update();
  void SendChat(const std::string &message);
  void ReResolveHistory();
  void ClearData();
  void UpdateOrAddHint(const Hint &hint);
  void UpdateHintStatus(int64_t location_id, int finder_slot, int status);
  void SendUpdateHint(int64_t location_id, int finder_slot, int status);
  void SendPacket(const nlohmann::json &packet);

  State GetState() const;
  bool IsConnected() const { return is_connected_; }
  const std::string &GetName() const { return name_; }
  const std::string &GetUrl() const { return original_url_; }
  int GetLocalSlot() const { return local_slot_; }
  int GetTeam() const { return team_; }

  const std::vector<RichMessage> &GetReceivedItems() const {
    return received_items_history_;
  }
  const std::vector<Hint> &GetHints() const { return hints_; }
  const std::set<int64_t> &GetCheckedLocations() const {
    return checked_locations_;
  }
  const std::set<int64_t> &GetMissingLocations() const {
    return missing_locations_;
  }
  bool IsDataPackageReceived() const {
    return metadata_ && metadata_->data_package_received;
  }

  // Metadata accessors
  std::string ResolveItemName(int64_t id, int slot = -1);
  std::string ResolveLocationName(int64_t id, int slot = -1);
  int64_t ResolveLocationID(const std::string &name, int slot = -1);
  std::string ResolveEntranceName(int64_t id, int slot = -1);
  std::string ResolvePlayerGame(int slot = -1) const;
  std::string ResolvePlayerName(int slot = -1) const;
  const std::map<int, std::string> &GetPlayerNames() const {
    if (metadata_) {
      return metadata_->player_names;
    }
    static const std::map<int, std::string> empty_map;
    return empty_map;
  }

  const nlohmann::json &GetSlotData() const { return slot_data_; }

  std::map<int64_t, std::string>
  GetLocationsForGame(const std::string &game) const {
    if (metadata_ && metadata_->location_names.count(game)) {
      return metadata_->location_names.at(game);
    }
    return {};
  }

private:
  void HandleMessage(const ix::WebSocketMessagePtr &msg);
  void SendConnect();
  void SendGetDataPackage(const std::vector<std::string> &games = {});
  void SendSync();
  void SendGetHints();
  void ResolvePendingItems();

  ArchipelagoNetwork *manager_;
  std::string name_;
  ix::WebSocket webSocket_;
  std::string original_url_;
  std::string password_;
  bool is_connected_ = false;
  bool user_wants_connection_ = false;
  bool tried_wss_ = false;
  bool tried_ws_ = false;
  bool pending_fallback_ = false;
  std::string pending_url_;
  int local_slot_ = -1;
  int team_ = 0;
  double connection_error_time_ = -1.0;

  struct QueuedPacket {
    nlohmann::json packet;
    double timestamp;
  };
  struct QueuedStatus {
    std::string message;
    double timestamp;
  };

  std::queue<QueuedPacket> message_queue_;
  std::queue<QueuedStatus> status_messages_;
  std::mutex queue_mutex_;
  std::mutex status_mutex_;

  // Shared Metadata
  std::shared_ptr<ServerMetadata> metadata_;

  // History buffers (session-specific)
  std::vector<RichMessage> received_items_history_;
  std::vector<Hint> hints_;
  std::set<int64_t> checked_locations_;
  std::set<int64_t> missing_locations_;
  nlohmann::json slot_data_;

  struct PendingItem {
    int64_t id;
    int sender;
    int receiver;
    int flags;
    bool is_received_packet;
  };
  std::vector<PendingItem> pending_items_;

  int64_t last_received_day_ = -1;
};

struct ConnectionSettings;

class ArchipelagoNetwork {
public:
  void SyncTotalLocations();
  using State = ArchipelagoSession::State;
  ArchipelagoNetwork();
  ~ArchipelagoNetwork();

  void StartNetworkThread();
  void StopNetworkThread();
  bool Update();
  void SetMaxHistory(int max_history) { max_history_size_ = max_history; }

  void LockHistory() const { history_mutex_.lock(); }
  void UnlockHistory() const { history_mutex_.unlock(); }

  // Session management
  ArchipelagoSession *AddSession(const std::string &name);
  void RemoveSession(const std::string &name);
  void DisconnectAll();
  ArchipelagoSession *GetSessionBySlot(int slot);
  const std::vector<std::unique_ptr<ArchipelagoSession>> &GetSessions() const {
    return sessions_;
  }
  ArchipelagoSession *GetSession(const std::string &name);
  ArchipelagoSession *GetSessionByGlobalSlot(int global_slot);
  void OnLocalHintStatusUpdated(int64_t location_id, int finder_slot,
                                 int status);
  std::shared_ptr<ServerMetadata> GetOrCreateMetadata(const std::string &url);

  std::recursive_mutex &GetHistoryMutex() const { return history_mutex_; }

  // Global history
  const std::vector<RichMessage> &GetChatHistory() const {
    return chat_history_;
  }
  const std::vector<RichMessage> &GetItemHistory() const {
    return item_history_;
  }

  MultiworldStats GetGlobalStats() const;
  void UpdateTrackerStats();
  void ForceTrackerSync();
  double GetLastTrackerSyncTime() const { return last_tracker_sync_time_; }

  enum class TrackerConfidence { Low, High };
  TrackerConfidence GetTrackerConfidence() const { return tracker_confidence_; }
  void SetTotalGames(int count);
  bool IsAnySessionConnected() const;

  // Aggregated data
  const std::vector<RichMessage> &GetAggregatedReceivedItems() const;
  const std::vector<Hint> &GetAggregatedHints() const;
  bool IsDataPackageReceived() const;

  void SendChat(const std::string &session_name, const std::string &message);
  void ClearAllData(bool keep_chat = true);

  // Global Resolution Helpers (DELETED in favor of Session-specific resolution)
  std::string ResolvePlayerName(int slot);
  std::string ResolvePlayerGame(int slot);
  const std::set<int> &GetConnectedSlots();
  bool IsAnySessionActive() const;

  void SetHintsDirty();
  void SetItemsDirty();
  void ClearChatHistory();
  void ClearItemHistory();

  // Callbacks
  std::function<void()> on_history_updated;
  void SetWakeUpCallback(std::function<void()> cb) { wake_up_callback_ = cb; }
  void SetTrackerSyncActive(bool active);
  void WakeUp() {
    if (wake_up_callback_)
      wake_up_callback_();
  }

  // Internal use by sessions
  void OnGlobalMessage(ArchipelagoSession *session, const RichMessage &msg,
                       bool is_item_feed, size_t message_hash = 0,
                       bool always_show = false);
  void OnStatusMessage(ArchipelagoSession *session, const std::string &msg);
  void SetSettings(const ConnectionSettings *settings) { settings_ = settings; }
  static std::string MaskURL(const std::string &url);
  bool IsMasterSession(ArchipelagoSession *session) const;
  void ReResolveHistory();
  void ReResolveHistoryVector(std::vector<RichMessage> &history);
  void SetDebugMode(bool debug) { debug_mode_ = debug; }
  bool IsDebugMode() const { return debug_mode_; }

  uint64_t GetDataVersion() const { return data_version_; }
  const ConnectionSettings *GetSettings() const { return settings_; }

private:
  bool debug_mode_ = false;
  std::vector<RichMessage> history_;

private:
  std::function<void()> wake_up_callback_;

private:
  void CheckDayChange(std::vector<RichMessage> &history, double timestamp,
                      int64_t &last_day);
  int64_t last_item_day_ = -1;
  int64_t last_chat_day_ = -1;

  std::vector<std::unique_ptr<ArchipelagoSession>> sessions_;
  std::vector<RichMessage> chat_history_;
  std::vector<RichMessage> item_history_;
  int max_history_size_ = 0;
  const ConnectionSettings *settings_ = nullptr;
  std::map<std::string, std::shared_ptr<ServerMetadata>> url_to_metadata_;

  std::set<int> connected_slots_cache_;
  bool slots_dirty_ = true;

  mutable std::vector<RichMessage> aggregated_items_cache_;
  mutable std::vector<Hint> aggregated_hints_cache_;
  mutable bool aggregated_items_dirty_ = true;
  mutable bool aggregated_hints_dirty_ = true;
  uint64_t data_version_ = 0;

  struct MessageHashEntry {
    std::string first_session_name;
    double last_time;
  };
  std::unordered_map<size_t, MessageHashEntry> message_hash_history_;
  std::deque<size_t> message_hash_queue_;
  mutable std::recursive_mutex history_mutex_;
  MultiworldStats global_stats_;
  double last_tracker_sync_time_ = -1.0;
  bool force_tracker_sync_ = false;
  TrackerConfidence tracker_confidence_ = TrackerConfidence::Low;
  int last_tracker_checked_count_ = -1;
  int live_checks_since_last_poll_ = 0;
  std::map<std::string, ArchipelagoSession::State> last_session_states_;
  bool last_any_session_connected_ = false;
  double last_item_activity_time_ = -1.0;
  double last_successful_sync_activity_time_ = -1.0;
  std::string last_synced_static_url_;
  bool tracker_sync_active_ = false;

  std::thread network_thread_;
  std::atomic<bool> network_thread_running_{false};
};
