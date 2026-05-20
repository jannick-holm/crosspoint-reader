#pragma once
#include <string>

/**
 * Singleton for storing Booklore server credentials on the SD card.
 * Passwords are XOR-obfuscated before writing (same scheme as KOReaderCredentialStore).
 * Stored at /.crosspoint/booklore_credentials.json
 */
class BookloreCredentialStore {
 private:
  static BookloreCredentialStore instance;
  std::string serverUrl;
  std::string username;
  std::string password;

  BookloreCredentialStore() = default;

 public:
  BookloreCredentialStore(const BookloreCredentialStore&) = delete;
  BookloreCredentialStore& operator=(const BookloreCredentialStore&) = delete;

  static BookloreCredentialStore& getInstance() { return instance; }

  bool saveToFile() const;
  bool loadFromFile();

  void setServerUrl(const std::string& url);
  void setCredentials(const std::string& user, const std::string& pass);

  const std::string& getServerUrl() const { return serverUrl; }
  const std::string& getUsername() const { return username; }
  const std::string& getPassword() const { return password; }

  // Returns the base URL with protocol normalisation and trailing-slash strip.
  std::string getBaseUrl() const;

  bool hasCredentials() const;
  void clearCredentials();
};

#define BOOKLORE_STORE BookloreCredentialStore::getInstance()
