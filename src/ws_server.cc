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

// ADD THIS HELPER FUNCTION
std::string getBaseName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos != std::string::npos) {
        return path.substr(pos + 1);
    }
    return path;
}

typedef websocketpp::server<websocketpp::config::asio> server;
using websocketpp::connection_hdl;
using websocketpp::lib::bind;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;

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
    
    _sendText = msgHdl; // ADD THIS LINE to store the message handler
    _render = std::make_shared<EdgeRender>();
    _render->setImgHdl(imgHdl);
    _render->setMsgHdl(msgHdl);
    _render->load(role);
    _render->startRender();

    _lmClient = std::make_shared<LmClient>();

// This is the final, corrected version that solves both the logic and compilation errors.
_lmClient->onSubText = [this](const std::vector<std::string> &array) {
    for (auto &msg : array) {
        PLOGD << "TTS input: " << msg;

        if (msg.empty()) {
            continue; // Skip empty messages
        }

        // =================================================================
        // PART 1: Create the TTS task and the response thread.
        // =================================================================
        
        // Create the TTS task ONCE.
        auto fut = std::async(std::launch::async, tts::tts, msg, "tianxin_xiaoling");

        // Launch a new background thread to wait for the result and send the response.
        std::thread([this](std::future<std::string> tts_future) {
            // This thread waits here until the audio file is ready.
            std::string audio_filepath = tts_future.get();

            if (!audio_filepath.empty() && audio_filepath != "TTS_DONE") {
                // 1. Construct the public URL the browser can access.
                std::string audio_url = "http://localhost:8080/audio/" + getBaseName(audio_filepath);

                // 2. Create the JSON response.
                json response_json;
                response_json["wav"] = audio_url;
                std::string response_message = response_json.dump();

                // 3. Send the JSON back to the client using the stored handler.
                if (_sendText) {
                    PLOGI << "Sending audio URL back to client: " << response_message;
                    _sendText(response_message);
                }
            }
        }, std::move(fut)).detach(); // Move the future into the thread and detach.

        
        // =================================================================
        // PART 2: Send a second task to the renderer for lip-syncing.
        // This now correctly avoids the compilation error.
        // =================================================================
        
        // Create a named variable for the renderer's future.
        auto fut_for_renderer = std::async(std::launch::async, tts::tts, msg, "tianxin_xiaoling");
        
        // Pass the named variable to the push function.
        _render->_ttsTasks.push(fut_for_renderer);
    }
};
    return 0;
  }
  void chat(const std::string &query) {
    // Check if the renderer exists and the query is not empty.
    if (_render && !query.empty()) {
        PLOGI << "Forwarding query to TTS engine: " << query;
        
        // Create an asynchronous task to call the TTS function.
        // This generates the audio file for the text.
        auto fut_for_renderer = std::async(std::launch::async, tts::tts, query, "tianxin_xiaoling");
        
        // Push the task into the renderer's queue to be processed for lip-syncing.
        _render->_ttsTasks.push(fut_for_renderer);
    }
    
    // The original LmClient call is commented out to ensure the text is spoken directly.
    // You can re-enable it if you want to get a response from a language model as well.
    /*
    if (_lmClient) {
      std::thread th(&LmClient::request, this->_lmClient, query);
      th.detach();
    }
    */
  }
};

// 连接管理器
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

typedef websocketpp::server<websocketpp::config::asio> server;
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
  } else if (event == "query") { // keyborad input
    std::string query = root.value("value", "介绍一下你自己好吗");
    auto flow = connectionManager.get(hdl);
    if (flow) {
      flow->chat(query);
    }
    // s->send(hdl, "text:" + query, websocketpp::frame::opcode::text);
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

  std::string cmd = "mkdir -p video";
  std::system(cmd.c_str());
  svr.set_mount_point("/video", "video");
  svr.set_mount_point("/audio", "audio");
  std::thread httpth(
      [&svr] { svr.listen("0.0.0.0", 8080); }); // fix later, http never exits
  PLOGD << "http server start at 8080";

  server ws_server;
  ws_server.set_access_channels(websocketpp::log::alevel::none);
  ws_server.set_error_channels(websocketpp::log::elevel::info);
  ws_server.init_asio();
  ws_server.set_reuse_addr(true);

  // ws_server.set_message_handler(bind(
  //     &on_message, &ws_server, std::placeholders::_1,
  //     std::placeholders::_2));

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

    // 创建消息缓冲区
    auto message_buffer = std::make_shared<std::vector<uint8_t>>();

    // 添加JSON长度(4字节网络字节序)
    uint32_t net_length = htonl(metadata_length);
    message_buffer->insert(message_buffer->end(),
                           reinterpret_cast<uint8_t *>(&net_length),
                           reinterpret_cast<uint8_t *>(&net_length) + 4);

    // 添加JSON元数据
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
