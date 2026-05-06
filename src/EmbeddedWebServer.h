#pragma once
#include "Config.h"
#include <ixwebsocket/IXHttpServer.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

struct FeedClientPrefs {
  bool items = true;
  bool hints = true;
  bool chat = true;
  bool misc = true;
};

class EmbeddedWebServer {
public:
  EmbeddedWebServer(const ConnectionSettings &settings);
  ~EmbeddedWebServer();

  // Result of a Start() attempt. attempted=false means the server is
  // disabled in config (no message warranted); otherwise success/error
  // describe whether bind succeeded.
  struct StartResult {
    bool attempted = false;
    bool success = false;
    std::string error;
    std::string bind_address;
    int port = 0;
  };
  StartResult Start();
  void Stop();
  void BroadcastFeedEvent(const std::string &json_payload,
                          const std::string &category);
  void BroadcastOverviewEvent(const std::string &json_payload);
  void BroadcastGraphEvent(const std::string &json_payload);
  void BroadcastStatsEvent(const std::string &json_payload);
  void BroadcastGoalEvent(const std::string &json_payload);
  void SetDebugMode(bool debug) { debug_mode_ = debug; }
  void SetGraphHistoryProvider(std::function<std::string()> provider) {
    graph_history_provider_ = std::move(provider);
  }

private:
  ix::HttpResponsePtr
  HandleRequest(ix::HttpRequestPtr request,
                std::shared_ptr<ix::ConnectionState> connectionState);

  ConnectionSettings settings_;
  std::unique_ptr<ix::HttpServer> server_;
  std::mutex clients_mutex_;
  std::map<ix::WebSocket *, FeedClientPrefs> feed_clients_;
  std::set<ix::WebSocket *> overview_clients_;
  std::set<ix::WebSocket *> graph_clients_;
  std::set<ix::WebSocket *> stats_clients_;
  std::string last_overview_payload_;
  std::string last_stats_payload_;
  bool is_running_ = false;
  bool debug_mode_ = false;
  std::function<std::string()> graph_history_provider_;
};
