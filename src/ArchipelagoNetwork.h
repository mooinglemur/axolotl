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
#include <mutex>
#include <nlohmann/json.hpp>
#include <queue>
#include <string>
#include <vector>

struct MessagePart {
  std::string text;
  uint32_t color = 0xFFFFFFFF; // RGBA (default white)
};

struct RichMessage {
  double timestamp;
  std::vector<MessagePart> parts;
  int sender_slot = -1;
  int receiver_slot = -1;
};

struct Hint {
  int64_t item_id;
  int64_t location_id;
  int receiver_slot;
  int finder_slot;
  bool found;
  int item_flags;
};

class ArchipelagoNetwork {
public:
  ArchipelagoNetwork();
  ~ArchipelagoNetwork();

  enum class State { Disconnected, Connecting, Connected };
  State GetState() const;
  void Disconnect();

  void SetMaxHistory(int max_history) { max_history_size_ = max_history; }

  void Connect(const std::string &url, const std::string &slot,
               const std::string &password = "");
  void Update();
  void SendChat(const std::string &message);

  int GetLocalSlot() const { return local_slot_; }
  int GetGlobalSlot() const { return (team_ << 16) | local_slot_; }

  bool IsConnected() const;

  const std::vector<RichMessage> &GetChatHistory() const {
    return chat_history_;
  }
  const std::vector<RichMessage> &GetItemHistory() const {
    return item_history_;
  }
  const std::vector<RichMessage> &GetReceivedItemsHistory() const {
    return received_items_history_;
  }
  const std::vector<Hint> &GetHints() const { return hints_; }
  const std::map<int, std::string> &GetPlayerNames() const {
    return player_names_;
  }
  const std::map<int64_t, std::string> &GetItemNames() const {
    return item_names_;
  }
  const std::map<int64_t, std::string> &GetLocationNames() const {
    return location_names_;
  }

  // Callbacks for UI
  std::function<void()> on_history_updated;

private:
  void HandleMessage(const ix::WebSocketMessagePtr &msg);
  void SendConnect();

  ix::WebSocket webSocket_;
  std::string original_url_;
  std::string slot_;
  std::string password_;
  bool is_connected_ = false;
  bool user_wants_connection_ = false;
  int max_history_size_ = 0;
  bool tried_wss_ = false;
  bool tried_ws_ = false;
  bool pending_fallback_ = false;
  std::string pending_url_;
  int local_slot_ = -1;
  int team_ = 0;
  bool clear_item_history_pending_ = false;
  std::string last_requested_url_;
  std::string last_requested_slot_;

  std::queue<nlohmann::json> message_queue_;
  std::queue<std::string> status_messages_;
  std::mutex queue_mutex_;
  std::mutex status_mutex_;

  // Data Package & Handshake data
  std::map<int64_t, std::string> item_names_;
  std::map<int64_t, std::string> location_names_;
  std::map<int, std::string> player_names_;

  // History buffers
  std::vector<RichMessage> chat_history_;
  std::vector<RichMessage> item_history_;
  std::vector<RichMessage> received_items_history_;
  std::vector<Hint> hints_;

  struct PendingItem {
    int64_t id;
    int sender;
    int receiver;
    int flags;
    bool is_received_packet; // true if from ReceivedItems, false if PrintJSON
                             // item
  };
  std::vector<PendingItem> pending_items_;

  void SendGetDataPackage();
  void SendSync();
  void SendGetHints();
  void ResolvePendingItems();
  std::string ResolveItemName(int64_t id);

  void CheckDayChange(std::vector<RichMessage> &history, double timestamp,
                      int64_t &last_day);
  int64_t last_chat_day_ = -1;
  int64_t last_item_day_ = -1;
  int64_t last_received_day_ = -1;
};
