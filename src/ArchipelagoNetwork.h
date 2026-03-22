#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include <functional>
#include <ixwebsocket/IXWebSocket.h>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <queue>
#include <set>
#include <string>
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
};

struct Hint {
  int64_t item_id;
  int64_t location_id;
  std::string entrance_name;
  int receiver_slot;
  int finder_slot;
  bool found;
  int item_flags;
  std::string source_slot;
};

struct ServerMetadata {
  std::map<std::string, std::map<int64_t, std::string>> item_names;
  std::map<std::string, std::map<int64_t, std::string>> location_names;
  std::map<std::string, std::map<int64_t, std::string>> entrance_names;
  std::map<int, std::string> player_names;
  std::map<int, std::string> slot_to_game;
  bool data_package_received = false;
};

class ArchipelagoNetwork;

class ArchipelagoSession {
public:
  enum class State { Disconnected, Connecting, Connected };

  ArchipelagoSession(ArchipelagoNetwork *manager, const std::string &name);
  ~ArchipelagoSession();

  void Connect(const std::string &url, const std::string &password);
  void Disconnect();
  bool Update();
  void SendChat(const std::string &message);

  State GetState() const;
  bool IsConnected() const { return GetState() == State::Connected; }
  const std::string &GetName() const { return name_; }
  const std::string &GetUrl() const { return original_url_; }
  int GetLocalSlot() const { return local_slot_; }
  int GetTeam() const { return team_; }

  const std::vector<RichMessage> &GetReceivedItems() const {
    return received_items_history_;
  }
  const std::vector<Hint> &GetHints() const { return hints_; }

  // Metadata accessors
  std::string ResolveItemName(int64_t id, int slot = -1);
  std::string ResolveLocationName(int64_t id, int slot = -1);
  std::string ResolveEntranceName(int64_t id, int slot = -1);
  const std::map<int, std::string> &GetPlayerNames() const {
    if (metadata_) {
      return metadata_->player_names;
    }
    static const std::map<int, std::string> empty_map;
    return empty_map;
  }

private:
  void HandleMessage(const ix::WebSocketMessagePtr &msg);
  void SendConnect();
  void SendGetDataPackage();
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
  using State = ArchipelagoSession::State;
  ArchipelagoNetwork();
  ~ArchipelagoNetwork();

  bool Update();
  void SetMaxHistory(int max_history) { max_history_size_ = max_history; }

  // Session management
  ArchipelagoSession *AddSession(const std::string &name);
  void RemoveSession(const std::string &name);
  void DisconnectAll();
  const std::vector<std::unique_ptr<ArchipelagoSession>> &GetSessions() const {
    return sessions_;
  }
  ArchipelagoSession *GetSession(const std::string &name);
  std::shared_ptr<ServerMetadata> GetOrCreateMetadata(const std::string &url);

  // Global history
  const std::vector<RichMessage> &GetChatHistory() const {
    return chat_history_;
  }
  const std::vector<RichMessage> &GetItemHistory() const {
    return item_history_;
  }

  // Aggregated data
  const std::vector<RichMessage> &GetAggregatedReceivedItems() const;
  const std::vector<Hint> &GetAggregatedHints() const;

  void SendChat(const std::string &session_name, const std::string &message);

  // Global Resolution Helpers
  std::string ResolveItemName(int64_t id, int slot = -1);
  std::string ResolveLocationName(int64_t id, int slot = -1);
  std::string ResolveEntranceName(int64_t id, int slot = -1);
  std::string ResolvePlayerName(int slot);
  const std::set<int> &GetConnectedSlots();
  bool IsAnySessionActive() const;

  void SetHintsDirty() { aggregated_hints_dirty_ = true; }
  void SetItemsDirty() { aggregated_items_dirty_ = true; }

  // Callbacks
  std::function<void()> on_history_updated;
  void SetWakeUpCallback(std::function<void()> cb) { wake_up_callback_ = cb; }
  void WakeUp() {
    if (wake_up_callback_)
      wake_up_callback_();
  }

  // Internal use by sessions
  void OnGlobalMessage(ArchipelagoSession *session, const RichMessage &msg,
                       bool is_item_feed);
  void OnStatusMessage(ArchipelagoSession *session, const std::string &msg);
  void SetSettings(const ConnectionSettings *settings) { settings_ = settings; }
  static std::string MaskURL(const std::string &url);
  bool IsMasterSession(ArchipelagoSession *session) const;
  void ReResolveHistory();

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
};
