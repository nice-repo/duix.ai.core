#include "tts.h"
#include "util.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <random>
#include <regex> // Include for std::regex_replace

#include "audio.h"
#include "config.h"
#include "clog.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;
using json = nlohmann::json;

// 将单个Hex字符转换为对应的数值
unsigned char hexCharToValue(char c) {
  c = toupper(c);
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'A' && c <= 'F')
    return 10 + (c - 'A');
  throw runtime_error("Invalid hex character");
}

// 将两个Hex字符转换为一个字节
unsigned char hexPairToByte(char high, char low) {
  return (hexCharToValue(high) << 4) | hexCharToValue(low);
}

// 从Hex字符串转换为二进制数据
vector<unsigned char> hexStringToBytes(const string &hexStr) {
  // 移除所有空白字符
  string cleanedHex;
  copy_if(hexStr.begin(), hexStr.end(), back_inserter(cleanedHex),
          [](char c) { return !isspace(c); });

  // 检查长度是否为偶数
  if (cleanedHex.length() % 2 != 0) {
    throw runtime_error("Hex string must have even length");
  }

  vector<unsigned char> bytes;
  bytes.reserve(cleanedHex.length() / 2);

  for (size_t i = 0; i < cleanedHex.length(); i += 2) {
    bytes.push_back(hexPairToByte(cleanedHex[i], cleanedHex[i + 1]));
  }

  return bytes;
}

static std::string uuid() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dis;

  uint64_t part1 = dis(gen);
  uint64_t part2 = dis(gen);

  std::ostringstream oss;
  oss << std::hex << part1 << part2;

  std::string uuidStr = oss.str();
  return uuidStr.substr(0, 32); // 截取32位作为简化的UUID
}

static std::string writeWav(const std::string &bytes, const std::string &text) {
  std::error_code ec;
  std::filesystem::create_directory("audio", ec);

  const auto &uid = uuid();

  std::string mp3 = "./audio/" + uid + ".mp3";
  std::ofstream oss(mp3.c_str(), std::ios::binary);

  auto data = hexStringToBytes(bytes);
  oss.write(reinterpret_cast<const char *>(data.data()), data.size());
  std::string wav = "./audio/" + uid + ".wav";
  mp3ToWavSystem(mp3, wav);

  oss.close();
  return wav;
}

// 用于存储响应数据的结构体
struct MemoryStruct {
  char *memory;
  size_t size;
};

// 回调函数，处理接收到的数据
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb,
                                  void *userp) {
  size_t realsize = size * nmemb;
  struct MemoryStruct *mem = (struct MemoryStruct *)userp;

  char *ptr = (char *)realloc(mem->memory, mem->size + realsize + 1);
  if (!ptr) {
    printf("not enough memory (realloc returned NULL)\n");
    return 0;
  }

  mem->memory = ptr;
  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;

  // std::cout << mem->memory << std::endl;

  // auto root = json::parse(mem->memory);
  // writeWav(root["data"]["audio"]);

  return realsize;
}

// Helper function to create a safe, absolute filename
static std::string createSafeFilename(const std::string& text) {
    std::string safe_text = text;
    // Replace spaces with underscores
    std::replace(safe_text.begin(), safe_text.end(), ' ', '_');
    // Remove any character that is not a letter, number, or underscore
    safe_text = std::regex_replace(safe_text, std::regex("[^a-zA-Z0-9_]"), "");
    // Prevent excessively long filenames
    if (safe_text.length() > 50) {
        safe_text = safe_text.substr(0, 50);
    }
    
    // --- FIX: Use the absolute path /app/audio/ ---
    return "/app/audio/" + safe_text + ".wav";
}


namespace tts {

//new...
std::string pack(const std::string &text, const std::string &voice) {
    // Using nlohmann::json for clean JSON creation.
    nlohmann::json payload;

    // 1. Set the model. "playai-tts" is Groq's current TTS model.
    payload["model"] = "playai-tts";

    // 2. Set the input text.
    payload["input"] = text;

    // 3. Set the voice. Use the provided voice or a default like "Aaliyah-PlayAI".
    payload["voice"] = !voice.empty() ? voice : "Aaliyah-PlayAI";

    // 4. Set the desired audio format.
    payload["response_format"] = "wav";

    // Convert the JSON object to a string.
    return payload.dump();
}


// Sends the TTS request to the Groq API.
std::string tts(const std::string &text, const std::string &voice) {
  Timer t("tts " + text);
  std::string wavPath = createSafeFilename(text);
	
// --- Temporarily disable caching to prevent using corrupt files. ---
// This forces a new download for every request.
/*  
  if (std::filesystem::exists(wavPath)) {
    return wavPath;
  }
  */

  auto *config = config::get();

    // --- DEBUG CODE TO PRINT LAST 4 DIGITS OF API KEY ---
	if (config && !config->apiKey.empty()) {
		if (config->apiKey.length() > 4) {
			std::string last_four = config->apiKey.substr(config->apiKey.length() - 4);
			PLOGI << "Using Groq API Key ending in: ... " << last_four;
		} else {
			PLOGI << "Groq API Key is too short to display partial key.";
		}
	} else {
		PLOGE << "Groq API Key not found in configuration!";
	}
	// --- END DEBUG CODE ---

	
  CURL *curl;
  CURLcode res;
  struct MemoryStruct chunk;

  chunk.memory = (char *)malloc(1); // 初始化为空字符串
  chunk.size = 0;

  // in main
  // curl_global_init(CURL_GLOBAL_DEFAULT);
  curl = curl_easy_init();

  struct curl_slist *headers = NULL;

  if (curl) {
	 std::string url = "https://api.groq.com/openai/v1/audio/speech";
    // 设置请求URL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    // 设置HTTP头
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string apiKey = "Authorization: Bearer " + config->apiKey;
    headers = curl_slist_append(headers, apiKey.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	 // Set request type to POST
    curl_easy_setopt(curl, CURLOPT_POST, 1L);

    // 设置POST数据
    const std::string &data = pack(text, voice);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());

    // 设置回调函数处理响应
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

    // 执行请求
    res = curl_easy_perform(curl);

    // 检查错误
		if (res != CURLE_OK) {
            PLOGE << "curl_easy_perform() failed: " << curl_easy_strerror(res);
            wavPath = ""; // Clear path on network failure
        } else {
            // Check if the response is a JSON error or audio data.
            // JSON errors will start with a '{'.
            if (chunk.size > 0 && chunk.memory[0] == '{') {
                PLOGE << "Groq API returned a JSON error: " << chunk.memory;
                wavPath = ""; // Clear path on API error
            } else {
                // Success! The response seems to be audio data.
                std::ofstream wavFile(wavPath, std::ios::binary);
                if (wavFile.is_open()) {
                    wavFile.write(chunk.memory, chunk.size);
                    wavFile.close();
                    PLOGI << "Successfully saved TTS audio to " << wavPath;
                } else {
                    PLOGE << "Failed to open file for writing: " << wavPath;
                    wavPath = ""; // Clear path on file system error
                }
            }
        }

        free(chunk.memory);
        curl_easy_cleanup(curl);
    }
    
    if (headers) {
        curl_slist_free_all(headers);
    }

    return wavPath;
}

} // namespace tts
