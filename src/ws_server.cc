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
#include <cstdlib> // for std::getenv
#include <filesystem> // Required for std::filesystem::exists
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

using json = nlohmann::json;

typedef websocketpp::server<websocketpp::config::asio> server;
using websocketpp::connection_hdl;
using websocketpp::lib::bind;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;

struct WorkFLow {
    std::shared_ptr<EdgeRender> _render = nullptr;
    std::function<void(const std::string &msg)> _sendText = nullptr;
    // --- FIX: Add a variable to hold the audio path while we wait for the client ---
    std::string pending_audio_path = "";

    WorkFLow() {
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
        return 0;
    }

    // --- FIX: This function now only sends the URL, it doesn't start the lip-sync ---
    void chat(const std::string &query) {
        if (query.empty()) {
            return;
        }
        PLOGI << "Processing query for TTS: " << query;

        std::thread([this, query]() {
            std::string original_audio_path = tts::tts(query, "Aaliyah-PlayAI");

            if (original_audio_path.empty()) {
                PLOGE << "TTS failed for query: '" << query << "'";
                return;
            }
            
            // Convert the audio for the lip-sync engine
            std::string converted_audio_path = original_audio_path;
            size_t pos = converted_audio_path.rfind(".wav");
            if (pos != std::string::npos) {
                converted_audio_path.insert(pos, "_16k_mono");
            }
            std::string ffmpeg_command = "ffmpeg -y -i " + original_audio_path + 
                                         " -ar 16000 -ac 1 -c:a pcm_s16le " + converted_audio_path;
            int ret = std::system(ffmpeg_command.c_str());

            if (ret != 0 || !std::filesystem::exists(converted_audio_path)) {
                PLOGE << "FFmpeg conversion failed for: " << original_audio_path;
                return;
            }

            // Store the path to the converted file, ready for lip-sync
            this->pending_audio_path = converted_audio_path;
            
            // Send the URL of the original audio to the client
            if (_sendText) {
                std::string original_filename = getBaseName(original_audio_path);
                std::string audio_url = "http://localhost:8080/audio/" + original_filename;
                
                json response_json;
                response_json["wav"] = audio_url;
                std::string response_message = response_json.dump();
                
                PLOGI << "Sending audio URL to client for buffering: " << response_message;
                _sendText(response_message);
            }
        }).detach();
    }

    // --- FIX: New function to start the lip-sync on command ---
    void startLipSync() {
        if (_render && !pending_audio_path.empty()) {
            PLOGI << "Client is ready. Starting lip-sync for: " << pending_audio_path;
            std::promise<std::string> promise;
            promise.set_value(pending_audio_path);
            // 1. Store the temporary future from promise.get_future() in a named variable.
            std::future<std::string> fut = promise.get_future();
            // 2. Pass the named variable (an lvalue) to the push function.
            _render->_ttsTasks.push(fut);
            pending_audio_path = ""; // Clear after use
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
        PLOGE << "send text failed";
    }
}

// --- FIX: Replace your entire on_message function with this one ---
void on_message(server *s, websocketpp::connection_hdl hdl,
                server::message_ptr msg) {
    PLOGI << "Received: " << msg->get_payload();
    try {
        auto root = json::parse(msg->get_payload());
        std::string event = root.value("event", "none");
        auto flow = connectionManager.get(hdl);

        if (!flow) return;

        if (event == "init") {
            std::string role = root.value("role", "SiYao");
            int ret = flow->init(std::bind(onImg, s, hdl, std::placeholders::_1),
                                 std::bind(onMsg, s, hdl, std::placeholders::_1), role);
            s->send(hdl, "init " + std::to_string(ret), websocketpp::frame::opcode::text);

        } else if (event == "query") {
            std::string query = root.value("value", "");
            flow->chat(query);
        
        } else if (event == "audio_ready") {
            // Client has buffered the audio. Start lip-sync and tell client to play.
            flow->startLipSync();
            json play_command;
            play_command["event"] = "play_audio";
            s->send(hdl, play_command.dump(), websocketpp::frame::opcode::text);
        }

    } catch (const std::exception& e) {
        PLOGE << "Error parsing message: " << e.what() << ". Payload: " << msg->get_payload();
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

    // Override API keys with environment variables if they exist.
    const char* groq_key_env = std::getenv("GROQ_API_KEY");
    if (groq_key_env != nullptr && std::string(groq_key_env).length() > 0) {
        config->apiKey = groq_key_env;
        PLOGI << "Loaded Groq API Key from environment variable.";
    }

    const char* lm_key_env = std::getenv("LM_API_KEY");
    if (lm_key_env != nullptr && std::string(lm_key_env).length() > 0) {
        config->lmApiKey = lm_key_env;
        PLOGI << "Loaded LM API Key from environment variable.";
    }

    // Validate the final configuration.
    if (config->apiKey.empty() || config->lmApiKey.empty()) {
        PLOGE << "API Key is missing. Set GROQ_API_KEY/LM_API_KEY environment variables or add them to " << conf;
        return 0;
    }

    if (config->valid() == false) {
        PLOGE << "config invalid:" << conf;
        return 0;
    }

    std::string IP = getPublicIP();
    PLOGI << "PublicIP:" << IP;
    httplib::Server svr;

    // Use absolute paths for directory creation and HTTP serving
    std::string cmd = "mkdir -p /app/video /app/audio";
    std::system(cmd.c_str());
    svr.set_mount_point("/video", "/app/video");
    svr.set_mount_point("/audio", "/app/audio");

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
        
        metadata["listen"] = 1;
        PLOGI << "Sending initial roles and listen signal: " << metadata.dump();
        
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
