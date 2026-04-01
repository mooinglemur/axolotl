#include "ArchipelagoNetwork.h"
#include "Config.h"
#include "DataPackageCache.h"
#include "version.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <ixwebsocket/IXHttpClient.h>

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

double ArchipelagoNetwork::GetCurrentTimestamp() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
             .count() /
         1000000.0;
}

void ArchipelagoNetwork::UpdateSlotActivity(int packed_slot, double timestamp) {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  if (timestamp < 0)
    timestamp = GetCurrentTimestamp();
  auto &slot_stats = global_stats_->slot_info[packed_slot];
  if (timestamp > slot_stats.last_activity_time) {
    slot_stats.last_activity_time = timestamp;
  }
}

// --- ArchipelagoSession ---

ArchipelagoSession::ArchipelagoSession(ArchipelagoNetwork *manager,
                                       const std::string &name)
    : manager_(manager), name_(name) {
  webSocket_.setOnMessageCallback(
      [this](const ix::WebSocketMessagePtr &msg) { HandleMessage(msg); });
}

ArchipelagoSession::~ArchipelagoSession() {
  webSocket_.stop();
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

  pending_url_ = full_url;
  pending_start_ = true;
  pending_stop_ = true;
  user_wants_connection_ = true;
  manager_->OnStatusMessage(this, "Connecting to " + full_url + "...");
}

void ArchipelagoSession::Disconnect() {
  if (!user_wants_connection_)
    return;
  pending_stop_ = true;
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
    if (manager_->IsDebugMode())
      fprintf(stderr, "[RECV][%s] %s\n", name_.c_str(), msg->str.c_str());
    std::lock_guard<std::mutex> lock(queue_mutex_);
    try {
      auto j = json::parse(msg->str);
      if (j.is_array()) {
        for (auto &packet : j) {
          message_queue_.push(
              {packet, ArchipelagoNetwork::GetCurrentTimestamp()});
        }
        manager_->WakeUp();
      }
    } catch (const std::exception &e) {
      std::lock_guard<std::mutex> status_lock(status_mutex_);
      status_messages_.push({"JSON parse error: " + std::string(e.what()),
                             ArchipelagoNetwork::GetCurrentTimestamp()});
      manager_->WakeUp(); // Added WakeUp call here
    }
  } else if (msg->type == ix::WebSocketMessageType::Open) {
    connection_error_time_ = -1.0;
    manager_->OnStatusMessage(this,
                              "WebSocket connected to " + webSocket_.getUrl());
  } else if (msg->type == ix::WebSocketMessageType::Close) {
    if (connection_error_time_ <= 0)
      connection_error_time_ = ArchipelagoNetwork::GetCurrentTimestamp();
    is_connected_ = false;
    manager_->OnStatusMessage(this, "WebSocket disconnected from " +
                                        webSocket_.getUrl());
  } else if (msg->type == ix::WebSocketMessageType::Error) {
    if (connection_error_time_ <= 0)
      connection_error_time_ = ArchipelagoNetwork::GetCurrentTimestamp();
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
    pending_stop_ = true;
    pending_start_ = true;
    // pending_url_ is already set
    changed = true;
  }

  // Timeout logic
  if (user_wants_connection_ && !is_connected_ && connection_error_time_ > 0) {
    if (ArchipelagoNetwork::GetCurrentTimestamp() - connection_error_time_ >
        300.0) {
      Disconnect();
      RichMessage rm;
      rm.timestamp = ArchipelagoNetwork::GetCurrentTimestamp();
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
      if (packet.contains("datapackage_checksums") && metadata_) {
        for (auto &[game, checksum] : packet["datapackage_checksums"].items()) {
          metadata_->datapackage_checksums[game] = checksum.get<std::string>();
        }
      }
      if (packet.contains("hint_cost")) {
        hint_cost_ = packet["hint_cost"].get<int>();
      }
      SendConnect();
    } else if (cmd == "Connected") {
      is_connected_ = true;
      connection_error_time_ = -1.0;
      local_slot_ = packet["slot"].get<int>();
      team_ = packet.contains("team") ? packet["team"].get<int>() : 0;
      last_received_day_ = -1;

      if (packet.contains("hint_points")) {
        hint_points_ = packet["hint_points"].get<int>();
      }
      if (packet.contains("hint_cost")) {
        hint_cost_ = packet["hint_cost"].get<int>();
      }

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
        manager_->SetTotalGames((int)packet["players"].size());
      }

      if (packet.contains("checked_locations") &&
          packet["checked_locations"].is_array()) {
        for (auto &loc : packet["checked_locations"]) {
          checked_locations_.insert(get_as_id(loc));
        }
      }

      if (packet.contains("missing_locations") &&
          packet["missing_locations"].is_array()) {
        for (auto &loc : packet["missing_locations"]) {
          missing_locations_.insert(get_as_id(loc));
        }
      }

      if (packet.contains("slot_data")) {
        slot_data_ = packet["slot_data"];
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

      if (metadata_) {
        std::vector<std::string> games_to_request;
        bool all_cached = true;
        for (auto &[game, checksum] : metadata_->datapackage_checksums) {
          nlohmann::json cached =
              DataPackageCache::LoadGameData(game, checksum);
          if (!cached.is_null()) {
            // Load from cache
            if (cached.contains("item_name_to_id")) {
              for (auto &[item_name, item_id] :
                   cached["item_name_to_id"].items())
                metadata_->item_names[game][item_id.get<int64_t>()] = item_name;
            }
            if (cached.contains("location_name_to_id")) {
              for (auto &[loc_name, loc_id] :
                   cached["location_name_to_id"].items()) {
                int64_t lid = loc_id.get<int64_t>();
                metadata_->location_names[game][lid] = loc_name;
                metadata_->location_name_to_id[game][loc_name] = lid;
              }
            }
            if (cached.contains("entrance_name_to_id")) {
              for (auto &[ent_name, ent_id] :
                   cached["entrance_name_to_id"].items())
                metadata_->entrance_names[game][ent_id.get<int64_t>()] =
                    ent_name;
            }
          } else {
            games_to_request.push_back(game);
            all_cached = false;
          }
        }

        if (!all_cached) {
          SendGetDataPackage(games_to_request);
        } else {
          metadata_->data_package_received = true;
          ResolvePendingItems();
          manager_->ReResolveHistory();
        }
      } else {
        SendGetDataPackage();
      }
      SendSync();
      SendGetHints();

      RichMessage rm;
      rm.timestamp = msg_time;
      rm.populate_local_time();
      std::string connected_text =
          name_ + " connected (Slot " + std::to_string(local_slot_) + ")";
      rm.parts.push_back({connected_text, 0xFF00FF00});
      size_t h = std::hash<std::string>{}(connected_text);
      manager_->OnGlobalMessage(this, rm, false, h);

    } else if (cmd == "DataPackage" && metadata_) {
      auto &dp = packet["data"];
      if (dp.contains("games")) {
        for (auto &[game_name, game_data] : dp["games"].items()) {
          // Save to cache if we have a checksum
          if (metadata_->datapackage_checksums.count(game_name)) {
            DataPackageCache::SaveGameData(
                game_name, metadata_->datapackage_checksums[game_name],
                game_data);
          }

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
        connection_error_time_ = ArchipelagoNetwork::GetCurrentTimestamp();
      RichMessage rm;
      rm.timestamp = msg_time;
      rm.populate_local_time();
      rm.parts.push_back(
          {"Connection refused for " + name_ + ": " + packet.dump(),
           0xFF0000FF});
      manager_->OnGlobalMessage(this, rm, false);
    } else if (cmd == "PrintJSON") {
      size_t msg_hash = 0;
      bool always_show = false;
      if (packet.contains("type")) {
        std::string t = packet["type"];
        if (t == "CommandResult" || t == "Tutorial" ||
            t == "AdminCommandResult") {
          always_show = true;
        }
      }

      if (!always_show && packet.contains("data")) {
        msg_hash = std::hash<std::string>{}(packet["data"].dump());
      }

      RichMessage rm;
      rm.timestamp =
          packet.contains("time") ? packet["time"].get<double>() : msg_time;
      rm.populate_local_time();
      rm.source_slot = name_;
      if (packet.contains("type") && packet["type"].is_string()) {
        rm.type = packet["type"].get<std::string>();
      }
      bool is_item_event = false;
      bool is_status_msg = false;
      if (packet.contains("type")) {
        std::string type_str = packet["type"];
        is_item_event = (type_str == "ItemSend" || type_str == "ItemCheat" ||
                         type_str == "Hint");
        is_status_msg =
            (type_str == "Join" || type_str == "Part" ||
             type_str == "TagsChanged" || type_str == "CommandResult" ||
             type_str == "Tutorial" || type_str == "AdminCommandResult");
      }

      int extracted_hint_status = -1;
      for (auto &part : packet["data"]) {
        std::string type =
            part.contains("type") ? part["type"].get<std::string>() : "text";
        std::string content =
            part.contains("text") ? part["text"].get<std::string>() : "";
        uint32_t color =
            (is_status_msg && !is_item_event) ? 0xFFAAAAAA : 0xFFFFFFFF;

        if (type == "hint_status") {
          if (part.contains("hint_status")) {
            extracted_hint_status = part["hint_status"].get<int>();
          }
          color = 0xFF00FFFF; // Cyan/Yellow for status
        }

        std::string class_name = type;

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
            if (pid == local_slot_)
              class_name += " player_self";
            rm.parts.push_back(
                MessagePart{content, color, global_id, class_name});
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
            if (flags & 0x01) {
              color = 0xFFFF5FAF;
              class_name += " item_progression";
            } else if (flags & 0x02) {
              color = 0xFFED9564;
              class_name += " item_useful";
            } else if (flags & 0x04) {
              color = 0xFF0045FF;
              class_name += " item_trap";
            } else {
              color = 0xFFFFFF00;
              class_name += " item_filler";
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
            color = 0xFF00FF00;
          } catch (...) {
          }
        }
        rm.parts.push_back(MessagePart{content, color, -1, class_name});
      }

      if (packet.contains("slot")) {
        int s_slot = (int)get_as_id(packet["slot"]);
        if (s_slot != -1)
          rm.sender_slot = (team_ << 16) | s_slot;
      }

      if (is_item_event) {
        if (packet.contains("type")) {
          std::string type = packet["type"];
          if (type == "ItemSend") {
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

            // Live Activity Update (Location-based only: ItemSend)
            if (rm.sender_slot != -1) {
              manager_->UpdateSlotActivity(rm.sender_slot, ArchipelagoNetwork::GetCurrentTimestamp());
            }

            if (packet.contains("item") &&
                packet["item"].contains("location")) {
              int64_t loc_id = get_as_id(packet["item"]["location"]);
              rm.location_id = loc_id; // Store in RichMessage
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
              h.status =
                  packet["item"].contains("status")
                      ? packet["item"]["status"].get<int>()
                      : (extracted_hint_status != -1 ? extracted_hint_status
                                                     : 0);
              if (h.found && h.status < 40)
                h.status = 40;
              h.source_slot = name_;
              UpdateOrAddHint(h);

              rm.receiver_slot = h.receiver_slot;
              rm.sender_slot = h.finder_slot;
            } catch (...) {
            }
          }
        }
        manager_->OnGlobalMessage(this, rm, true, msg_hash, always_show);
      } else {
        manager_->OnGlobalMessage(this, rm, false, msg_hash, always_show);
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
                h.status =
                    h_val.contains("status") ? h_val["status"].get<int>() : 0;
                if (h.found && h.status < 40)
                  h.status = 40;
                h.source_slot = name_;
                UpdateOrAddHint(h);
              } catch (...) {
              }
            }
          }
        }
      }
    } else if (cmd == "RoomUpdate") {
      if (packet.contains("hint_points")) {
        hint_points_ = packet["hint_points"].get<int>();
      }
      if (packet.contains("hint_cost")) {
        hint_cost_ = packet["hint_cost"].get<int>();
      }
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
          missing_locations_.erase(get_as_id(loc));
        }
        manager_->SetItemsDirty();
      }
    } else if (cmd == "Bounced") {
      if (packet.contains("tags") && packet["tags"].is_array()) {
        bool is_deathlink = false;
        for (const auto &tag : packet["tags"]) {
          if (tag == "DeathLink") {
            is_deathlink = true;
            break;
          }
        }
        if (is_deathlink && packet.contains("data")) {
          if (manager_->GetSettings() &&
              !manager_->GetSettings()->show_deathlink_messages) {
            continue; // Skip if disabled
          }
          auto &data = packet["data"];
          std::string source =
              data.contains("source") ? data["source"].get<std::string>() : "";
          std::string cause =
              data.contains("cause") ? data["cause"].get<std::string>() : "";

          RichMessage rm;
          rm.timestamp = msg_time;
          rm.populate_local_time();
          rm.type = "DeathLink";
          rm.source_slot = name_;
          rm.parts.push_back(
              MessagePart{"[DeathLink] ", 0xFF0000FF, -1, "deathlink-header"});
          if (!source.empty()) {
            rm.parts.push_back(MessagePart{"(" + source + ") ", 0xFFCCCCCC, -1,
                                           "deathlink-source"});
          }
          if (!cause.empty()) {
            rm.parts.push_back(
                MessagePart{cause, 0xFFAAAAAA, -1, "deathlink-cause"});
          } else {
            rm.parts.push_back(
                MessagePart{" died", 0xFFAAAAAA, -1, "deathlink-cause"});
          }

          // Use a hash of the data to prevent duplicates if multiple sessions
          // receive the same bounce
          size_t msg_hash = std::hash<std::string>{}(data.dump() + rm.type);
          manager_->OnGlobalMessage(this, rm, true, msg_hash);
        }
      }
    }
  }
  return changed;
}

void ArchipelagoSession::SendConnect() {
  json packet = json::array();
  packet.push_back(
      {{"cmd", "Connect"},
       {"password", password_},
       {"game", ""},
       {"name", name_},
       {"uuid", manager_->GetSettings() ? manager_->GetSettings()->uuid : ""},
       {"version",
        {{"class", "Version"}, {"major", 0}, {"minor", 5}, {"build", 1}}},
       {"items_handling", 7},
       {"tags", {"Tracker", "Axolotl", "DeathLink"}}});
  SendPacket(packet);
}

void ArchipelagoSession::SendChat(const std::string &message) {
  json packet = json::array();
  packet.push_back({{"cmd", "Say"}, {"text", message}});
  SendPacket(packet);
}

void ArchipelagoSession::SendDeathLink(const std::string &cause) {
  json packet = json::array();
  json data = {{"cmd", "DeathLink"},
               {"source", name_},
               {"timestamp", ArchipelagoNetwork::GetCurrentTimestamp()},
               {"cause", cause}};
  packet.push_back(
      {{"cmd", "Bounce"}, {"tags", {"DeathLink"}}, {"data", data}});
  SendPacket(packet);
}

void ArchipelagoSession::SendGetDataPackage(
    const std::vector<std::string> &games) {
  json packet = json::array();
  json cmd = {{"cmd", "GetDataPackage"}};
  if (!games.empty()) {
    cmd["games"] = games;
  }
  packet.push_back(cmd);
  SendPacket(packet);
}

void ArchipelagoSession::SendSync() {
  json packet = json::array();
  packet.push_back({{"cmd", "Sync"}});
  SendPacket(packet);
}

void ArchipelagoSession::SendGetHints() {
  json packet = json::array();
  packet.push_back({{"cmd", "Get"},
                    {"keys",
                     {"_read_hints_" + std::to_string(team_) + "_" +
                      std::to_string(local_slot_)}}});
  SendPacket(packet);
}

void ArchipelagoSession::SendUpdateHint(int64_t location_id, int finder_slot,
                                        int status) {
  json packet = json::array();
  packet.push_back({{"cmd", "UpdateHint"},
                    {"player", finder_slot},
                    {"location", location_id},
                    {"status", status}});
  SendPacket(packet);

  // Update local view across all sessions as the server doesn't echo this back
  manager_->OnLocalHintStatusUpdated(location_id, finder_slot, status);
}

void ArchipelagoSession::SendPacket(const json &packet) {
  std::string dump = packet.dump();
  if (manager_->IsDebugMode())
    fprintf(stderr, "[SEND][%s] %s\n", name_.c_str(), dump.c_str());
  std::lock_guard<std::mutex> lock(outgoing_mutex_);
  outgoing_queue_.push_back(dump);
}

void ArchipelagoSession::ProcessNetworkCommands() {
  if (pending_stop_) {
    webSocket_.stop();
    pending_stop_ = false;
  }
  if (!pending_url_.empty()) {
    webSocket_.setUrl(pending_url_);
    pending_url_.clear();
  }
  if (pending_start_) {
    webSocket_.start();
    pending_start_ = false;
  }

  std::deque<std::string> to_send;
  {
    std::lock_guard<std::mutex> lock(outgoing_mutex_);
    std::swap(to_send, outgoing_queue_);
  }
  for (const auto &msg : to_send) {
    webSocket_.send(msg);
  }
}

void ArchipelagoSession::UpdateHintStatus(int64_t location_id, int finder_slot,
                                          int status) {
  for (auto &h : hints_) {
    if (h.location_id == location_id && h.finder_slot == finder_slot) {
      h.status = status;
    }
  }
}

void ArchipelagoSession::UpdateOrAddHint(const Hint &hint) {
  for (auto &h : hints_) {
    if (h.location_id == hint.location_id &&
        h.finder_slot == hint.finder_slot &&
        h.receiver_slot == hint.receiver_slot) {
      h.found = hint.found;
      h.status = hint.status;
      h.item_flags = hint.item_flags;
      h.item_id = hint.item_id;
      h.entrance_name = hint.entrance_name;
      manager_->SetHintsDirty();
      return;
    }
  }
  hints_.push_back(hint);
  manager_->SetHintsDirty();
}

void ArchipelagoSession::ClearData() {
  received_items_history_.clear();
  hints_.clear();
  checked_locations_.clear();
  missing_locations_.clear();

  if (manager_ && local_slot_ != -1) {
    manager_->ClearSessionStats((team_ << 16) | local_slot_);
  }

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    std::queue<QueuedPacket> empty;
    std::swap(message_queue_, empty);
  }
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    std::queue<QueuedStatus> empty;
    std::swap(status_messages_, empty);
  }
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

std::string ArchipelagoSession::ResolvePlayerGame(int slot) const {
  if (!metadata_)
    return "";
  int target_slot = (slot != -1) ? slot : ((team_ << 16) | local_slot_);
  if (metadata_->slot_to_game.count(target_slot))
    return metadata_->slot_to_game.at(target_slot);
  if (slot != -1 && metadata_->slot_to_game.count(slot))
    return metadata_->slot_to_game.at(slot);
  return "";
}

std::string ArchipelagoSession::ResolvePlayerName(int slot) const {
  if (!metadata_)
    return "Unknown Player " + std::to_string(slot);
  int target_slot = (slot != -1) ? slot : ((team_ << 16) | local_slot_);
  if (metadata_->player_names.count(target_slot))
    return metadata_->player_names.at(target_slot);
  if (slot != -1 && metadata_->player_names.count(slot))
    return metadata_->player_names.at(slot);
  return "Unknown Player " + std::to_string(slot);
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

ArchipelagoNetwork::ArchipelagoNetwork() {
  global_stats_ = std::make_shared<MultiworldStats>();
}


void ArchipelagoNetwork::StartNetworkThread() {
  if (network_thread_running_)
    return;
  network_thread_running_ = true;
  network_thread_ = std::thread([this]() {
    while (network_thread_running_) {
      Update();
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });
}

void ArchipelagoNetwork::StopNetworkThread() {
  if (!network_thread_running_)
    return;
  network_thread_running_ = false;
  if (network_thread_.joinable()) {
    network_thread_.join();
  }
}

std::shared_ptr<const MultiworldStats> ArchipelagoNetwork::GetGlobalStats() const {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  return global_stats_;
}

std::string ArchipelagoNetwork::MaskURL(const std::string &url) {
  size_t protocol_pos = url.find("://");
  if (protocol_pos != std::string::npos) {
    std::string protocol = url.substr(0, protocol_pos + 3);
    return protocol + std::string(url.length() - protocol.length(), '*');
  }
  return std::string(url.length(), '*');
}
ArchipelagoNetwork::~ArchipelagoNetwork() {
  StopNetworkThread();
  sessions_.clear(); // This will destroy sessions and stop their websockets
}

ArchipelagoSession *ArchipelagoNetwork::AddSession(const std::string &name) {
  ArchipelagoSession *session = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    if (GetSession(name))
      return GetSession(name);
    sessions_.push_back(std::make_unique<ArchipelagoSession>(this, name));
    slots_dirty_ = true;
    session = sessions_.back().get();
  }
  if (session)
    session->ProcessNetworkCommands();
  return session;
}

void ArchipelagoNetwork::RemoveSession(const std::string &name) {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
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
  {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    for (auto &session : sessions_) {
      session->Disconnect();
    }
    ClearGlobalStats();
  }
  for (auto &session : sessions_) {
    session->ProcessNetworkCommands();
  }
}

ArchipelagoSession *
ArchipelagoNetwork::GetSessionByGlobalSlot(int global_slot) {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  for (auto &session : sessions_) {
    if (session->IsConnected() &&
        (session->GetTeam() << 16 | session->GetLocalSlot()) == global_slot)
      return session.get();
  }
  return nullptr;
}

ArchipelagoSession *ArchipelagoNetwork::GetSession(const std::string &name) {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  for (auto &s : sessions_)
    if (s->GetName() == name)
      return s.get();
  return nullptr;
}

void ArchipelagoNetwork::OnLocalHintStatusUpdated(int64_t location_id,
                                                  int finder_slot, int status) {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  for (auto &session : sessions_) {
    session->UpdateHintStatus(location_id, finder_slot, status);
  }
  SetHintsDirty();
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
  {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    for (auto &session : sessions_) {
      bool was_connected = session->IsConnected();
      if (session->Update())
        changed = true;
      if (was_connected != session->IsConnected()) {
        slots_dirty_ = true;
        aggregated_items_dirty_ = true;
        aggregated_hints_dirty_ = true;
        if (sync_state_ == TrackerSyncState::Completed) {
          if (debug_mode_) {
            std::cout << "[Overview] Session state change detected. Restarting "
                         "sync cycle."
                      << std::endl;
          }
          ForceTrackerSync();
        }
        changed = true;
      }
    }
    UpdateTrackerStats();
  }

  for (auto &session : sessions_) {
    session->ProcessNetworkCommands();
  }

  return changed;
}

void ArchipelagoNetwork::ClearGlobalStats() {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  global_stats_ = std::make_shared<MultiworldStats>();
  sync_state_ = TrackerSyncState::Idle;
  initial_tracker_sync_time_ = 0.0;
  last_tracker_sync_time_ = -1.0;
  last_successful_sync_activity_time_ = -1.0;
  if (on_stats_updated)
    on_stats_updated(*global_stats_);
}

void ArchipelagoNetwork::ClearSessionStats(int global_slot) {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  if (global_stats_->slot_info.count(global_slot)) {
    // Create new stats for thread safety (Copy-on-Write)
    auto new_stats = std::make_shared<MultiworldStats>(*global_stats_);
    new_stats->slot_info.erase(global_slot);
    new_stats->completed_slots.erase(global_slot);

    // Re-calculate global totals
    int total_checked = 0;
    for (auto const &[id, stats] : new_stats->slot_info) {
      total_checked += (int)stats.checked_location_ids.size();
    }
    new_stats->checked_locations = total_checked;
    last_tracker_checked_count_ = total_checked;
    global_stats_ = new_stats;

    if (on_stats_updated)
      on_stats_updated(*global_stats_);
  }
}

bool ArchipelagoNetwork::IsAnySessionActive() const {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  for (const auto &s : sessions_) {
    if (s->GetState() != State::Disconnected)
      return true;
  }
  return false;
}

const std::set<int> &ArchipelagoNetwork::GetConnectedSlots() {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
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
                                         bool is_item_feed, size_t message_hash,
                                         bool always_show) {
  // Identify status messages for specialized handling (colors, goal detection)
  bool is_status =
      always_show ||
      (!is_item_feed && !msg.parts.empty() &&
       (msg.parts[0].text.find("[System]") != std::string::npos ||
        msg.parts[0].text.find(" connected (Slot ") != std::string::npos ||
        msg.parts[0].text.find("Connection refused") != std::string::npos));

  size_t hash = message_hash;
  if (hash == 0 && !msg.parts.empty()) {
    std::string full_text;
    for (const auto &p : msg.parts)
      full_text += p.text;
    hash = std::hash<std::string>{}(full_text);
  }

  if (hash != 0) {
    if (message_hash_history_.count(hash)) {
      auto &entry = message_hash_history_[hash];
      if (session && entry.first_session_name != session->GetName()) {
        return; // Duplicate from another session
      }
      entry.last_time = msg.timestamp;
      // Refresh in queue
      auto it = std::find(message_hash_queue_.begin(),
                          message_hash_queue_.end(), hash);
      if (it != message_hash_queue_.end()) {
        message_hash_queue_.erase(it);
        message_hash_queue_.push_back(hash);
      }
    } else {
      // New hash
      auto &entry = message_hash_history_[hash];
      entry.first_session_name = session ? session->GetName() : "[System]";
      entry.last_time = msg.timestamp;
      message_hash_queue_.push_back(hash);
      if (message_hash_queue_.size() > 1000) {
        size_t old_hash = message_hash_queue_.front();
        message_hash_queue_.pop_front();
        message_hash_history_.erase(old_hash);
      }
    }
  }

  std::lock_guard<std::recursive_mutex> lock(state_mutex_);

  if (is_item_feed) {
    SetItemsDirty(); // Replaced aggregated_items_dirty_ = true;
    CheckDayChange(item_history_, msg.timestamp, last_item_day_);
    item_history_.push_back(msg);
    last_item_activity_time_ = GetCurrentTimestamp();

    if (msg.type == "ItemSend") {
      int sender = msg.sender_slot;
      if (sender != -1) {
        // Create new stats for thread safety (Copy-on-Write)
        auto new_stats = std::make_shared<MultiworldStats>(*global_stats_);
        auto &slot_stats = new_stats->slot_info[sender];
        slot_stats.checked_location_ids.insert(msg.location_id);
        slot_stats.checked_locations =
            (int)slot_stats.checked_location_ids.size();

        // Re-calculate global count
        int total_checked = 0;
        for (auto const &[id, stats] : new_stats->slot_info) {
          total_checked += (int)stats.checked_location_ids.size();
        }
        new_stats->checked_locations = total_checked;
        global_stats_ = new_stats;

        if (on_stats_updated)
          on_stats_updated(*global_stats_);
      }
    }
  } else {
    CheckDayChange(chat_history_, msg.timestamp, last_chat_day_);
    if (!chat_history_.empty() &&
        msg.timestamp < chat_history_.back().timestamp) {
      chat_history_.push_back(msg);
      std::stable_sort(chat_history_.begin(), chat_history_.end(),
                       [](const auto &a, const auto &b) {
                         return a.timestamp < b.timestamp;
                       });
    } else {
      chat_history_.push_back(msg);
    }
    if (!is_status) {
      std::string full_text;
      for (const auto &p : msg.parts)
        full_text += p.text;
      if (msg.type == "Goal" ||
          full_text.find(" completed their goal") != std::string::npos) {
        // Create new stats for thread safety (Copy-on-Write)
        auto new_stats = std::make_shared<MultiworldStats>(*global_stats_);
        bool changed = false;
        if (msg.sender_slot != -1) {
          new_stats->completed_slots.insert(msg.sender_slot);
          changed = true;
        } else {
          for (const auto &p : msg.parts) {
            if (p.player_id != -1) {
              new_stats->completed_slots.insert(p.player_id);
              changed = true;
              break;
            }
          }
        }
        if (changed) {
          global_stats_ = new_stats;
          if (on_stats_updated)
            on_stats_updated(*global_stats_);
        }
      }
    }
  }

  if (on_message_received) {
    on_message_received(msg);
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

// Global Resolution Helpers removed in favor of Session-specific resolution

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

void ArchipelagoNetwork::ClearAllData(bool keep_chat) {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  if (!keep_chat) {
    ClearChatHistory();
  }
  ClearItemHistory();

  for (auto &session : sessions_) {
    session->ClearData();
  }

  aggregated_items_cache_.clear();
  aggregated_hints_cache_.clear();
  aggregated_items_dirty_ = true;
  aggregated_hints_dirty_ = true;
  data_version_++;

  if (on_history_updated)
    on_history_updated();
}

void ArchipelagoNetwork::OnStatusMessage(ArchipelagoSession *session,
                                         const std::string &msg) {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
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
  if (session) {
    p.text = "[System][" + session->GetName() + "] " + final_msg;
  } else {
    p.text = "[System] " + final_msg;
  }
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
    auto session = GetSession(rm.source_slot);
    if (!session)
      continue;

    for (auto &part : rm.parts) {
      if (part.text.find("Unknown Item ") == 0) {
        try {
          int64_t id = std::stoll(part.text.substr(13));
          std::string name = session->ResolveItemName(id, rm.receiver_slot);
          if (!name.empty())
            part.text = name;
        } catch (...) {
        }
      }
      if (part.text.find("Unknown Location ") == 0) {
        try {
          int64_t id = std::stoll(part.text.substr(17));
          std::string name = session->ResolveLocationName(id, rm.sender_slot);
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
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
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
void ArchipelagoNetwork::SyncTotalLocations() {
  if (!settings_ || settings_->tracker_url.empty())
    return;

  std::string api_url = settings_->tracker_url;
  size_t pos = api_url.find("/tracker/");
  if (pos != std::string::npos) {
    api_url.replace(pos, 9, "/api/static_tracker/");
  } else {
    pos = api_url.find("/api/tracker/");
    if (pos != std::string::npos) {
      api_url.replace(pos, 13, "/api/static_tracker/");
    } else {
      return;
    }
  }

  if (debug_mode_) {
    std::cout << "[Overview] Syncing with static tracker API: " << api_url
              << std::endl;
  }

  auto args = std::make_shared<ix::HttpRequestArgs>();
  args->followRedirects = true;
  args->compress = true;
  args->extraHeaders["User-Agent"] =
      "Axolotl/" AXOLOTL_VERSION_STRING " (" GIT_HASH ")";

  // Use a temporary thread to avoid blocking the network thread or UI
  std::thread([this, api_url, args]() {
    ix::HttpClient httpClient;
    auto response = httpClient.get(api_url, args);

    if (response && response->statusCode == 200) {
      try {
        auto j = nlohmann::json::parse(response->body);
        int total_l = 0;
        if (j.contains("player_locations_total") &&
            j["player_locations_total"].is_array()) {

          std::lock_guard<std::recursive_mutex> lock(state_mutex_);
          for (const auto &p : j["player_locations_total"]) {
            if (p.contains("total_locations") && p.contains("player")) {
              int slot = p["player"].get<int>();
              int team = p.contains("team") ? p["team"].get<int>() : 0;
              int packed_slot = (team << 16) | slot;

              int player_total = p["total_locations"].get<int>();
              total_l += player_total;
              global_stats_->slot_info[packed_slot].total_locations =
                  player_total;
            }
          }

          global_stats_->total_locations = total_l;

          if (on_stats_updated) {
            on_stats_updated(*global_stats_);
          }

          if (debug_mode_) {
            std::cout << "[Overview] Static Tracker Stats: Total Locations "
                      << total_l << std::endl;
          }
        }
      } catch (...) {
      }
    }
  }).detach();
}

void ArchipelagoNetwork::UpdateTrackerStats() {
  if (!settings_ || settings_->tracker_url.empty() ||
      !IsAnySessionConnected() || !tracker_sync_active_)
    return;

  {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    if (settings_->tracker_url != last_synced_tracker_url_) {
      ClearGlobalStats();
      last_synced_tracker_url_ = settings_->tracker_url;
    }
  }

  if (sync_state_ == TrackerSyncState::Completed)
    return;

  double now = GetCurrentTimestamp();

  bool trigger_poll = false;
  {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    // If we haven't started syncing yet, start now
    if (sync_state_ == TrackerSyncState::Idle) {
      SyncTotalLocations();
      force_tracker_sync_ = true; // Trigger the poll
      sync_state_ = TrackerSyncState::FirstPollInProgress;
    } else if (sync_state_ == TrackerSyncState::WaitingForSecondPoll) {
      // If 2 minutes have passed since the first sync, trigger the second poll
      if (now - initial_tracker_sync_time_ >= 120.0) {
        force_tracker_sync_ = true;
        sync_state_ = TrackerSyncState::SecondPollInProgress;
      }
    }

    if (force_tracker_sync_) {
      trigger_poll = true;
      force_tracker_sync_ = false;
    }
  }

  if (!trigger_poll)
    return;

  std::string api_url = settings_->tracker_url;
  size_t pos = api_url.find("/tracker/");
  if (pos != std::string::npos) {
    api_url.replace(pos, 9, "/api/tracker/");
  } else {
    // Check if it's already an API URL
    if (api_url.find("/api/tracker/") == std::string::npos)
      return; // Invalid URL
  }

  last_tracker_sync_time_ = now;

  if (debug_mode_) {
    std::cout << "[Overview] Syncing with tracker API: " << api_url
              << std::endl;
  }

  auto args = std::make_shared<ix::HttpRequestArgs>();
  args->followRedirects = true;
  args->compress = true; // Enables Accept-Encoding: gzip
  args->extraHeaders["User-Agent"] =
      "Axolotl/" AXOLOTL_VERSION_STRING " (" GIT_HASH ")";

  std::thread([this, api_url, args, now]() {
    ix::HttpClient httpClient;
    auto response = httpClient.get(api_url, args);
    if (response) {
      if (debug_mode_) {
        std::cout << "[Overview] API Response: " << response->statusCode << " ("
                  << response->description << ")" << std::endl;
      }
      if (response->statusCode == 200) {
        try {
          auto j = nlohmann::json::parse(response->body);
          int total_g = 0;
          int completed_g = 0;
          int checked_l = 0;

          std::lock_guard<std::recursive_mutex> lock(state_mutex_);
          auto new_stats = std::make_shared<MultiworldStats>(*global_stats_);

          if (j.contains("player_status") && j["player_status"].is_array()) {
            total_g = (int)j["player_status"].size();
            for (const auto &stats : j["player_status"]) {
              if (stats.contains("player")) {
                int slot = stats["player"].get<int>();
                int team =
                    stats.contains("team") ? stats["team"].get<int>() : 0;
                int packed_slot = (team << 16) | slot;

                if (stats.contains("status") && stats["status"].is_number() &&
                    stats["status"].get<int>() == 30) {
                  completed_g++;
                  new_stats->completed_slots.insert(packed_slot);
                }
              }
            }
          }

          if (j.contains("player_checks_done") &&
              j["player_checks_done"].is_array()) {
            for (const auto &checks : j["player_checks_done"]) {
              if (checks.contains("player") && checks.contains("locations") &&
                  checks["locations"].is_array()) {
                int slot = checks["player"].get<int>();
                int team =
                    checks.contains("team") ? checks["team"].get<int>() : 0;
                int packed_slot = (team << 16) | slot;
                auto &slot_stats = new_stats->slot_info[packed_slot];
                for (const auto &loc : checks["locations"]) {
                  slot_stats.checked_location_ids.insert(get_as_id(loc));
                }
                slot_stats.checked_locations =
                    (int)slot_stats.checked_location_ids.size();
              }
            }
          }

          if (j.contains("activity_timers") &&
              j["activity_timers"].is_array()) {
            for (const auto &timer_entry : j["activity_timers"]) {
              if (timer_entry.contains("player") &&
                  timer_entry.contains("time")) {
                int slot = timer_entry["player"].get<int>();
                int team = timer_entry.contains("team")
                               ? timer_entry["team"].get<int>()
                               : 0;
                int packed_slot = (team << 16) | slot;

                std::string time_str = timer_entry["time"].get<std::string>();
                std::tm tm = {};
                std::stringstream ss(time_str);
                ss >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
                if (!ss.fail()) {
#ifdef _WIN32
                  new_stats->slot_info[packed_slot].last_activity_time =
                      (double)_mkgmtime(&tm);
#else
                  new_stats->slot_info[packed_slot].last_activity_time =
                      (double)timegm(&tm);
#endif
                }
              }
            }
          }

          if (j.contains("total_checks_done") &&
              j["total_checks_done"].is_array()) {
            for (const auto &team_stats : j["total_checks_done"]) {
              if (team_stats.contains("checks_done")) {
                checked_l += (int)team_stats["checks_done"].get<int>();
              }
            }
          }

          if (total_g > 0)
            new_stats->total_games = total_g;

          // Re-calculate global count
          int total_checked = 0;
          for (auto const &[id, stats] : new_stats->slot_info) {
            total_checked += (int)stats.checked_location_ids.size();
          }
          new_stats->checked_locations = total_checked;
          last_tracker_checked_count_ = total_checked;

          // Handle two-poll strategy state
          if (sync_state_ == TrackerSyncState::FirstPollInProgress) {
            initial_tracker_sync_time_ = now;
            sync_state_ = TrackerSyncState::WaitingForSecondPoll;
            if (debug_mode_) {
              std::cout
                  << "[Overview] First tracker sync complete. Second poll "
                     "scheduled for 2 minutes from now."
                  << std::endl;
            }
          } else if (sync_state_ == TrackerSyncState::SecondPollInProgress) {
            sync_state_ = TrackerSyncState::Completed;
            if (debug_mode_) {
              std::cout
                  << "[Overview] Second tracker sync complete. Sync finished."
                  << std::endl;
            }
          }

          global_stats_ = new_stats;

          if (on_stats_updated) {
            on_stats_updated(*global_stats_);
          }
          last_successful_sync_activity_time_ = last_item_activity_time_;

          if (debug_mode_) {
            std::cout << "[Overview] Tracker Stats Parsed: Games "
                      << completed_g << "/" << total_g << ", Checked Locations "
                      << total_checked << std::endl;
          }
        } catch (...) {
        }
      }
    }
  }).detach();
}

void ArchipelagoNetwork::ForceTrackerSync() {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  last_tracker_sync_time_ = -1.0;
  last_successful_sync_activity_time_ = -1.0;

  sync_state_ = TrackerSyncState::Idle;
  initial_tracker_sync_time_ = 0.0;

  force_tracker_sync_ = true;
}

void ArchipelagoNetwork::SetTotalGames(int count) {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  if (global_stats_->total_games == 0) {
    auto new_stats = std::make_shared<MultiworldStats>(*global_stats_);
    new_stats->total_games = count;
    global_stats_ = new_stats;
  }
}

bool ArchipelagoNetwork::IsAnySessionConnected() const {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  for (const auto &session : sessions_) {
    if (session->IsConnected())
      return true;
  }
  return false;
}

void ArchipelagoNetwork::SetTrackerSyncActive(bool active) {
  std::lock_guard<std::recursive_mutex> lock(state_mutex_);
  if (active && !tracker_sync_active_) {
    // When re-enabling, reset sync state to allow immediate poll
    ForceTrackerSync();
  }
  tracker_sync_active_ = active;
}
