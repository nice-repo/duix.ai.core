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
#include <filesystem> // Required for std::filesystem::exists, copy_file
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <nlohmann/json.hpp>

// New includes for the fix
#include <atomic>
#include <chrono>
#include <queue>
#include <mutex>
#include <condition_variable>

using json = nlohmann::json;

typedef websocketpp::server<websocketpp::config::asio> server;
using websocketpp::connection_hdl;
using websocketpp::lib::bind;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;

// Helper to get current time in milliseconds
long long getCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

struct WorkFLow {
    std::shared_ptr<EdgeRender> _render = nullptr;
    std::function<void(const std::string &msg)> _sendText = nullptr;
    std::atomic<bool> paused{false};
    std::atomic<bool> initialized{false};
    std::atomic<long long> last_active_ts{0}; // epoch ms

    WorkFLow() {
        _render = nullptr;
        paused.store(false);
        initialized.store(false);
        last_active_ts = getCurrentTimeMs();
    }

    ~WorkFLow() {
        stop(); // ensure cleanup
    }

    // update last active time
    void touch() {
        last_active_ts = getCurrentTimeMs();
    }

    // Start / init renderer
    int init(std::function<void(std::vector<uint8_t> &data)> imgHdl,
             std::function<void(const std::string &msg)> msgHdl,
             const std::string &role = "SiYao") {

        touch();
        // Clean up prior render if any
        if (_render) {
            // NOTE: The original EdgeRender interface might not have a stop function.
            // Resetting the smart pointer is the best we can do to signal cleanup.
            _render.reset();
        }

        _sendText = msgHdl;
        _render = std::make_shared<EdgeRender>();
        _render->setImgHdl(imgHdl);
        _render->setMsgHdl(msgHdl);
        int ret = _render->load(role);
        if (ret != 0) {
            PLOGE << "EdgeRender::load failed role=" << role;
            return ret;
        }
        _render->startRender(); // FIX: Does not return a value.
        initialized.store(true);
        PLOGI << "WorkFLow initialized for role=" << role;
        return 0; // Assume success if we reach here
    }

    // Pause without freeing renderer (keep resources alive)
    void pause() {
        touch();
        paused.store(true);
        PLOGI << "WorkFLow paused";
        // NOTE: Removed call to non-existent _render->pauseRendering()
    }

    // Resume activity
    void resume() {
        touch();
        paused.store(false);
        PLOGI << "WorkFLow resumed";
        // NOTE: Removed call to non-existent _render->resumeRendering()
    }

    bool isPaused() const { return paused.load(); }

    // Stop and free renderer and clear queues
    void stop() {
        PLOGI << "Stopping WorkFLow";
        if (_render) {
            // NOTE: Removed call to non-existent _render->stopRender()
            _render.reset();
        }
        initialized.store(false);
    }

    // Enqueue a TTS file path for lip-sync using the original promise/future method
    void enqueueTTS(const std::string &fullpath) {
        if (_render && _render->_ttsTasks.is_lock_free()) {
            std::promise<std::string> promise;
            promise.set_value(fullpath);
            std::future<std::string> fut = promise.get_future();
            _render->_ttsTasks.push(std::move(fut)); // Use std::move for future
            PLOGI << "Enqueued TTS file for EdgeRender: " << fullpath;
        } else {
             PLOGI << "Cannot enqueue TTS, render is null or queue is busy."; // FIX: PLOGW -> PLOGI
        }
    }

    // New chat method to handle audio_id
    void chat(const std::string &query) {
        if (query.empty()) return;
        PLOGI << "Processing query for TTS: " << query;

        std::thread([this, query]() {
            this->touch();

            std::string original_audio_path = tts::tts(query, "Aaliyah-PlayAI");
            if (original_audio_path.empty()) {
                PLOGE << "TTS failed for query: '" << query << "'";
                return;
            }

            // Build converted path (e.g., original.wav -> original_16k_mono.wav)
            std::string converted = original_audio_path;
            size_t pos = converted.rfind(".wav");
            if (pos != std::string::npos) converted.insert(pos, "_16k_mono");

            std::string ffmpeg_cmd = "ffmpeg -y -i \"" + original_audio_path + "\" -ar 16000 -ac 1 -c:a pcm_s16le \"" + converted + "\"";
            int ret = std::system(ffmpeg_cmd.c_str());
            if (ret != 0 || !std::filesystem::exists(converted)) {
                PLOGE << "FFmpeg conversion failed for: " << original_audio_path;
                return;
            }

            // Copy file into /app/audio so http server can serve it reliably
            std::string audio_filename = getBaseName(converted); // e.g. abc_16k_mono.wav
            std::string dest_path = "/app/audio/" + audio_filename;
            try {
                std::filesystem::copy_file(converted, dest_path, std::filesystem::copy_options::overwrite_existing);
                PLOGI << "Copied TTS file to " << dest_path;
            } catch (const std::exception &e) {
                PLOGE << "Failed to copy audio to /app/audio/: " << e.what();
                return; // Don't proceed if copy fails
            }

            // Send URL + audio_id to client for buffering
            if (_sendText) {
                std::string audio_url = "http://localhost:8080/audio/" + audio_filename;
                json response_json;
                response_json["event"] = "tts_ready";
                response_json["wav"] = audio_url;
                response_json["audio_id"] = audio_filename;
                std::string response_message = response_json.dump();
                PLOGI << "Sending audio info to client: " << response_message;
                _sendText(response_message);
            }
        }).detach();
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
        if (connections.count(hdl) > 0) {
            PLOGI << "Replacing existing WorkFLow for this handle (cleaning old)";
            auto old = connections[hdl];
            if (old) old->stop();
            connections.erase(hdl);
        }
        connections[hdl] = std::make_shared<WorkFLow>();
        PLOGI << "New connection added. Total connections: " << connections.size();
    }

    void remove(connection_hdl hdl) {
        std::lock_guard<std::mutex> lock(mutex);
        if (connections.count(hdl) > 0) {
            auto wf = connections[hdl];
            if (wf) {
                wf->stop(); // ensure renderer threads are stopped
            }
            connections.erase(hdl);
            PLOGI << "Removed connection. Remaining: " << connections.size();
        }
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex);
        return connections.size();
    }
    
    std::vector<connection_hdl> listHandles() const {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<connection_hdl> out;
        for (auto const& [hdl, val] : connections) {
            out.push_back(hdl);
        }
        return out;
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

void on_message(server *s, websocketpp::connection_hdl hdl,
                server::message_ptr msg) {
    PLOGD << "Received: " << msg->get_payload();
    try {
        auto root = json::parse(msg->get_payload());
        std::string event = root.value("event", "none");
        auto flow = connectionManager.get(hdl);

        if (!flow) {
            PLOGI << "No workflow found for connection"; // FIX: PLOGW -> PLOGI
            return;
        }

        flow->touch();

        if (event == "init") {
            std::string role = root.value("role", "SiYao");
            PLOGI << "Initializing workflow for role: " << role;
            int ret = flow->init(std::bind(onImg, s, hdl, std::placeholders::_1),
                                 std::bind(onMsg, s, hdl, std::placeholders::_1), role);

            json response;
            response["event"] = "init_result";
            response["status"] = ret;
            response["message"] = (ret == 0) ? "success" : "failed";
            s->send(hdl, response.dump(), websocketpp::frame::opcode::text);
            PLOGI << "Sent init response: " << response.dump();

        } else if (event == "query") {
            if (!flow->initialized) {
                PLOGI << "Workflow not initialized, cannot process query"; // FIX: PLOGW -> PLOGI
                json error_response;
                error_response["event"] = "error";
                error_response["message"] = "Workflow not initialized";
                s->send(hdl, error_response.dump(), websocketpp::frame::opcode::text);
                return;
            }
            std::string query = root.value("value", "");
            PLOGI << "Processing query: " << query;
            flow->chat(query);

        } else if (event == "audio_ready") {
            std::string audio_id = root.value("audio_id", "");
            PLOGI << "Client audio_ready for audio_id=" << audio_id;
            if (!audio_id.empty()) {
                std::string audio_full_path = "/app/audio/" + audio_id;
                flow->enqueueTTS(audio_full_path);
                json play_command;
                play_command["event"] = "play_audio";
                s->send(hdl, play_command.dump(), websocketpp::frame::opcode::text);
            } else {
                PLOGI << "audio_ready without audio_id"; // FIX: PLOGW -> PLOGI
            }

        } else if (event == "pause") {
            flow->pause();

        } else if (event == "resume") {
            flow->resume();

        } else if (event == "heartbeat") {
            PLOGD << "Received heartbeat";

        } else {
            PLOGI << "Unknown event: " << event; // FIX: PLOGW -> PLOGI
        }

    } catch (const std::exception& e) {
        PLOGE << "Error parsing message: " << e.what() << ". Payload: " << msg->get_payload();
    }
}

int main(int argc, char* argv[]) {
    // Note: You may need to initialize your logger here, e.g., clog::init();
    
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

    if (config->apiKey.empty() || config->lmApiKey.empty()) {
        PLOGE << "API Key is missing. Set GROQ_API_KEY/LM_API_KEY environment variables or add them to " << conf;
        return 1;
    }

    if (config->valid() == false) {
        PLOGE << "config invalid:" << conf;
        return 1;
    }

    std::string IP = getPublicIP();
    PLOGI << "PublicIP:" << IP;
    httplib::Server svr;

    std::string cmd = "mkdir -p /app/video /app/audio";
    std::system(cmd.c_str());
    svr.set_mount_point("/video", "/app/video");
    svr.set_mount_point("/audio", "/app/audio");

    std::thread httpth([&svr] { svr.listen("0.0.0.0", 8080); });
    PLOGD << "http server start at 8080";

    server ws_server;
    ws_server.set_access_channels(websocketpp::log::alevel::none);
    ws_server.set_error_channels(websocketpp::log::elevel::info);
    ws_server.init_asio();
    ws_server.set_reuse_addr(true);

    ws_server.set_open_handler([&](connection_hdl hdl) {
        connectionManager.add(hdl);
        PLOGI << "New connection, total: " << connectionManager.size();
        json metadata;
        metadata["timestamp"] = getCurrentTime();
        metadata["role"].push_back("SiYao");
        metadata["role"].push_back("DearSister");
        metadata["listen"] = 1;

        std::string metadata_str = metadata.dump();
        uint32_t metadata_length = static_cast<uint32_t>(metadata_str.size());
        
        auto message_buffer = std::make_shared<std::vector<uint8_t>>();
        uint32_t net_length = htonl(metadata_length);
        message_buffer->insert(message_buffer->end(),
                               reinterpret_cast<uint8_t*>(&net_length),
                               reinterpret_cast<uint8_t*>(&net_length) + 4);
        message_buffer->insert(message_buffer->end(), metadata_str.begin(), metadata_str.end());
        ws_server.send(hdl, message_buffer->data(), message_buffer->size(),
                       websocketpp::frame::opcode::binary);
        PLOGI << "Sent initial roles metadata";
    });

    ws_server.set_close_handler([&](connection_hdl hdl) {
        connectionManager.remove(hdl);
        PLOGI << "Connection closed, remaining: " << connectionManager.size();
    });

    ws_server.set_message_handler(bind(&on_message, &ws_server, ::_1, ::_2));
    
    // Idle session cleanup thread
    const long long IDLE_TIMEOUT_MS = 5 * 60 * 1000; // 5 minutes
    std::thread session_cleaner([&]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            long long now = getCurrentTimeMs();
            std::vector<connection_hdl> to_remove;
            auto handles = connectionManager.listHandles();
            for (auto const& hdl : handles) {
                auto wf = connectionManager.get(hdl);
                if (!wf) continue;
                if (wf->isPaused() && (now - wf->last_active_ts) > IDLE_TIMEOUT_MS) {
                    PLOGI << "Session idle for > timeout, scheduling removal.";
                    to_remove.push_back(hdl);
                }
            }
            for (auto const& hdl : to_remove) {
                connectionManager.remove(hdl);
            }
        }
    });
    session_cleaner.detach();


    ws_server.listen(6001);
    ws_server.start_accept();

    PLOGI << "WebSocket Server started on port 6001";
    ws_server.run();
    curl_global_cleanup();
    return 0;
}
