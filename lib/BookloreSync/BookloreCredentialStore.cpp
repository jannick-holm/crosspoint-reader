#include "BookloreCredentialStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

BookloreCredentialStore BookloreCredentialStore::instance;

namespace {
constexpr char CRED_FILE[] = "/.crosspoint/booklore_credentials.json";
}

bool BookloreCredentialStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  doc["serverUrl"] = serverUrl;
  doc["username"] = username;
  doc["password_obf"] = obfuscation::obfuscateToBase64(password);

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(CRED_FILE, json);
}

bool BookloreCredentialStore::loadFromFile() {
  if (!Storage.exists(CRED_FILE)) {
    LOG_DBG("BLS", "No credentials file found");
    return false;
  }

  String json = Storage.readFile(CRED_FILE);
  if (json.isEmpty()) {
    LOG_ERR("BLS", "Empty credentials file");
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    LOG_ERR("BLS", "JSON parse error");
    return false;
  }

  serverUrl = doc["serverUrl"] | std::string("");
  username = doc["username"] | std::string("");

  bool ok = false;
  password = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", &ok);
  if (!ok) {
    password = doc["password"] | std::string("");
  }

  LOG_DBG("BLS", "Loaded credentials for user: %s", username.c_str());
  return true;
}

void BookloreCredentialStore::setServerUrl(const std::string& url) {
  serverUrl = url;
  LOG_DBG("BLS", "Set server URL: %s", url.empty() ? "(empty)" : url.c_str());
}

void BookloreCredentialStore::setCredentials(const std::string& user, const std::string& pass) {
  username = user;
  password = pass;
  LOG_DBG("BLS", "Set credentials for user: %s", user.c_str());
}

std::string BookloreCredentialStore::getBaseUrl() const {
  std::string url = serverUrl;
  if (url.empty()) return "";

  if (url.find("://") == std::string::npos) {
    url = "http://" + url;
  }

  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }

  return url;
}

bool BookloreCredentialStore::hasCredentials() const {
  return !serverUrl.empty() && !username.empty() && !password.empty();
}

void BookloreCredentialStore::clearCredentials() {
  username.clear();
  password.clear();
  saveToFile();
  LOG_DBG("BLS", "Cleared Booklore credentials");
}
