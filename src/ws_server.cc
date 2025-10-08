#include "clog.h"
#include "config.h"
#include "fetch.h"
#include "httplib.h"
#include "lm_client.h"
#include "tts.h"
#include "util.h"
#include <edge_render.h>
#include <future>
#include <getopt.hpp>
#include <iostream>
#include <map>
#include <vector>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
using json = nlohmann::json;

typedef websocketpp::server<websocketpp::config::asio> server;
using websocketpp::connection_hdl;
using websocketpp::lib::bind;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;

// In ws_server.cc

struct WorkFLow {
  std::shared_ptr<LmClient> _lmClient = nullptr;
  std::shared_ptr<EdgeRender> _render = nullptr;
  std::function<void(std::vector<uint8_t> &data)> _sendBinary = nullptr;
  std::function<void(const std::string &msg)> _sendText = nullptr;

  WorkFLow() {
    _lmClient = nullptr;
    _render = nullptr;
  }
  int init(std::function<void(std::vector<uint8_t> &data)> imgHdl,
           std::function<void(const std::string &msg)> msgHdl,
           const std::string &role = "SiYao") {
    
    _sendText = msgHdl;
    _render = std::make_shared<EdgeRender>();
    _render->setImgHdl(imgHdl);
    _render->setMsgHdl(msgHdl);
    _render->load(role);
    _render->startRender();

    _lmClient = std::make_shared<LmClient>();

    _lmClient->onSubText = [this](const std::vector<std::string> &array) {
        for (auto &msg : array) {
            PLOGD << "TTS input: " << msg;

            if (msg.empty()) {
                continue;
            }

            auto fut = std::async(std::launch::async, tts::tts, msg, "tianxin_xiaoling");

            std::thread([this](std::future<std::string> tts_future) {
                std::string audio_filepath = tts_future.get();
                if (!audio_filepath.empty() && audio_filepath != "TTS_DONE") {
                    
                    // --- FIX STARTS HERE ---
                    // 1. Get just the filename (e.g., "Yeah.wav") from the full path (/app/audio/Yeah.wav).
                    std::string audio_filename = getBaseName(audio_filepath);

                    // 2. Construct the correct URL. The HTTP server maps the web path "/audio"
                    //    to the file system path "/app/audio".
                    std::string audio_url = "http://localhost:8080/audio/" + audio_filename;
                    // --- FIX ENDS HERE ---

                    json response_json;
                    response_json["wav"] = audio_url;
                    std::string response_message = response_json.dump();
                    
                    if (_sendText) {
                        PLOGI << "Sending audio URL back to client: " << response_message;
                        _sendText(response_message);
                    }
                }
            }, std::move(fut)).detach();

            auto fut_for_renderer = std::async(std::launch::async, tts::tts, msg, "tianxin_xiaoling");
            _render->_ttsTasks.push(fut_for_renderer);
        }
    };
    return 0;
  }
  
  void chat(const std::string &query) {
    if (_render && !query.empty()) {
        PLOGI << "Forwarding query to TTS engine: " << query;
        auto fut_for_renderer = std::async(std::launch::async, tts::tts, query, "tianxin_xiaoling");
        _render->_ttsTasks.push(fut_for_renderer);
    }
  }
};

class ConnectionManager {
private:
  std::map<connection_hdl, std::shared_ptr<WorkFLow>,
           std::owner_less<connection_hdl>>
      connections;
  mutable std::mutex mutex;

public:
  std::shared_ptr<WorkFLow> get(connection_hdl hdl) {
    std::lock_guard<std::mutex> lock(mutex);
    if (connections.count(hdl) == 0) {
      return nullptr;
    }
    return connections[hdl];
  }

  void add(connection_hdl hdl) {
    std::lock_guard<std::mutex> lock(mutex);
    connections[hdl] = std::make_shared<WorkFLow>();
  }

  void remove(connection_hdl hdl) {
    std::lock_guard<std::mutex> lock(mutex);
    connections.erase(hdl);
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex);
    return connections.size();
  }
};

ConnectionManager connectionManager;

void onImg(server *s, websocketpp::connection_hdl hdl,
           std::vector<uint8_t> &buf) {
  try {
    if (hdl.lock()) {
      s->send(hdl, buf.data(), buf.size(), websocketpp::frame::opcode::binary);
    }
  } catch (...) {
    PLOGE << "send img failed";
  }
}

void onMsg(server *s, websocketpp::connection_hdl hdl, const std::string &msg) {
  try {
    if (hdl.lock()) {
      s->send(hdl, msg, websocketpp::frame::opcode::text);
    }
  } catch (...) {
    PLOGE << "send binary failed";
  }
}

void on_message(server *s, websocketpp::connection_hdl hdl,
                server::message_ptr msg) {
  PLOGI << "Received: " << msg->get_payload();
  auto root = json::parse(msg->get_payload());
  std::string event = root.value("event", "none");
  if (event == "init") {
    std::string role = root.value("role", "SiYao");
    auto flow = connectionManager.get(hdl);
    int ret = flow->init(std::bind(onImg, s, hdl, std::placeholders::_1),
                         std::bind(onMsg, s, hdl, std::placeholders::_1), role);

    s->send(hdl, "init " + std::to_string(ret),
            websocketpp::frame::opcode::text);
  } else if (event == "query") {
    std::string query = root.value("value", "介绍一下你自己好吗");
    auto flow = connectionManager.get(hdl);
    if (flow) {
      flow->chat(query);
    }
  }
};

int main() {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  auto *config = config::get();
  std::string conf = getarg("conf/conf.json", "-c", "--conf");
  std::ifstream stream(conf);
  if (stream.is_open()) {
    auto root = json::parse(stream);
    if (root.count("groq")) {
      config->groupId = root["groq"].value("groupId", "");
      config->apiKey = root["groq"].value("apiKey", "");
    }
    if (root.count("lmUrl")) {
      config->lmUrl = root["lmUrl"];
    }
    if (root.count("lmApiKey")) {
      config->lmApiKey = root["lmApiKey"];
    }
    if (root.count("lmModel")) {
      config->lmModel = root["lmModel"];
    }
    if (root.count("lmPrompt")) {
      config->lmPrompt = root["lmPrompt"];
    }
  }

  if (config->valid() == false) {
    PLOGE << "config invalid:" << conf;
    return 0;
  }

  std::string IP = getPublicIP();
  PLOGI << "PublicIP:" << IP;
  httplib::Server svr;

  // --- FIX: Use absolute paths for directory creation and HTTP serving ---
  std::string cmd = "mkdir -p /app/video /app/audio";
  std::system(cmd.c_str());
  svr.set_mount_point("/video", "/app/video");
  svr.set_mount_point("/audio", "/app/audio");
  // --- END FIX ---

  std::thread httpth(
      [&svr] { svr.listen("0.0.0.0", 8080); });
  PLOGD << "http server start at 8080";

  server ws_server;
  ws_server.set_access_channels(websocketpp::log::alevel::none);
  ws_server.set_error_channels(websocketpp::log::elevel::info);
  ws_server.init_asio();
  ws_server.set_reuse_addr(true);

  ws_server.set_open_handler([&](connection_hdl hdl) {
    connectionManager.add(hdl);
    PLOGI << "new connection: " << connectionManager.size();
    json metadata;
    metadata["timestamp"] = getCurrentTime();
    metadata["role"].push_back("SiYao");
    metadata["role"].push_back("DearSister");
    std::string metadata_str =
        metadata.dump(-1, ' ', false, json::error_handler_t::ignore);
    uint32_t metadata_length = static_cast<uint32_t>(metadata_str.size());
    PLOGI << "send role:" << metadata_str;
    
    auto message_buffer = std::make_shared<std::vector<uint8_t>>();
    
    uint32_t net_length = htonl(metadata_length);
    message_buffer->insert(message_buffer->end(),
                           reinterpret_cast<uint8_t *>(&net_length),
                           reinterpret_cast<uint8_t *>(&net_length) + 4);
    
    message_buffer->insert(message_buffer->end(), metadata_str.begin(),
                           metadata_str.end());
    ws_server.send(hdl, message_buffer->data(), message_buffer->size(),
                   websocketpp::frame::opcode::binary);
  });

  ws_server.set_close_handler([&](connection_hdl hdl) {
    connectionManager.remove(hdl);
    PLOGI << "连接关闭，剩余连接数: " << connectionManager.size();
  });

  ws_server.set_message_handler(bind(&on_message, &ws_server, ::_1, ::_2));

  ws_server.listen(6001);
  ws_server.start_accept();

  PLOGI << "WebSocket Server started on port 6001";
  ws_server.run();
  curl_global_cleanup();
  return 0;
}
