#pragma once
#include "Config.h"
#include <ixwebsocket/IXHttpServer.h>
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

  void Start();
  void Stop();
  void BroadcastFeedEvent(const std::string &json_payload,
                          const std::string &category);
  void BroadcastOverviewEvent(const std::string &json_payload);
  void SetDebugMode(bool debug) { debug_mode_ = debug; }

private:
  ix::HttpResponsePtr
  HandleRequest(ix::HttpRequestPtr request,
                std::shared_ptr<ix::ConnectionState> connectionState);

  ConnectionSettings settings_;
  std::unique_ptr<ix::HttpServer> server_;
  std::mutex clients_mutex_;
  std::map<ix::WebSocket *, FeedClientPrefs> feed_clients_;
  std::set<ix::WebSocket *> overview_clients_;
  std::string last_overview_payload_;
  bool is_running_ = false;
  bool debug_mode_ = false;
};
