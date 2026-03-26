#include "ArchipelagoNetwork.h"
#include "Config.h"
#include "version.h"
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

static double GetCurrentTimestamp() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
             .count() /
         1000000.0;
}

// --- ArchipelagoSession ---

ArchipelagoSession::ArchipelagoSession(ArchipelagoNetwork *manager,
                                       const std::string &name)
    : manager_(manager), name_(name) {
  webSocket_.setOnMessageCallback(
      [this](const ix::WebSocketMessagePtr &msg) { HandleMessage(msg); });
}

ArchipelagoSession::~ArchipelagoSession() {
  Disconnect();
  webSocket_.setOnMessageCallback(nullptr);
}

void ArchipelagoSession::Connect(const std::string &url,
                                 const std::string &password) {
  original_url_ = url;
  password_ = password;
  tried_wss_ = false;
  tried_ws_ = false;
  pending_fallback_ = false;
  connection_error_time_ = -1.0;

  metadata_ = manager_->GetOrCreateMetadata(url);

  std::string full_url = url;
  if (full_url.find("://") == std::string::npos) {
    tried_wss_ = true;
    full_url = "wss://" + full_url;
  }

  webSocket_.setUrl(full_url);
  webSocket_.enableAutomaticReconnection();
  webSocket_.enablePerMessageDeflate();

  std::string user_agent = "AxolotlAPTextClient/";
  user_agent += AXOLOTL_VERSION_STRING;
#ifdef GIT_HASH
  user_agent += " (git " + std::string(GIT_HASH) + ")";
#endif
  webSocket_.setExtraHeaders({{"User-Agent", user_agent}});

  auto ca_path = Config::GetCaBundlePath();
  if (std::filesystem::exists(ca_path)) {
    ix::SocketTLSOptions tls_options;
    tls_options.caFile = ca_path.string();
    webSocket_.setTLSOptions(tls_options);
  }

  webSocket_.start();
  user_wants_connection_ = true;
  manager_->OnStatusMessage(this, "Connecting to " + full_url + "...");
}

void ArchipelagoSession::Disconnect() {
  if (!user_wants_connection_)
    return;
  webSocket_.stop();
  is_connected_ = false;
  user_wants_connection_ = false;
  manager_->OnStatusMessage(this, "Disconnected by user");
}

ArchipelagoSession::State ArchipelagoSession::GetState() const {
  auto readyState = webSocket_.getReadyState();
  if (readyState == ix::ReadyState::Open && is_connected_) {
    return State::Connected;
  } else if (user_wants_connection_) {
    return State::Connecting;
  }
  return State::Disconnected;
}

void ArchipelagoSession::HandleMessage(const ix::WebSocketMessagePtr &msg) {
  if (msg->type == ix::WebSocketMessageType::Message) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    try {
      auto j = json::parse(msg->str);
      if (j.is_array()) {
        for (auto &packet : j) {
          message_queue_.push({packet, GetCurrentTimestamp()});
        }
        manager_->WakeUp();
      }
    } catch (const std::exception &e) {
      std::lock_guard<std::mutex> status_lock(status_mutex_);
      status_messages_.push({"JSON parse error: " + std::string(e.what()),
                             GetCurrentTimestamp()});
      manager_->WakeUp(); // Added WakeUp call here
    }
  } else if (msg->type == ix::WebSocketMessageType::Open) {
    connection_error_time_ = -1.0;
    manager_->OnStatusMessage(this,
                              "WebSocket connected to " + webSocket_.getUrl());
  } else if (msg->type == ix::WebSocketMessageType::Close) {
    if (connection_error_time_ <= 0)
      connection_error_time_ = GetCurrentTimestamp();
    is_connected_ = false;
    manager_->OnStatusMessage(this, "WebSocket disconnected from " +
                                        webSocket_.getUrl());
  } else if (msg->type == ix::WebSocketMessageType::Error) {
    if (connection_error_time_ <= 0)
      connection_error_time_ = GetCurrentTimestamp();
    manager_->OnStatusMessage(this,
                              "WebSocket error: " + msg->errorInfo.reason);

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

bool ArchipelagoSession::Update() {
  bool changed = false;
  if (pending_fallback_) {
    manager_->OnStatusMessage(this,
                              "Executing deferred fallback to " + pending_url_);
    pending_fallback_ = false;
    webSocket_.stop();
    webSocket_.setUrl(pending_url_);
    webSocket_.start();
    changed = true;
  }

  // Timeout logic
  if (user_wants_connection_ && !is_connected_ && connection_error_time_ > 0) {
    if (GetCurrentTimestamp() - connection_error_time_ > 300.0) {
      Disconnect();
      RichMessage rm;
      rm.timestamp = GetCurrentTimestamp();
      rm.populate_local_time();
      rm.parts.push_back(
          {"[System][" + name_ +
               "] Reconnection attempts timed out after 5 minutes.",
           0xFF0000FF});
      manager_->OnGlobalMessage(this, rm, false);
      changed = true;
    }
  }

  // Handle status messages
  std::queue<QueuedStatus> local_status;
  {
    std::lock_guard<std::mutex> status_lock(status_mutex_);
    std::swap(local_status, status_messages_);
  }
  if (!local_status.empty())
    changed = true;
  while (!local_status.empty()) {
    auto q_status = local_status.front();
    local_status.pop();
    RichMessage rm;
    rm.timestamp = q_status.timestamp;
    rm.populate_local_time();
    rm.parts.push_back(
        {"[System][" + name_ + "] " + q_status.message, 0xFFAAAAAA});
    manager_->OnGlobalMessage(this, rm, false);
  }

  std::queue<QueuedPacket> local_queue;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    std::swap(local_queue, message_queue_);
  }

  if (!local_queue.empty())
    changed = true;
  while (!local_queue.empty()) {
    auto q_packet = local_queue.front();
    local_queue.pop();
    json &packet = q_packet.packet;
    double msg_time = q_packet.timestamp;
    std::string cmd = packet["cmd"];

    if (cmd == "RoomInfo") {
      SendConnect();
    } else if (cmd == "Connected") {
      is_connected_ = true;
      connection_error_time_ = -1.0;
      local_slot_ = packet["slot"].get<int>();
      team_ = packet.contains("team") ? packet["team"].get<int>() : 0;
      last_received_day_ = -1;

      if (packet.contains("players") && metadata_) {
        for (auto &player : packet["players"]) {
          int p_team = player.contains("team") ? player["team"].get<int>() : 0;
          int p_slot = player["slot"].get<int>();
          std::string p_name =
              player.contains("alias")
                  ? player["alias"].get<std::string>()
                  : (player.contains("name") ? player["name"].get<std::string>()
                                             : "Unknown");
          metadata_->player_names[(p_team << 16) | p_slot] = p_name;
          if (p_team == 0)
            metadata_->player_names[p_slot] = p_name;
          if (p_team == team_ && p_slot == local_slot_) {
            // Player status logic removed as per simplification request
          }
        }
      }

      if (packet.contains("checked_locations") &&
          packet["checked_locations"].is_array()) {
        for (auto &loc : packet["checked_locations"]) {
          checked_locations_.insert(get_as_id(loc));
        }
      }

      if (packet.contains("slot_info") && metadata_) {
        for (auto &[slot_str, slot_data] : packet["slot_info"].items()) {
          try {
            int slot_id = std::stoi(slot_str);
            if (slot_data.contains("game")) {
              std::string g_name = slot_data["game"].get<std::string>();
              metadata_->slot_to_game[slot_id] = g_name;
              metadata_->slot_to_game[(team_ << 16) | slot_id] = g_name;
            }
          } catch (...) {
          }
        }
      }

      SendGetDataPackage();
      SendSync();
      SendGetHints();

      RichMessage rm;
      rm.timestamp = msg_time;
      rm.populate_local_time();
      rm.parts.push_back(
          {name_ + " connected (Slot " + std::to_string(local_slot_) + ")",
           0xFF00FF00});
      manager_->OnGlobalMessage(this, rm, false);

    } else if (cmd == "DataPackage" && metadata_) {
      auto &dp = packet["data"];
      if (dp.contains("games")) {
        for (auto &[game_name, game_data] : dp["games"].items()) {
          if (game_data.contains("item_name_to_id")) {
            for (auto &[item_name, item_id] :
                 game_data["item_name_to_id"].items())
              metadata_->item_names[game_name][item_id.get<int64_t>()] =
                  item_name;
          }
          if (game_data.contains("location_name_to_id")) {
            for (auto &[loc_name, loc_id] :
                 game_data["location_name_to_id"].items()) {
              int64_t lid = loc_id.get<int64_t>();
              metadata_->location_names[game_name][lid] = loc_name;
              metadata_->location_name_to_id[game_name][loc_name] = lid;
            }
          }
          if (game_data.contains("entrance_name_to_id")) {
            for (auto &[ent_name, ent_id] :
                 game_data["entrance_name_to_id"].items())
              metadata_->entrance_names[game_name][ent_id.get<int64_t>()] =
                  ent_name;
          }
        }
      }
      metadata_->data_package_received = true;
      ResolvePendingItems();
      manager_->ReResolveHistory();
    } else if (cmd == "ConnectionRefused") {
      if (connection_error_time_ <= 0)
        connection_error_time_ = GetCurrentTimestamp();
      RichMessage rm;
      rm.timestamp = msg_time;
      rm.populate_local_time();
      rm.parts.push_back(
          {"Connection refused for " + name_ + ": " + packet.dump(),
           0xFF0000FF});
      manager_->OnGlobalMessage(this, rm, false);
    } else if (cmd == "PrintJSON") {
      RichMessage rm;
      rm.timestamp = msg_time;
      rm.populate_local_time();
      rm.source_slot = name_;
      bool is_item_event = false;
      bool is_status_msg = false;
      if (packet.contains("type")) {
        std::string type_str = packet["type"];
        is_item_event = (type_str == "ItemSend" || type_str == "ItemCheat" ||
                         type_str == "Hint");
        is_status_msg =
            (type_str == "Join" || type_str == "Part" ||
             type_str == "TagsChanged" || type_str == "CommandResult");
      }

      for (auto &part : packet["data"]) {
        std::string type =
            part.contains("type") ? part["type"].get<std::string>() : "text";
        std::string content =
            part.contains("text") ? part["text"].get<std::string>() : "";
        uint32_t color =
            (is_status_msg && !is_item_event) ? 0xFFAAAAAA : 0xFFFFFFFF;

        if (type == "player_id") {
          try {
            int pid = part.contains("player") ? (int)get_as_id(part["player"])
                                              : (int)get_as_id(content);
            int global_id = -1;
            if (metadata_) {
              if (pid == -1) {
                // If we don't have an ID, try to find it by name
                for (auto const &[id, name] : metadata_->player_names) {
                  if (name == content) {
                    pid = id;
                    break;
                  }
                }
              }

              global_id = (pid != -1) ? ((team_ << 16) | pid) : -1;
              if (global_id != -1 && metadata_->player_names.count(global_id)) {
                content = metadata_->player_names[global_id];
              }
            }
            color = (pid == local_slot_) ? 0xFFFF00FF : 0xFFCCCCCC;
            rm.parts.push_back({content, color, global_id});
            continue;
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
              if (is_item_event)
                pending_items_.push_back({iid, 0, sid, flags, false});
            }
            if (flags & 0x01)
              color = 0xFFFF5FAF;
            else if (flags & 0x02)
              color = 0xFFED9564;
            else if (flags & 0x04)
              color = 0xFF0045FF;
            else
              color = 0xFFFFFF00;
          } catch (...) {
          }
        } else if (type == "location_id") {
          try {
            int64_t lid = get_as_id(content);
            int sid = part.contains("player") ? part["player"].get<int>() : -1;
            std::string loc_name = ResolveLocationName(lid, sid);
            if (!loc_name.empty())
              content = loc_name;
            color = 0xFF00FF00;
          } catch (...) {
          }
        }
        rm.parts.push_back({content, color});
      }

      if (packet.contains("slot")) {
        int s_slot = (int)get_as_id(packet["slot"]);
        if (s_slot != -1)
          rm.sender_slot = (team_ << 16) | s_slot;
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
                  manager_->SetHintsDirty();
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
              h.source_slot = name_;
              hints_.push_back(h);
              manager_->SetHintsDirty();

              rm.receiver_slot = h.receiver_slot;
              rm.sender_slot = h.finder_slot;
            } catch (...) {
            }
          }
        }
        manager_->OnGlobalMessage(this, rm, true);
      } else {
        manager_->OnGlobalMessage(this, rm, false);
      }
    } else if (cmd == "ReceivedItems") {
      int index = packet.contains("index") ? packet["index"].get<int>() : 0;
      if (index == 0) {
        for (auto &rm : received_items_history_)
          rm.is_reconciled = false;
      }

      int last_match_idx = 0;
      for (const auto &item : packet["items"]) {
        int64_t iid = get_as_id(item["item"]);
        int flags = item.contains("flags") ? item["flags"].get<int>() : 0;
        int sid = item.contains("player") ? item["player"].get<int>() : -1;
        std::string name = ResolveItemName(iid, local_slot_);

        // Try to reconcile with existing item
        bool found = false;
        for (int j = last_match_idx; j < (int)received_items_history_.size();
             ++j) {
          auto &existing = received_items_history_[j];
          if (!existing.is_reconciled && existing.item_id == iid &&
              existing.sender_slot == ((team_ << 16) | sid) &&
              existing.item_flags == flags) {
            existing.is_reconciled = true;
            last_match_idx = j + 1;
            found = true;
            break;
          }
        }

        if (!found) {
          RichMessage rm;
          rm.timestamp = msg_time;
          rm.populate_local_time();
          rm.source_slot = name_;
          rm.item_id = iid;
          rm.item_flags = flags;
          rm.sender_slot = (sid != -1) ? ((team_ << 16) | sid) : -1;
          rm.receiver_slot = (team_ << 16) | local_slot_;
          rm.is_reconciled = true;

          uint32_t color = 0xFFFFFF00;
          if (flags & 0x01)
            color = 0xFFFF5FAF;
          else if (flags & 0x02)
            color = 0xFFED9564;
          else if (flags & 0x04)
            color = 0xFF0045FF;

          if (name.empty()) {
            pending_items_.push_back({iid, sid, local_slot_, flags, true});
            rm.parts.push_back({"Unknown Item " + std::to_string(iid), color});
          } else {
            rm.parts.push_back({name, color});
          }
          // Insert at current match position to maintain order
          received_items_history_.insert(
              received_items_history_.begin() + last_match_idx, rm);
          last_match_idx++;
        }
        manager_->SetItemsDirty();
      }

      // Prune unreconciled items that were supposed to be in this range
      if (index == 0) {
        received_items_history_.erase(
            std::remove_if(
                received_items_history_.begin(), received_items_history_.end(),
                [](const RichMessage &rm) { return !rm.is_reconciled; }),
            received_items_history_.end());
      }
    } else if (cmd == "Retrieved") {
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
                  if (h_val["entrance"].is_string())
                    h.entrance_name = h_val["entrance"].get<std::string>();
                  else
                    h.entrance_name = ResolveEntranceName(
                        get_as_id(h_val["entrance"]), f_slot);
                }
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
                h.source_slot = name_;
                hints_.push_back(h);
                manager_->SetHintsDirty();
              } catch (...) {
              }
            }
          }
        }
      }
    } else if (cmd == "RoomUpdate") {
      if (packet.contains("players")) {
        for (auto &player : packet["players"]) {
          int p_team = player.contains("team") ? player["team"].get<int>() : 0;
          int p_slot = player["slot"].get<int>();
          if (p_team == team_ && p_slot == local_slot_) {
            // Player status logic removed as per simplification request
          }
        }
      }
      if (packet.contains("checked_locations") &&
          packet["checked_locations"].is_array()) {
        for (auto &loc : packet["checked_locations"]) {
          checked_locations_.insert(get_as_id(loc));
        }
        manager_->SetItemsDirty();
      }
    }
  }
  return changed;
}

void ArchipelagoSession::SendConnect() {
  json packet = json::array();
  std::string uuid = "axolotl-client-" + name_;
#ifdef GIT_HASH
  uuid += "-" + std::string(GIT_HASH);
#endif
  packet.push_back(
      {{"cmd", "Connect"},
       {"password", password_},
       {"game", ""},
       {"name", name_},
       {"uuid", uuid},
       {"version",
        {{"class", "Version"}, {"major", 0}, {"minor", 5}, {"build", 1}}},
       {"items_handling", 7},
       {"tags", {"TextOnly", "Tracker"}}});
  webSocket_.send(packet.dump());
}

void ArchipelagoSession::SendChat(const std::string &message) {
  json packet = json::array();
  packet.push_back({{"cmd", "Say"}, {"text", message}});
  webSocket_.send(packet.dump());
}

void ArchipelagoSession::SendGetDataPackage() {
  json packet = json::array();
  packet.push_back({{"cmd", "GetDataPackage"}});
  webSocket_.send(packet.dump());
}

void ArchipelagoSession::SendSync() {
  json packet = json::array();
  packet.push_back({{"cmd", "Sync"}});
  webSocket_.send(packet.dump());
}

void ArchipelagoSession::SendGetHints() {
  json packet = json::array();
  std::string hint_key = "_read_hints_" + std::to_string(team_) + "_" +
                         std::to_string(local_slot_);
  packet.push_back({{"cmd", "Get"}, {"keys", {hint_key}}});
  webSocket_.send(packet.dump());
}

void ArchipelagoSession::ReResolveHistory() {
  manager_->ReResolveHistoryVector(received_items_history_);
}

std::string ArchipelagoSession::ResolveItemName(int64_t id, int slot) {
  if (!metadata_)
    return "";
  std::string game = (slot != -1 && metadata_->slot_to_game.count(slot))
                         ? metadata_->slot_to_game[slot]
                         : "";
  if (!game.empty() && metadata_->item_names.count(game) &&
      metadata_->item_names[game].count(id))
    return metadata_->item_names[game][id];
  for (auto const &[gn, items] : metadata_->item_names)
    if (items.count(id))
      return items.at(id);
  return "";
}

std::string ArchipelagoSession::ResolveLocationName(int64_t id, int slot) {
  if (!metadata_)
    return "";
  std::string game = (slot != -1 && metadata_->slot_to_game.count(slot))
                         ? metadata_->slot_to_game[slot]
                         : "";
  if (!game.empty() && metadata_->location_names.count(game) &&
      metadata_->location_names[game].count(id))
    return metadata_->location_names[game][id];
  for (auto const &[gn, locations] : metadata_->location_names)
    if (locations.count(id))
      return locations.at(id);
  return "";
}

int64_t ArchipelagoSession::ResolveLocationID(const std::string &name,
                                              int slot) {
  if (!metadata_)
    return -1;

  if (slot != -1) {
    if (metadata_->slot_to_game.count(slot)) {
      std::string game = metadata_->slot_to_game.at(slot);
      if (metadata_->location_name_to_id.count(game)) {
        auto &locations = metadata_->location_name_to_id.at(game);
        if (locations.count(name))
          return locations.at(name);

        // Prefix-aware fallback for multiworld logs
        std::string prefix = game + ": ";
        if (name.find(prefix) == 0) {
          std::string stripped = name.substr(prefix.length());
          if (locations.count(stripped))
            return locations.at(stripped);
        }
      }
    }
    return -1; // If a specific slot was requested, don't fall back to global
               // search
  }

  // Fallback global search for untargeted resolution
  for (auto const &[gn, locations] : metadata_->location_name_to_id)
    if (locations.count(name))
      return locations.at(name);
  return -1;
}

std::string ArchipelagoSession::ResolveEntranceName(int64_t id, int slot) {
  if (id == 0 || !metadata_)
    return "";
  std::string game = (slot != -1 && metadata_->slot_to_game.count(slot))
                         ? metadata_->slot_to_game[slot]
                         : "";
  if (!game.empty() && metadata_->entrance_names.count(game) &&
      metadata_->entrance_names[game].count(id))
    return metadata_->entrance_names[game][id];
  for (auto const &[gn, entrances] : metadata_->entrance_names)
    if (entrances.count(id))
      return entrances.at(id);
  return "";
}

void ArchipelagoSession::ResolvePendingItems() {
  for (auto &pending : pending_items_) {
    std::string name = ResolveItemName(pending.id, pending.receiver);
    if (!name.empty()) {
      std::string search = "Unknown Item " + std::to_string(pending.id);
      if (pending.is_received_packet) {
        for (auto &rm : received_items_history_)
          for (auto &part : rm.parts)
            if (part.text == search)
              part.text = name;
      }
    }
  }
}

// --- ArchipelagoNetwork (Manager) ---

ArchipelagoNetwork::ArchipelagoNetwork() {}

std::string ArchipelagoNetwork::MaskURL(const std::string &url) {
  size_t protocol_pos = url.find("://");
  if (protocol_pos != std::string::npos) {
    std::string protocol = url.substr(0, protocol_pos + 3);
    return protocol + std::string(url.length() - protocol.length(), '*');
  }
  return std::string(url.length(), '*');
}
ArchipelagoNetwork::~ArchipelagoNetwork() {
  sessions_.clear(); // This will destroy sessions and stop their websockets
}

ArchipelagoSession *ArchipelagoNetwork::AddSession(const std::string &name) {
  if (GetSession(name))
    return GetSession(name);
  sessions_.push_back(std::make_unique<ArchipelagoSession>(this, name));
  slots_dirty_ = true;
  return sessions_.back().get();
}

void ArchipelagoNetwork::RemoveSession(const std::string &name) {
  for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
    if ((*it)->GetName() == name) {
      sessions_.erase(it);
      slots_dirty_ = true;
      SetItemsDirty();
      SetHintsDirty();
      if (on_history_updated)
        on_history_updated();
      return;
    }
  }
}

void ArchipelagoNetwork::DisconnectAll() {
  for (auto &session : sessions_) {
    session->Disconnect();
  }
}

ArchipelagoSession *ArchipelagoNetwork::GetSession(const std::string &name) {
  for (auto &s : sessions_)
    if (s->GetName() == name)
      return s.get();
  return nullptr;
}

std::shared_ptr<ServerMetadata>
ArchipelagoNetwork::GetOrCreateMetadata(const std::string &url) {
  if (url_to_metadata_.count(url))
    return url_to_metadata_[url];
  auto metadata = std::make_shared<ServerMetadata>();
  url_to_metadata_[url] = metadata;
  return metadata;
}

bool ArchipelagoNetwork::Update() {
  bool changed = false;
  for (auto &session : sessions_) {
    bool was_connected = session->IsConnected();
    if (session->Update())
      changed = true;
    if (was_connected != session->IsConnected()) {
      slots_dirty_ = true;
      aggregated_items_dirty_ = true;
      aggregated_hints_dirty_ = true;
      changed = true;
    }
  }
  return changed;
}

bool ArchipelagoNetwork::IsAnySessionActive() const {
  for (const auto &s : sessions_) {
    if (s->GetState() != State::Disconnected)
      return true;
  }
  return false;
}

const std::set<int> &ArchipelagoNetwork::GetConnectedSlots() {
  if (slots_dirty_) {
    connected_slots_cache_.clear();
    for (const auto &s : sessions_) {
      if (s->IsConnected()) {
        connected_slots_cache_.insert((s->GetTeam() << 16) | s->GetLocalSlot());
        if (s->GetTeam() == 0)
          connected_slots_cache_.insert(s->GetLocalSlot());
      }
    }
    slots_dirty_ = false;
  }
  return connected_slots_cache_;
}

void ArchipelagoNetwork::SendChat(const std::string &session_name,
                                  const std::string &message) {
  auto s = GetSession(session_name);
  if (s)
    s->SendChat(message);
}

void ArchipelagoNetwork::OnGlobalMessage(ArchipelagoSession *session,
                                         const RichMessage &msg,
                                         bool is_item_feed) {
  // Always allow status messages (not attributed to a game event) through
  bool is_status =
      !is_item_feed && !msg.parts.empty() &&
      (msg.parts[0].text.find("[System]") != std::string::npos ||
       msg.parts[0].text.find(" connected (Slot ") != std::string::npos ||
       msg.parts[0].text.find("Connection refused") != std::string::npos);

  if (!is_status && !IsMasterSession(session))
    return;

  if (is_item_feed) {
    SetItemsDirty(); // Replaced aggregated_items_dirty_ = true;
    CheckDayChange(item_history_, msg.timestamp, last_item_day_);
    item_history_.push_back(msg);
  } else {
    CheckDayChange(chat_history_, msg.timestamp, last_chat_day_);
    chat_history_.push_back(msg);
  }

  if (max_history_size_ > 0) {
    if ((int)chat_history_.size() > max_history_size_)
      chat_history_.erase(chat_history_.begin(),
                          chat_history_.begin() +
                              (chat_history_.size() - max_history_size_));
    if ((int)item_history_.size() > max_history_size_)
      item_history_.erase(item_history_.begin(),
                          item_history_.begin() +
                              (item_history_.size() - max_history_size_));
  }

  if (on_history_updated)
    on_history_updated();
}

std::string ArchipelagoNetwork::ResolveItemName(int64_t id, int slot) {
  for (auto &s : sessions_) {
    std::string name = s->ResolveItemName(id, slot);
    if (!name.empty())
      return name;
  }
  return "";
}

std::string ArchipelagoNetwork::ResolveLocationName(int64_t id, int slot) {
  for (auto &s : sessions_) {
    std::string name = s->ResolveLocationName(id, slot);
    if (!name.empty())
      return name;
  }
  return "";
}

int64_t ArchipelagoNetwork::ResolveLocationID(const std::string &name,
                                              int slot) {
  // 1. Try resolving as-is
  for (auto &session : sessions_) {
    int64_t id = session->ResolveLocationID(name, slot);
    if (id != -1)
      return id;
  }

  // 2. Try stripping prefix AND resolving with slot specificity
  size_t colon_pos = name.find(": ");
  if (colon_pos != std::string::npos) {
    std::string stripped_name = name.substr(colon_pos + 2);
    for (auto &session : sessions_) {
      int64_t id = session->ResolveLocationID(stripped_name, slot);
      if (id != -1)
        return id;
    }
  }

  return -1;
}

std::string ArchipelagoNetwork::ResolveEntranceName(int64_t id, int slot) {
  for (auto &s : sessions_) {
    std::string name = s->ResolveEntranceName(id, slot);
    if (!name.empty())
      return name;
  }
  return "";
}

std::string ArchipelagoNetwork::ResolvePlayerName(int slot) {
  for (auto &s : sessions_) {
    const auto &names = s->GetPlayerNames();
    if (names.count(slot))
      return names.at(slot);
  }
  return "Unknown Player " + std::to_string(slot);
}

std::string ArchipelagoNetwork::ResolvePlayerGame(int slot) {
  for (auto const &[url, metadata] : url_to_metadata_) {
    if (metadata->slot_to_game.count(slot))
      return metadata->slot_to_game.at(slot);
  }
  return "";
}

void ArchipelagoNetwork::OnStatusMessage(ArchipelagoSession *session,
                                         const std::string &msg) {
  RichMessage rm;
  rm.timestamp = GetCurrentTimestamp();
  rm.populate_local_time();

  std::string final_msg = msg;
  if (settings_ && settings_->streamer_mode) {
    static const std::vector<std::string> keywords = {
        "Connecting to ", "WebSocket connected to ",
        "WebSocket disconnected from ", "Unable to connect to ",
        "Executing deferred fallback to "};

    for (const auto &kw : keywords) {
      size_t pos = final_msg.find(kw);
      if (pos != std::string::npos) {
        size_t start = pos + kw.length();
        size_t end = final_msg.find_first_of(" \n", start);
        if (end != std::string::npos &&
            final_msg.compare(end, 9, " on port ") == 0) {
          size_t port_end = final_msg.find_first_of(" \n", end + 9);
          end = port_end;
        }

        bool has_dots = false;
        if (end == std::string::npos) {
          end = final_msg.length();
          if (final_msg.length() >= 3 &&
              final_msg.substr(final_msg.length() - 3) == "...") {
            end -= 3;
            has_dots = true;
          }
        }

        std::string url = final_msg.substr(start, end - start);
        final_msg.replace(start, end - start, MaskURL(url));
        break;
      }
    }
  }

  MessagePart p;
  p.text = "[System] " + final_msg;
  p.color = 0xFFC0A5A5; // Dim Blue-Gray
  rm.parts.push_back(p);

  CheckDayChange(chat_history_, rm.timestamp, last_chat_day_);
  chat_history_.push_back(rm);

  if (max_history_size_ > 0 && (int)chat_history_.size() > max_history_size_) {
    chat_history_.erase(chat_history_.begin(),
                        chat_history_.begin() +
                            (chat_history_.size() - max_history_size_));
  }

  if (on_history_updated)
    on_history_updated();
}

bool ArchipelagoNetwork::IsMasterSession(ArchipelagoSession *session) const {
  if (!session)
    return true; // Local/debug messages are always allowed
  if (sessions_.empty())
    return false;
  // Simple rule: first session connected to a specific URL is the master for
  // that URL
  std::string url = session->GetUrl();
  for (const auto &s : sessions_) {
    if (s->GetUrl() == url && s->IsConnected()) {
      return s.get() == session;
    }
  }
  // If none connected yet, the first one trying is the master
  for (const auto &s : sessions_) {
    if (s->GetUrl() == url)
      return s.get() == session;
  }
  return false;
}

const std::vector<RichMessage> &
ArchipelagoNetwork::GetAggregatedReceivedItems() const {
  if (aggregated_items_dirty_) {
    aggregated_items_cache_.clear();
    for (const auto &s : sessions_) {
      const auto &history = s->GetReceivedItems();
      aggregated_items_cache_.insert(aggregated_items_cache_.end(),
                                     history.begin(), history.end());
    }
    std::sort(
        aggregated_items_cache_.begin(), aggregated_items_cache_.end(),
        [](const auto &a, const auto &b) { return a.timestamp < b.timestamp; });
    aggregated_items_dirty_ = false;
  }
  return aggregated_items_cache_;
}

const std::vector<Hint> &ArchipelagoNetwork::GetAggregatedHints() const {
  if (aggregated_hints_dirty_) {
    aggregated_hints_cache_.clear();
    std::set<std::tuple<int64_t, int64_t, int, int>> seen;
    for (const auto &s : sessions_) {
      const auto &hints = s->GetHints();
      for (const auto &h : hints) {
        auto key = std::make_tuple(h.item_id, h.location_id, h.receiver_slot,
                                   h.finder_slot);
        if (seen.insert(key).second) {
          aggregated_hints_cache_.push_back(h);
        }
      }
    }
    aggregated_hints_dirty_ = false;
  }
  return aggregated_hints_cache_;
}

ArchipelagoSession *ArchipelagoNetwork::GetSessionBySlot(int slot) {
  for (auto &session : sessions_) {
    if (session->IsConnected() &&
        (session->GetLocalSlot() | (session->GetTeam() << 16)) == slot)
      return session.get();
    if (session->IsConnected() && session->GetLocalSlot() == slot)
      return session.get();
  }
  return nullptr;
}

void ArchipelagoNetwork::CheckDayChange(std::vector<RichMessage> &history,
                                        double timestamp, int64_t &last_day) {
  struct tm tm_info;
  time_t t = (time_t)timestamp;
#ifdef _WIN32
  localtime_s(&tm_info, &t);
#else
  localtime_r(&t, &tm_info);
#endif

  int64_t current_day = (int64_t)(tm_info.tm_year + 1900) * 10000 +
                        (int64_t)(tm_info.tm_mon + 1) * 100 +
                        (int64_t)tm_info.tm_mday;

  if (last_day != -1 && current_day != last_day) {
    RichMessage dm;
    struct tm day_start = tm_info;
    day_start.tm_hour = 0;
    day_start.tm_min = 0;
    day_start.tm_sec = 0;
    dm.timestamp = (double)mktime(&day_start);
    dm.populate_local_time();

    char buf[64];
    strftime(buf, sizeof(buf), "Day changed to %Y-%m-%d", &tm_info);
    dm.parts.push_back({buf, 0xFFAAAAAA});
    history.push_back(dm);
  }
  last_day = current_day;
}

void ArchipelagoNetwork::ReResolveHistoryVector(
    std::vector<RichMessage> &history) {
  for (auto &rm : history) {
    for (auto &part : rm.parts) {
      if (part.text.find("Unknown Item ") == 0) {
        try {
          int64_t id = std::stoll(part.text.substr(13));
          std::string name = ResolveItemName(id, rm.receiver_slot);
          if (!name.empty())
            part.text = name;
        } catch (...) {
        }
      }
      if (part.text.find("Unknown Location ") == 0) {
        try {
          int64_t id = std::stoll(part.text.substr(17));
          std::string name = ResolveLocationName(id, rm.sender_slot);
          if (!name.empty())
            part.text = name;
        } catch (...) {
        }
      }
    }
  }
}

void ArchipelagoNetwork::ReResolveHistory() {
  ReResolveHistoryVector(chat_history_);
  ReResolveHistoryVector(item_history_);
  for (auto &s : sessions_) {
    s->ReResolveHistory();
  }
  data_version_++;
}

void ArchipelagoNetwork::SetItemsDirty() {
  aggregated_items_dirty_ = true;
  data_version_++;
}

void ArchipelagoNetwork::SetHintsDirty() {
  aggregated_hints_dirty_ = true;
  data_version_++;
}

void ArchipelagoNetwork::ClearChatHistory() {
  chat_history_.clear();
  last_chat_day_ = -1;
  if (on_history_updated)
    on_history_updated();
}

void ArchipelagoNetwork::ClearItemHistory() {
  item_history_.clear();
  last_item_day_ = -1;
  if (on_history_updated)
    on_history_updated();
}

bool ArchipelagoNetwork::IsDataPackageReceived() const {
  for (const auto &session : sessions_) {
    if (session->IsConnected() && session->IsDataPackageReceived())
      return true;
  }
  return false;
}
