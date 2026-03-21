#include "ArchipelagoNetwork.h"
#include "Config.h"
#include <algorithm>
#include <chrono>

using json = nlohmann::json;

// Helper to reliably get a numeric ID from a JSON value that might be a number
// or string
static int64_t get_as_id(const json &j, int64_t default_val = -1) {
  if (j.is_number())
    return j.get<int64_t>();
  if (j.is_string()) {
    try {
      return std::stoll(j.get<std::string>());
    } catch (...) {
    }
  }
  return default_val;
}

ArchipelagoNetwork::ArchipelagoNetwork() {
  webSocket_.setOnMessageCallback(
      [this](const ix::WebSocketMessagePtr &msg) { HandleMessage(msg); });
}

ArchipelagoNetwork::~ArchipelagoNetwork() {
  webSocket_.setOnMessageCallback(nullptr);
  Disconnect();
}

void ArchipelagoNetwork::HandleMessage(const ix::WebSocketMessagePtr &msg) {
  if (msg->type == ix::WebSocketMessageType::Message) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    try {
      auto j = json::parse(msg->str);
      if (j.is_array()) {
        for (auto &packet : j) {
          message_queue_.push(packet);
        }
      }
    } catch (const std::exception &e) {
      std::lock_guard<std::mutex> status_lock(status_mutex_);
      status_messages_.push("JSON parse error: " + std::string(e.what()));
    }
  } else if (msg->type == ix::WebSocketMessageType::Open) {
    std::lock_guard<std::mutex> status_lock(status_mutex_);
    status_messages_.push("WebSocket connected to " + webSocket_.getUrl());
  } else if (msg->type == ix::WebSocketMessageType::Close) {
    std::lock_guard<std::mutex> status_lock(status_mutex_);
    status_messages_.push("WebSocket disconnected from " + webSocket_.getUrl());
    is_connected_ = false;
  } else if (msg->type == ix::WebSocketMessageType::Error) {
    {
      std::lock_guard<std::mutex> status_lock(status_mutex_);
      status_messages_.push("WebSocket error: " + msg->errorInfo.reason +
                            " (URL: " + webSocket_.getUrl() + ")");
    }

    // Only fallback if it's likely an SSL/TLS error
    std::string reason = msg->errorInfo.reason;
    std::transform(reason.begin(), reason.end(), reason.begin(), ::tolower);
    bool is_tls_error = (reason.find("ssl") != std::string::npos ||
                         reason.find("tls") != std::string::npos ||
                         reason.find("handshake") != std::string::npos);

    if (is_tls_error) {
      if (tried_wss_ && !tried_ws_) {
        tried_ws_ = true;
        pending_url_ = "ws://" + original_url_;
        pending_fallback_ = true;
      } else if (tried_ws_ && !tried_wss_ &&
                 original_url_.find("://") == std::string::npos) {
        tried_wss_ = true;
        pending_url_ = "wss://" + original_url_;
        pending_fallback_ = true;
      }
    }
  }
}

void ArchipelagoNetwork::Update() {
  // Handle deferred fallback
  if (pending_fallback_) {
    {
      std::lock_guard<std::mutex> status_lock(status_mutex_);
      status_messages_.push("Executing deferred fallback to " + pending_url_);
    }
    pending_fallback_ = false;
    webSocket_.stop();
    webSocket_.setUrl(pending_url_);
    webSocket_.start();
  }

  // Handle status messages
  std::queue<std::string> local_status;
  {
    std::lock_guard<std::mutex> status_lock(status_mutex_);
    std::swap(local_status, status_messages_);
  }
  double current_time = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count() /
                        1000000.0;

  while (!local_status.empty()) {
    RichMessage rm;
    rm.timestamp = current_time;
    rm.parts.push_back({"[System] " + local_status.front(),
                        0xFFAAAAAA}); // Gray Opaque (AABBGGRR)
    CheckDayChange(chat_history_, current_time, last_chat_day_);
    chat_history_.push_back(rm);
    local_status.pop();
  }

  std::queue<json> local_queue;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    std::swap(local_queue, message_queue_);
  }

  bool history_changed = false;
  if (!chat_history_.empty() || !item_history_.empty() ||
      !received_items_history_.empty()) {
    history_changed = true;
  }

  while (!local_queue.empty()) {
    json packet = local_queue.front();
    local_queue.pop();

    std::string cmd = packet["cmd"];
    if (cmd == "RoomInfo") {
      SendConnect();
    } else if (cmd == "Connected") {
      is_connected_ = true;
      local_slot_ = packet["slot"].get<int>();
      team_ = packet.contains("team") ? packet["team"].get<int>() : 0;

      // Always clear received items on NEW connection (server sends them again)
      received_items_history_.clear();
      last_received_day_ = -1;

      if (clear_item_history_pending_) {
        item_history_.clear();
        hints_.clear();
        player_names_.clear();
        last_item_day_ = -1;
        clear_item_history_pending_ = false;
      }

      // Store player names
      if (packet.contains("players")) {
        for (auto &player : packet["players"]) {
          int p_team = player.contains("team") ? player["team"].get<int>() : 0;
          int p_slot = player["slot"].get<int>();
          std::string p_name =
              player.contains("alias")
                  ? player["alias"].get<std::string>()
                  : (player.contains("name") ? player["name"].get<std::string>()
                                             : "Unknown");

          player_names_[(p_team << 16) | p_slot] = p_name;
          // Also store just by slot for team 0 (legacy/simple)
          if (p_team == 0) {
            player_names_[p_slot] = p_name;
          }
        }
      }

      // Store slot-to-game mapping
      if (packet.contains("slot_info")) {
        for (auto &[slot_str, slot_data] : packet["slot_info"].items()) {
          try {
            int slot_id = std::stoi(slot_str);
            if (slot_data.contains("game")) {
              std::string g_name = slot_data["game"].get<std::string>();
              slot_to_game_[slot_id] = g_name;
              slot_to_game_[(team_ << 16) | slot_id] = g_name;
            }
          } catch (...) {
          }
        }
      }

      SendGetDataPackage();
      SendSync();
      SendGetHints();

      RichMessage rm;
      rm.timestamp = current_time;
      rm.parts.push_back({"Connected as " + slot_ + " (Slot " +
                              std::to_string(local_slot_) + ", Team " +
                              std::to_string(team_) + ")",
                          0xFF00FF00}); // Green Opaque (AABBGGRR)
      CheckDayChange(chat_history_, current_time, last_chat_day_);
      chat_history_.push_back(rm);
      history_changed = true;
    } else if (cmd == "DataPackage") {
      auto &dp = packet["data"];
      if (dp.contains("games")) {
        for (auto &[game_name, game_data] : dp["games"].items()) {
          if (game_data.contains("item_name_to_id")) {
            for (auto &[item_name, item_id] :
                 game_data["item_name_to_id"].items()) {
              item_names_[game_name][item_id.get<int64_t>()] = item_name;
            }
          }
          if (game_data.contains("location_name_to_id")) {
            for (auto &[loc_name, loc_id] :
                 game_data["location_name_to_id"].items()) {
              location_names_[game_name][loc_id.get<int64_t>()] = loc_name;
            }
          }
          if (game_data.contains("entrance_name_to_id")) {
            for (auto &[ent_name, ent_id] :
                 game_data["entrance_name_to_id"].items()) {
              entrance_names_[game_name][ent_id.get<int64_t>()] = ent_name;
            }
          }
        }
      }
      {
        std::lock_guard<std::mutex> status_lock(status_mutex_);
        size_t total_items = 0;
        for (auto const &[game, items] : item_names_)
          total_items += items.size();
        status_messages_.push("Received Data Package (" +
                              std::to_string(total_items) + " items)");
      }
      ResolvePendingItems();
      history_changed = true;
    } else if (cmd == "ConnectionRefused") {
      RichMessage rm;
      rm.timestamp = current_time;
      rm.parts.push_back({"Connection refused: " + packet.dump(),
                          0xFF0000FF}); // Red Opaque (AABBGGRR)
      CheckDayChange(chat_history_, current_time, last_chat_day_);
      chat_history_.push_back(rm);
      history_changed = true;
    } else if (cmd == "PrintJSON") {
      RichMessage rm;
      rm.timestamp = current_time;
      bool is_item_event = false;
      if (packet.contains("type")) {
        std::string type = packet["type"];
        is_item_event =
            (type == "ItemSend" || type == "ItemCheat" || type == "Hint");
      }

      for (auto &part : packet["data"]) {
        std::string type =
            part.contains("type") ? part["type"].get<std::string>() : "text";
        std::string content =
            part.contains("text") ? part["text"].get<std::string>() : "";
        uint32_t color = 0xFFFFFFFF; // White Opaque (AABBGGRR)

        if (type == "player_id") {
          try {
            int pid = (int)get_as_id(content);
            int global_id =
                (player_names_.count(pid)) ? pid : ((team_ << 16) | pid);
            if (player_names_.count(global_id))
              content = player_names_[global_id];

            if (pid == local_slot_) {
              color = 0xFFFF00FF; // Magenta Opaque (AABBGGRR)
            } else {
              color = 0xFFCCCCCC; // Slightly Gray Opaque (AABBGGRR)
            }
          } catch (...) {
          }
        } else if (type == "item_id") {
          try {
            int64_t iid = get_as_id(content);
            int flags = part.contains("flags") ? part["flags"].get<int>() : 0;
            int sid = part.contains("player") ? part["player"].get<int>() : -1;
            content = ResolveItemName(iid, sid);
            if (content.empty()) {
              content = "Unknown Item " + std::to_string(iid);
              if (is_item_event) {
                pending_items_.push_back({iid, 0, sid, flags, false});
              }
            }

            // Colors based on flags
            if (flags & 0x01) {        // Progression
              color = 0xFFFF5FAF;      // Purple Opaque (AABBGGRR)
            } else if (flags & 0x02) { // Useful
              color = 0xFFED9564;      // Blue Opaque (AABBGGRR)
            } else if (flags & 0x04) { // Trap
              color = 0xFF0045FF;      // Red Opaque (AABBGGRR)
            } else {                   // Junk
              color = 0xFFFFFF00;      // Cyan Opaque (AABBGGRR)
            }
          } catch (...) {
          }
        } else if (type == "location_id") {
          try {
            int64_t lid = get_as_id(content);
            int sid = part.contains("player") ? part["player"].get<int>() : -1;
            std::string loc_name = ResolveLocationName(lid, sid);
            if (!loc_name.empty())
              content = loc_name;
            color = 0xFF00FF00; // Green Opaque (AABBGGRR)
          } catch (...) {
          }
        }
        rm.parts.push_back({content, color});
      }

      if (is_item_event) {
        if (packet.contains("type")) {
          std::string type = packet["type"];
          if (type == "ItemSend" || type == "ItemCheat") {
            int r_slot = (int)get_as_id(
                packet.contains("receiving")
                    ? packet["receiving"]
                    : (packet.contains("receiving_player")
                           ? packet["receiving_player"]
                           : (packet.contains("player") ? packet["player"]
                                                        : json(-1))));
            rm.receiver_slot = (r_slot != -1) ? ((team_ << 16) | r_slot) : -1;

            int s_slot = (int)get_as_id(
                packet.contains("item") && packet["item"].contains("player")
                    ? packet["item"]["player"]
                    : json(-1));
            rm.sender_slot = (s_slot != -1) ? ((team_ << 16) | s_slot) : -1;

            if (packet.contains("item") &&
                packet["item"].contains("location")) {
              int64_t loc_id = get_as_id(packet["item"]["location"]);
              for (auto &hint : hints_) {
                if (hint.location_id == loc_id &&
                    hint.finder_slot == rm.sender_slot) {
                  hint.found = true;
                }
              }
            }
          } else if (type == "Hint") {
            try {
              Hint h;
              h.item_id = get_as_id(packet["item"]["item"]);
              h.location_id = get_as_id(packet["item"]["location"]);

              int r_slot = (int)get_as_id(packet.contains("receiving_player")
                                              ? packet["receiving_player"]
                                              : (packet.contains("receiving")
                                                     ? packet["receiving"]
                                                     : json(-1)));
              h.receiver_slot = (r_slot != -1) ? ((team_ << 16) | r_slot) : -1;

              int f_slot =
                  (int)get_as_id(packet.contains("finding_player")
                                     ? packet["finding_player"]
                                     : (packet.contains("finding")
                                            ? packet["finding"]
                                            : (packet["item"].contains("player")
                                                   ? packet["item"]["player"]
                                                   : json(-1))));
              h.finder_slot = (f_slot != -1) ? ((team_ << 16) | f_slot) : -1;

              h.found = packet.contains("found") ? packet["found"].get<bool>()
                                                 : false;
              h.item_flags = packet["item"].contains("flags")
                                 ? packet["item"]["flags"].get<int>()
                                 : 0;
              hints_.push_back(h);

              rm.receiver_slot = h.receiver_slot;
              rm.sender_slot = h.finder_slot;
            } catch (...) {
            }
          }
        }
        CheckDayChange(item_history_, current_time, last_item_day_);
        item_history_.push_back(rm);
      } else {
        CheckDayChange(chat_history_, current_time, last_chat_day_);
        chat_history_.push_back(rm);
      }
      history_changed = true;
    } else if (cmd == "ReceivedItems") {
      int index = packet.contains("index") ? packet["index"].get<int>() : 0;
      if (index == 0) {
        received_items_history_.clear();
        last_received_day_ = -1;
      }
      for (const auto &item : packet["items"]) {
        int64_t iid = get_as_id(item["item"]);
        int flags = item.contains("flags") ? item["flags"].get<int>() : 0;
        int sid = item.contains("player") ? item["player"].get<int>() : -1;
        std::string name = ResolveItemName(iid, local_slot_);

        RichMessage rm;
        rm.timestamp = current_time;
        uint32_t color = 0xFFFFFF00; // Junk/Cyan default Opaque (AABBGGRR)
        if (flags & 0x01)
          color = 0xFFFF5FAF; // Purple
        else if (flags & 0x02)
          color = 0xFFED9564; // Blue
        else if (flags & 0x04)
          color = 0xFF0045FF; // Red

        if (name.empty()) {
          pending_items_.push_back({iid, sid, local_slot_, flags, true});
          rm.parts.push_back({"Unknown Item " + std::to_string(iid), color});
        } else {
          rm.parts.push_back({name, color});
        }
        CheckDayChange(received_items_history_, current_time,
                       last_received_day_);
        received_items_history_.push_back(rm);
      }
      history_changed = true;
    } else if (cmd == "Retrieved") {
      int hint_count = 0;
      if (packet.contains("keys") && packet["keys"].is_object()) {
        auto &keys = packet["keys"];
        std::string hint_key = "_read_hints_" + std::to_string(team_) + "_" +
                               std::to_string(local_slot_);

        if (keys.contains(hint_key)) {
          auto &hints_json = keys[hint_key];
          if (hints_json.is_array()) {
            hints_.clear();
            for (auto &h_val : hints_json) {
              try {
                Hint h;
                h.item_id =
                    get_as_id(h_val.contains("item") ? h_val["item"] : json(0));
                h.location_id = get_as_id(
                    h_val.contains("location") ? h_val["location"] : json(0));
                int f_slot = (int)get_as_id(h_val.contains("finding_player")
                                                ? h_val["finding_player"]
                                                : (h_val.contains("finding")
                                                       ? h_val["finding"]
                                                       : json(-1)));

                int r_slot = (int)get_as_id(
                    h_val.contains("receiving_player")
                        ? h_val["receiving_player"]
                        : (h_val.contains("receiving")
                               ? h_val["receiving"]
                               : (h_val.contains("player") ? h_val["player"]
                                                           : json(-1))));

                if (h_val.contains("entrance")) {
                  if (h_val["entrance"].is_string()) {
                    h.entrance_name = h_val["entrance"].get<std::string>();
                  } else {
                    int64_t eid = get_as_id(h_val["entrance"]);
                    h.entrance_name = ResolveEntranceName(eid, f_slot);
                  }
                }

                // Apply team offset to slot IDs
                h.receiver_slot =
                    (r_slot != -1) ? ((team_ << 16) | r_slot) : -1;
                h.finder_slot = (f_slot != -1) ? ((team_ << 16) | f_slot) : -1;

                h.found = h_val.contains("found") ? h_val["found"].get<bool>()
                                                  : false;
                h.item_flags =
                    h_val.contains("item_flags")
                        ? h_val["item_flags"].get<int>()
                        : (h_val.contains("flags") ? h_val["flags"].get<int>()
                                                   : 0);

                hints_.push_back(h);
                hint_count++;
              } catch (...) {
              }
            }
          }
        }
      }
      {
        std::lock_guard<std::mutex> status_lock(status_mutex_);
        status_messages_.push("Retrieved " + std::to_string(hint_count) +
                              " hints from server storage");
      }
      history_changed = true;
    }
  }

  if (max_history_size_ > 0) {
    if ((int)chat_history_.size() > max_history_size_) {
      chat_history_.erase(chat_history_.begin(),
                          chat_history_.begin() +
                              (chat_history_.size() - max_history_size_));
      history_changed = true;
    }
    if ((int)item_history_.size() > max_history_size_) {
      item_history_.erase(item_history_.begin(),
                          item_history_.begin() +
                              (item_history_.size() - max_history_size_));
      history_changed = true;
    }
    if ((int)received_items_history_.size() > max_history_size_) {
      received_items_history_.erase(
          received_items_history_.begin(),
          received_items_history_.begin() +
              (received_items_history_.size() - max_history_size_));
      history_changed = true;
    }
  }

  if (history_changed && on_history_updated) {
    on_history_updated();
  }
}

void ArchipelagoNetwork::SendChat(const std::string &message) {
  if (message == "/debug fakefeed") {
    item_history_.clear();
    last_item_day_ = -1;

    if (item_names_.empty() || player_names_.empty()) {
      RichMessage rm;
      rm.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count() /
                     1000000.0;
      rm.parts.push_back({"Not connected or no data package.", 0xFF0000FF});
      item_history_.push_back(rm);
      if (on_history_updated)
        on_history_updated();
      return;
    }

    std::vector<std::string> p_names;
    std::vector<int> p_slots;
    for (auto const &[id, name] : player_names_) {
      if (name != "Unknown" && name != "Server") {
        p_names.push_back(name);
        p_slots.push_back(id);
      }
    }
    if (p_names.empty()) {
      p_names.push_back("Player");
      p_slots.push_back(1);
    }

    std::vector<std::string> i_names;
    for (auto const &[gn, items] : item_names_) {
      for (auto const &[id, name] : items) {
        i_names.push_back(name);
      }
    }

    double now = std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count() /
                 1000000.0;
    double start_time = now - (72.0 * 3600.0);

    int num_fakes = 100;
    double time_step = (now - start_time) / num_fakes;

    for (int i = 0; i < num_fakes; ++i) {
      double t = start_time + (i * time_step);
      CheckDayChange(item_history_, t, last_item_day_);

      int s_idx = rand() % p_names.size();
      int r_idx = rand() % p_names.size();
      int i_idx = rand() % i_names.size();

      RichMessage rm;
      rm.timestamp = t;
      rm.sender_slot = p_slots[s_idx];
      rm.receiver_slot = p_slots[r_idx];

      uint32_t name_color =
          0xFFFF00FF; // Magenta default (will just use one color for debug)
      uint32_t item_color = 0xFFED9564; // Blue

      rm.parts.push_back({p_names[s_idx], name_color});
      rm.parts.push_back({" found ", 0xFFFFFFFF});
      rm.parts.push_back({i_names[i_idx], item_color});
      rm.parts.push_back({" for ", 0xFFFFFFFF});
      rm.parts.push_back({p_names[r_idx], name_color});

      item_history_.push_back(rm);
    }

    if (on_history_updated)
      on_history_updated();

    return;
  }

  json packet = json::array();
  packet.push_back({{"cmd", "Say"}, {"text", message}});
  webSocket_.send(packet.dump());
}

std::string ArchipelagoNetwork::ResolveItemName(int64_t id, int slot) {
  std::string game = "";
  if (slot != -1 && slot_to_game_.count(slot)) {
    game = slot_to_game_[slot];
  }

  if (!game.empty() && item_names_.count(game) && item_names_[game].count(id)) {
    return item_names_[game][id];
  }

  // Fallback: search all games if game is unknown or item not found in that
  // game
  for (auto const &[gn, items] : item_names_) {
    if (items.count(id))
      return items.at(id);
  }

  return "";
}

std::string ArchipelagoNetwork::ResolveLocationName(int64_t id, int slot) {
  std::string game = "";
  if (slot != -1 && slot_to_game_.count(slot)) {
    game = slot_to_game_[slot];
  }

  if (!game.empty() && location_names_.count(game) &&
      location_names_[game].count(id)) {
    return location_names_[game][id];
  }

  // Fallback: search all games
  for (auto const &[gn, locations] : location_names_) {
    if (locations.count(id))
      return locations.at(id);
  }

  return "";
}

std::string ArchipelagoNetwork::ResolveEntranceName(int64_t id, int slot) {
  if (id == 0)
    return "";

  std::string game = "";
  if (slot != -1 && slot_to_game_.count(slot)) {
    game = slot_to_game_[slot];
  }

  if (!game.empty() && entrance_names_.count(game) &&
      entrance_names_[game].count(id)) {
    return entrance_names_[game][id];
  }

  // Fallback: search all games
  for (auto const &[gn, entrances] : entrance_names_) {
    if (entrances.count(id))
      return entrances.at(id);
  }

  return "";
}

void ArchipelagoNetwork::ResolvePendingItems() {
  for (auto &pending : pending_items_) {
    std::string name = ResolveItemName(pending.id, pending.receiver);
    if (!name.empty()) {
      if (pending.is_received_packet) {
        std::string search = "Unknown Item " + std::to_string(pending.id);
        for (auto &rm : received_items_history_) {
          for (auto &part : rm.parts) {
            if (part.text == search) {
              part.text = name;
            }
          }
        }
      } else {
        std::string search = "Unknown Item " + std::to_string(pending.id);
        for (auto &rm : item_history_) {
          for (auto &part : rm.parts) {
            size_t pos = part.text.find(search);
            if (pos != std::string::npos) {
              part.text.replace(pos, search.length(), name);
            }
          }
        }
      }
    }
  }
}

void ArchipelagoNetwork::SendGetDataPackage() {
  json packet = json::array();
  packet.push_back({{"cmd", "GetDataPackage"}});
  webSocket_.send(packet.dump());
}

void ArchipelagoNetwork::SendSync() {
  json packet = json::array();
  packet.push_back({{"cmd", "Sync"}});
  webSocket_.send(packet.dump());
}

void ArchipelagoNetwork::SendGetHints() {
  json packet = json::array();
  std::string hint_key = "_read_hints_" + std::to_string(team_) + "_" +
                         std::to_string(local_slot_);
  packet.push_back({{"cmd", "Get"}, {"keys", {hint_key}}});
  webSocket_.send(packet.dump());
}

void ArchipelagoNetwork::SendConnect() {
  json packet = json::array();
  packet.push_back(
      {{"cmd", "Connect"},
       {"password", password_},
       {"game", ""},
       {"name", slot_},
       {"uuid", "axolotl-client"},
       {"version",
        {{"class", "Version"}, {"major", 0}, {"minor", 5}, {"build", 1}}},
       {"items_handling", 7},
       {"tags", {"TextOnly"}}});
  webSocket_.send(packet.dump());
}

void ArchipelagoNetwork::Connect(const std::string &url,
                                 const std::string &slot,
                                 const std::string &password) {
  if (url != last_requested_url_ || slot != last_requested_slot_) {
    clear_item_history_pending_ = true;
  }
  last_requested_url_ = url;
  last_requested_slot_ = slot;

  original_url_ = url;
  slot_ = slot;
  password_ = password;
  tried_wss_ = false;
  tried_ws_ = false;
  pending_fallback_ = false;

  std::string full_url = url;
  if (full_url.find("://") == std::string::npos) {
    tried_wss_ = true;
    full_url = "wss://" + full_url;
  }

  webSocket_.setUrl(full_url);
  webSocket_.enableAutomaticReconnection();
  webSocket_.enablePerMessageDeflate();

  // Configure TLS if we have a bundled CA
  auto ca_path = Config::GetCaBundlePath();
  if (std::filesystem::exists(ca_path)) {
    ix::SocketTLSOptions tls_options;
    tls_options.caFile = ca_path.string();
    webSocket_.setTLSOptions(tls_options);
  }

  webSocket_.start();
  user_wants_connection_ = true;
}

void ArchipelagoNetwork::Disconnect() {
  webSocket_.stop();
  is_connected_ = false;
  user_wants_connection_ = false;
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_messages_.push("Disconnected by user");
  }
}

bool ArchipelagoNetwork::IsConnected() const {
  return GetState() == State::Connected;
}

ArchipelagoNetwork::State ArchipelagoNetwork::GetState() const {
  auto readyState = webSocket_.getReadyState();
  if (readyState == ix::ReadyState::Open && is_connected_) {
    return State::Connected;
  } else if (user_wants_connection_) {
    return State::Connecting;
  }
  return State::Disconnected;
}

void ArchipelagoNetwork::CheckDayChange(std::vector<RichMessage> &history,
                                        double timestamp, int64_t &last_day) {
  time_t t = (time_t)timestamp;
  struct tm *tm_info = localtime(&t);

  int64_t current_day = (int64_t)(tm_info->tm_year + 1900) * 10000 +
                        (int64_t)(tm_info->tm_mon + 1) * 100 +
                        (int64_t)tm_info->tm_mday;

  if (last_day != -1 && current_day != last_day) {
    RichMessage dm;
    struct tm day_start = *tm_info;
    day_start.tm_hour = 0;
    day_start.tm_min = 0;
    day_start.tm_sec = 0;
    dm.timestamp = (double)mktime(&day_start);

    char buf[64];
    strftime(buf, sizeof(buf), "Day changed to %Y-%m-%d", tm_info);
    dm.parts.push_back({buf, 0xFFAAAAAA});
    history.push_back(dm);
  }
  last_day = current_day;
}
