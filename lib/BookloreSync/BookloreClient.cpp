#include "BookloreClient.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include "BookloreCredentialStore.h"

int BookloreClient::lastHttpCode = 0;

namespace {

static std::string accessToken;
// 2KB TLS buffers — KoSync payloads are small JSON; default 16KB causes OOM on ESP32-C3.
constexpr int HTTP_BUF_SIZE = 2048;

// mbedTLS aggregate heap need during handshake (Cloudflare-style 3-cert chain).
constexpr uint32_t MIN_HEAP_FOR_TLS = 55000;

struct ResponseBuffer {
  char* data = nullptr;
  int len = 0;
  int capacity = 0;

  ~ResponseBuffer() { free(data); }

  bool ensure(int size) {
    if (size <= capacity) return true;
    char* p = static_cast<char*>(realloc(data, size));
    if (!p) return false;
    data = p;
    capacity = size;
    return true;
  }
};

esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
  auto* buf = static_cast<ResponseBuffer*>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_DATA && buf) {
    if (buf->ensure(buf->len + evt->data_len + 1)) {
      memcpy(buf->data + buf->len, evt->data, evt->data_len);
      buf->len += evt->data_len;
      buf->data[buf->len] = '\0';
    }
  }
  return ESP_OK;
}

esp_http_client_handle_t createClient(const char* url, ResponseBuffer* buf,
                                      esp_http_client_method_t method = HTTP_METHOD_GET) {
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.event_handler = httpEventHandler;
  cfg.user_data = buf;
  cfg.method = method;
  cfg.timeout_ms = 15000;
  cfg.buffer_size = HTTP_BUF_SIZE;
  cfg.buffer_size_tx = HTTP_BUF_SIZE;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) return nullptr;

  if (!accessToken.empty()) {
    std::string authHeader = "Bearer " + accessToken;
    if (esp_http_client_set_header(client, "Authorization", authHeader.c_str()) != ESP_OK) {
      esp_http_client_cleanup(client);
      return nullptr;
    }
  }

  if (esp_http_client_set_header(client, "Content-Type", "application/json") != ESP_OK ||
      esp_http_client_set_header(client, "Accept", "application/json") != ESP_OK) {
    esp_http_client_cleanup(client);
    return nullptr;
  }

  return client;
}

// Normalise a string for matching: lowercase, trim leading/trailing whitespace.
std::string normalise(const std::string& s) {
  size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  size_t end = s.find_last_not_of(" \t\r\n");
  std::string result = s.substr(start, end - start + 1);
  for (char& c : result) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  }
  return result;
}
}  // namespace

BookloreClient::Error BookloreClient::authenticate() {
  lastHttpCode = 0;
  accessToken.clear();

  if (!BOOKLORE_STORE.hasCredentials()) {
    LOG_DBG("BLS", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string baseUrl = BOOKLORE_STORE.getBaseUrl();
  if (baseUrl.empty()) {
    return NO_SERVER_URL;
  }

  const uint32_t freeHeap = ESP.getFreeHeap();
  LOG_DBG("BLS", "Authenticating to %s (heap: %u)", baseUrl.c_str(), (unsigned)freeHeap);
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("BLS", "Insufficient heap for TLS: %u bytes (need %u)", freeHeap, MIN_HEAP_FOR_TLS);
    return LOW_MEMORY;
  }

  std::string url = baseUrl + "/api/v1/auth/login";

  JsonDocument reqDoc;
  reqDoc["username"] = BOOKLORE_STORE.getUsername();
  reqDoc["password"] = BOOKLORE_STORE.getPassword();
  std::string body;
  serializeJson(reqDoc, body);

  ResponseBuffer buf;
  esp_http_client_handle_t client = createClient(url.c_str(), &buf, HTTP_METHOD_POST);
  if (!client) return NETWORK_ERROR;

  if (esp_http_client_set_post_field(client, body.c_str(), static_cast<int>(body.length())) != ESP_OK) {
    esp_http_client_cleanup(client);
    return NETWORK_ERROR;
  }

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  esp_http_client_cleanup(client);

  LOG_DBG("BLS", "Auth response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode == 401 || httpCode == 403) return AUTH_FAILED;
  if (httpCode != 200) return SERVER_ERROR;

  if (!buf.data) return JSON_ERROR;

  JsonDocument resDoc;
  if (deserializeJson(resDoc, buf.data)) {
    LOG_ERR("BLS", "Auth JSON parse failed");
    return JSON_ERROR;
  }

  const char* token = resDoc["accessToken"];
  if (!token || token[0] == '\0') {
    LOG_ERR("BLS", "No accessToken in auth response");
    return JSON_ERROR;
  }

  accessToken = token;
  LOG_DBG("BLS", "Authenticated successfully");
  return OK;
}

BookloreClient::Error BookloreClient::searchBook(const std::string& title, const std::string& author,
                                                  BookloreBookMatch& outMatch) {
  lastHttpCode = 0;
  if (accessToken.empty()) return AUTH_FAILED;

  const std::string baseUrl = BOOKLORE_STORE.getBaseUrl();
  if (baseUrl.empty()) return NO_SERVER_URL;

  // Build URL: /api/v1/books/search?title=<encoded>
  // We use a simple percent-encoding for spaces only — title/author from EPUB metadata
  // are typically ASCII or simple unicode; full URL encoding adds code size.
  std::string normTitle = normalise(title);
  std::string url = baseUrl + "/api/v1/books/search?title=" + normTitle;

  const std::string normAuthor = normalise(author);
  if (!normAuthor.empty()) {
    url += "&author=" + normAuthor;
  }

  // Replace spaces with %20 in query params
  for (size_t i = url.find('?'); i != std::string::npos; i = url.find(' ', i)) {
    if (url[i] == ' ') {
      url.replace(i, 1, "%20");
      i += 3;
    } else {
      i++;
    }
  }

  const uint32_t freeHeap = ESP.getFreeHeap();
  LOG_DBG("BLS", "Searching: %s (heap: %u)", url.c_str(), (unsigned)freeHeap);
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("BLS", "Insufficient heap for TLS: %u bytes", freeHeap);
    return LOW_MEMORY;
  }

  ResponseBuffer buf;
  esp_http_client_handle_t client = createClient(url.c_str(), &buf);
  if (!client) return NETWORK_ERROR;

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  esp_http_client_cleanup(client);

  LOG_DBG("BLS", "Search response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode == 401 || httpCode == 403) return AUTH_FAILED;
  if (httpCode != 200) return SERVER_ERROR;
  if (!buf.data) return JSON_ERROR;

  // Parse response — expecting array of book objects
  JsonDocument doc;
  if (deserializeJson(doc, buf.data)) {
    LOG_ERR("BLS", "Search JSON parse failed");
    return JSON_ERROR;
  }

  if (!doc.is<JsonArray>() || doc.as<JsonArray>().size() == 0) {
    LOG_DBG("BLS", "No books found for title: %s", normTitle.c_str());
    return BOOK_NOT_FOUND;
  }

  // Pick the first result (server returns best matches first)
  JsonObject book = doc[0].as<JsonObject>();
  outMatch.bookId = book["id"].as<long>();

  // Primary file holds the bookFileId used for progress updates.
  outMatch.bookFileId = book["primaryFile"]["id"].as<long>();

  // epubProgress.percentage is 0.0–1.0; convert to 0–100.
  outMatch.progressPercent = 0.0f;
  JsonObject epubProgress = book["epubProgress"].as<JsonObject>();
  if (!epubProgress.isNull()) {
    outMatch.progressPercent = epubProgress["percentage"].as<float>() * 100.0f;
  }

  LOG_DBG("BLS", "Found book %ld (fileId=%ld) at %.1f%%",
          outMatch.bookId, outMatch.bookFileId, outMatch.progressPercent);
  return OK;
}

BookloreClient::Error BookloreClient::updateProgress(const BookloreBookMatch& match, float progressPercent) {
  lastHttpCode = 0;
  if (accessToken.empty()) return AUTH_FAILED;

  const std::string baseUrl = BOOKLORE_STORE.getBaseUrl();
  if (baseUrl.empty()) return NO_SERVER_URL;

  std::string url = baseUrl + "/api/v1/books/progress";

  JsonDocument doc;
  doc["bookId"] = match.bookId;
  if (match.bookFileId > 0) {
    JsonObject fp = doc["fileProgress"].to<JsonObject>();
    fp["bookFileId"] = match.bookFileId;
    fp["progressPercent"] = progressPercent;
  } else {
    // Fallback: legacy epubProgress percentage field
    JsonObject ep = doc["epubProgress"].to<JsonObject>();
    ep["percentage"] = progressPercent / 100.0f;
  }

  std::string body;
  serializeJson(doc, body);

  const uint32_t freeHeap = ESP.getFreeHeap();
  LOG_DBG("BLS", "Updating progress for book %ld: %.1f%% (heap: %u)",
          match.bookId, progressPercent, (unsigned)freeHeap);
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("BLS", "Insufficient heap for TLS: %u bytes", freeHeap);
    return LOW_MEMORY;
  }

  ResponseBuffer buf;
  esp_http_client_handle_t client = createClient(url.c_str(), &buf, HTTP_METHOD_POST);
  if (!client) return NETWORK_ERROR;

  if (esp_http_client_set_post_field(client, body.c_str(), static_cast<int>(body.length())) != ESP_OK) {
    esp_http_client_cleanup(client);
    return NETWORK_ERROR;
  }

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  esp_http_client_cleanup(client);

  LOG_DBG("BLS", "Update progress response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode == 401 || httpCode == 403) return AUTH_FAILED;
  if (httpCode == 200 || httpCode == 204) return OK;
  return SERVER_ERROR;
}

const char* BookloreClient::errorString(Error error) {
  switch (error) {
    case OK:            return "Success";
    case NO_CREDENTIALS: return "No credentials configured";
    case NO_SERVER_URL:  return "No server URL configured";
    case NETWORK_ERROR:  return "Network error";
    case AUTH_FAILED:    return "Login failed";
    case SERVER_ERROR:   return "Server error";
    case JSON_ERROR:     return "Response parse error";
    case BOOK_NOT_FOUND: return "Book not found in Booklore";
    case LOW_MEMORY:     return "Not enough memory for sync";
    default:             return "Unknown error";
  }
}
