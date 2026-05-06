#include "EmbeddedWebServer.h"
#include "embedded_static.h"
#include <iostream>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace {

// Decode application/x-www-form-urlencoded text: '+' → space, '%XX' → byte.
// Malformed escapes are left untouched.
std::string UrlDecode(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c == '+') {
      out.push_back(' ');
    } else if (c == '%' && i + 2 < in.size()) {
      auto hex = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
      };
      int hi = hex(in[i + 1]);
      int lo = hex(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
      } else {
        out.push_back(c);
      }
    } else {
      out.push_back(c);
    }
  }
  return out;
}

} // namespace

EmbeddedWebServer::EmbeddedWebServer(const ConnectionSettings &settings)
    : settings_(settings) {

  if (!settings_.http_server_enabled) {
    return;
  }

  std::string host = settings_.http_server_bind_address;
  int port = settings_.http_server_port;
  int backlog = ix::SocketServer::kDefaultTcpBacklog;
  size_t maxConnections = ix::SocketServer::kDefaultMaxConnections;

  int addressFamily =
      (host.find(':') != std::string::npos) ? AF_INET6 : AF_INET;

  server_ = std::make_unique<ix::HttpServer>(port, host, backlog,
                                             maxConnections, addressFamily);

  server_->setOnConnectionCallback(
      [this](ix::HttpRequestPtr request,
             std::shared_ptr<ix::ConnectionState> connectionState) {
        return HandleRequest(request, connectionState);
      });

  server_->setOnClientMessageCallback(
      [this](std::shared_ptr<ix::ConnectionState> connectionState,
             ix::WebSocket &webSocket, const ix::WebSocketMessagePtr &msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
          bool send_graph_history = false;
          {
          std::lock_guard<std::mutex> lock(clients_mutex_);
          if (debug_mode_) {
            std::cout << "[WebServer] WebSocket opened: " << msg->openInfo.uri
                      << std::endl;
          }

          std::string uri = msg->openInfo.uri;
          std::string path = uri;
          std::string query;
          size_t q_pos = uri.find('?');
          if (q_pos != std::string::npos) {
            path = uri.substr(0, q_pos);
            query = uri.substr(q_pos + 1);
          }

          if (path == "/feed") {
            FeedClientPrefs prefs;
            // Simple query parser
            size_t start = 0;
            while (start < query.length()) {
              size_t end = query.find('&', start);
              std::string pair = query.substr(start, (end == std::string::npos)
                                                         ? std::string::npos
                                                         : end - start);
              size_t eq = pair.find('=');
              if (eq != std::string::npos) {
                std::string key = UrlDecode(pair.substr(0, eq));
                std::string val = UrlDecode(pair.substr(eq + 1));
                bool enabled = (val != "0" && val != "false");
                if (key == "items")
                  prefs.items = enabled;
                else if (key == "hints")
                  prefs.hints = enabled;
                else if (key == "chat")
                  prefs.chat = enabled;
                else if (key == "misc")
                  prefs.misc = enabled;
              }
              if (end == std::string::npos)
                break;
              start = end + 1;
            }
            feed_clients_[&webSocket] = prefs;
          } else if (path == "/overview") {
            overview_clients_.insert(&webSocket);
            if (!last_overview_payload_.empty()) {
              webSocket.sendText(last_overview_payload_);
            }
          } else if (path == "/graph") {
            graph_clients_.insert(&webSocket);
            // Defer history send until after we release clients_mutex_ — the
            // provider acquires its own (foreign) mutex and may be slow.
            send_graph_history = true;
          } else if (path == "/stats") {
            stats_clients_.insert(&webSocket);
            if (!last_stats_payload_.empty()) {
              webSocket.sendText(last_stats_payload_);
            }
          } else {
            webSocket.close(1008, "Invalid endpoint");
          }
          } // release clients_mutex_
          if (send_graph_history && graph_history_provider_) {
            std::string history = graph_history_provider_();
            if (!history.empty())
              webSocket.sendText(history);
          }
        } else if (msg->type == ix::WebSocketMessageType::Close) {
          std::lock_guard<std::mutex> lock(clients_mutex_);
          if (debug_mode_) {
            std::cout << "[WebServer] WebSocket closed." << std::endl;
          }
          feed_clients_.erase(&webSocket);
          overview_clients_.erase(&webSocket);
          graph_clients_.erase(&webSocket);
          stats_clients_.erase(&webSocket);
        } else if (msg->type == ix::WebSocketMessageType::Error) {
          if (debug_mode_) {
            std::cout << "[WebServer] WebSocket error: "
                      << msg->errorInfo.reason << std::endl;
          }
        } else if (msg->type == ix::WebSocketMessageType::Message) {
          if (debug_mode_) {
            std::cout << "[WebServer] WebSocket message received: " << msg->str
                      << std::endl;
          }
        }
      });
}

EmbeddedWebServer::~EmbeddedWebServer() { Stop(); }

EmbeddedWebServer::StartResult EmbeddedWebServer::Start() {
  StartResult result;
  result.bind_address = settings_.http_server_bind_address;
  result.port = settings_.http_server_port;
  if (!server_ || is_running_)
    return result; // attempted=false (disabled or already running)
  result.attempted = true;
  std::string addr = result.bind_address;
  if (addr.find(':') != std::string::npos)
    addr = "[" + addr + "]";
  std::cout << "Starting embedded HTTP server on " << addr << ":"
            << result.port << std::endl;
  auto res = server_->listen();
  if (!res.first) {
    std::cerr << "Failed to start HTTP server: " << res.second << std::endl;
    result.error = res.second;
    return result;
  }
  is_running_ = true;
  server_->start();
  result.success = true;
  return result;
}

void EmbeddedWebServer::Stop() {
  if (server_ && is_running_) {
    std::string addr = settings_.http_server_bind_address;
    if (addr.find(':') != std::string::npos) {
      addr = "[" + addr + "]";
    }
    std::cout << "Stopping embedded HTTP server on " << addr << ":"
              << settings_.http_server_port << std::endl;
    server_->stop();
    is_running_ = false;
  }
}

void EmbeddedWebServer::BroadcastFeedEvent(const std::string &json_payload,
                                           const std::string &category) {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  if (debug_mode_ && !feed_clients_.empty()) {
    std::cout << "[WebServer] Broadcasting feed event (" << category << ") to "
              << feed_clients_.size() << " potential clients: " << json_payload
              << std::endl;
  }
  for (auto const &[ws, prefs] : feed_clients_) {
    bool should_send = true;
    if (category == "ItemSend" || category == "ItemCheat") {
      should_send = prefs.items;
    } else if (category == "Hint") {
      should_send = prefs.hints;
    } else if (category == "Chat") {
      should_send = prefs.chat;
    } else {
      should_send = prefs.misc;
    }

    if (should_send) {
      ws->sendText(json_payload);
    }
  }
}

void EmbeddedWebServer::BroadcastOverviewEvent(
    const std::string &json_payload) {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  last_overview_payload_ = json_payload;
  if (debug_mode_ && !overview_clients_.empty()) {
    std::cout << "[WebServer] Broadcasting overview event to "
              << overview_clients_.size() << " clients: " << json_payload
              << std::endl;
  }
  for (auto *ws : overview_clients_) {
    ws->sendText(json_payload);
  }
}

ix::HttpResponsePtr EmbeddedWebServer::HandleRequest(
    ix::HttpRequestPtr request,
    std::shared_ptr<ix::ConnectionState> connectionState) {

  if (debug_mode_) {
    std::cout << "[WebServer] HTTP Request: " << request->method << " "
              << request->uri << std::endl;
  }

  std::string path = request->uri;
  size_t q_pos = path.find('?');
  if (q_pos != std::string::npos) {
    path = path.substr(0, q_pos);
  }

  if (path == "/check") {
    ix::WebSocketHttpHeaders headers;
    headers["Content-Type"] = "text/plain";
    return std::make_shared<ix::HttpResponse>(200, "OK", ix::HttpErrorCode::Ok,
                                              headers, "OK");
  }

  if (path == "/feed") {
    ix::WebSocketHttpHeaders headers;
    headers["Content-Type"] = "text/html";
    return std::make_shared<ix::HttpResponse>(
        200, "OK", ix::HttpErrorCode::Ok, headers,
        std::string((const char *)feed_html, feed_html_len));
  }

  if (path == "/feed.js") {
    ix::WebSocketHttpHeaders headers;
    headers["Content-Type"] = "application/javascript";
    return std::make_shared<ix::HttpResponse>(
        200, "OK", ix::HttpErrorCode::Ok, headers,
        std::string((const char *)feed_js, feed_js_len));
  }

  if (path == "/feed.css") {
    ix::WebSocketHttpHeaders headers;
    headers["Content-Type"] = "text/css";
    return std::make_shared<ix::HttpResponse>(
        200, "OK", ix::HttpErrorCode::Ok, headers,
        std::string((const char *)feed_css, feed_css_len));
  }

  if (path == "/overview") {
    ix::WebSocketHttpHeaders headers;
    headers["Content-Type"] = "text/html";
    return std::make_shared<ix::HttpResponse>(
        200, "OK", ix::HttpErrorCode::Ok, headers,
        std::string((const char *)overview_html, overview_html_len));
  }

  if (path == "/overview.js") {
    ix::WebSocketHttpHeaders headers;
    headers["Content-Type"] = "application/javascript";
    return std::make_shared<ix::HttpResponse>(
        200, "OK", ix::HttpErrorCode::Ok, headers,
        std::string((const char *)overview_js, overview_js_len));
  }

  if (path == "/overview.css") {
    ix::WebSocketHttpHeaders headers;
    headers["Content-Type"] = "text/css";
    return std::make_shared<ix::HttpResponse>(
        200, "OK", ix::HttpErrorCode::Ok, headers,
        std::string((const char *)overview_css, overview_css_len));
  }

  if (path == "/graph") {
    ix::WebSocketHttpHeaders headers;
    headers["Content-Type"] = "text/html";
    return std::make_shared<ix::HttpResponse>(
        200, "OK", ix::HttpErrorCode::Ok, headers,
        std::string((const char *)graph_html, graph_html_len));
  }

  if (path == "/graph.js") {
    ix::WebSocketHttpHeaders headers;
    headers["Content-Type"] = "application/javascript";
    return std::make_shared<ix::HttpResponse>(
        200, "OK", ix::HttpErrorCode::Ok, headers,
        std::string((const char *)graph_js, graph_js_len));
  }

  if (path == "/graph.css") {
    ix::WebSocketHttpHeaders headers;
    headers["Content-Type"] = "text/css";
    return std::make_shared<ix::HttpResponse>(
        200, "OK", ix::HttpErrorCode::Ok, headers,
        std::string((const char *)graph_css, graph_css_len));
  }

  if (path == "/chart.min.js") {
    ix::WebSocketHttpHeaders headers;
    headers["Content-Type"] = "application/javascript";
    return std::make_shared<ix::HttpResponse>(
        200, "OK", ix::HttpErrorCode::Ok, headers,
        std::string((const char *)chart_min_js, chart_min_js_len));
  }

  if (path == "/stats") {
    ix::WebSocketHttpHeaders headers;
    headers["Content-Type"] = "text/html";
    return std::make_shared<ix::HttpResponse>(
        200, "OK", ix::HttpErrorCode::Ok, headers,
        std::string((const char *)stats_html, stats_html_len));
  }

  if (path == "/stats.js") {
    ix::WebSocketHttpHeaders headers;
    headers["Content-Type"] = "application/javascript";
    return std::make_shared<ix::HttpResponse>(
        200, "OK", ix::HttpErrorCode::Ok, headers,
        std::string((const char *)stats_js, stats_js_len));
  }

  if (path == "/stats.css") {
    ix::WebSocketHttpHeaders headers;
    headers["Content-Type"] = "text/css";
    return std::make_shared<ix::HttpResponse>(
        200, "OK", ix::HttpErrorCode::Ok, headers,
        std::string((const char *)stats_css, stats_css_len));
  }

  ix::WebSocketHttpHeaders headers;
  headers["Content-Type"] = "text/plain";
  return std::make_shared<ix::HttpResponse>(
      404, "Not Found", ix::HttpErrorCode::Ok, headers, "Not Found");
}

void EmbeddedWebServer::BroadcastGraphEvent(
    const std::string &json_payload) {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  if (debug_mode_ && !graph_clients_.empty()) {
    std::cout << "[WebServer] Broadcasting graph event to "
              << graph_clients_.size() << " clients" << std::endl;
  }
  for (auto *ws : graph_clients_) {
    ws->sendText(json_payload);
  }
}

void EmbeddedWebServer::BroadcastStatsEvent(
    const std::string &json_payload) {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  last_stats_payload_ = json_payload;
  if (debug_mode_ && !stats_clients_.empty()) {
    std::cout << "[WebServer] Broadcasting stats event to "
              << stats_clients_.size() << " clients" << std::endl;
  }
  for (auto *ws : stats_clients_) {
    ws->sendText(json_payload);
  }
}

void EmbeddedWebServer::BroadcastGoalEvent(
    const std::string &json_payload) {
  // Discrete event — no caching; clients connecting after the fact don't
  // see prior goals (would be stale celebration anyway).
  std::lock_guard<std::mutex> lock(clients_mutex_);
  if (debug_mode_ && !stats_clients_.empty()) {
    std::cout << "[WebServer] Broadcasting goal event to "
              << stats_clients_.size() << " clients" << std::endl;
  }
  for (auto *ws : stats_clients_) {
    ws->sendText(json_payload);
  }
}
