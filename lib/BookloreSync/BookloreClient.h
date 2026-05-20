#pragma once
#include <string>

/**
 * A matched book entry from GET /api/v1/books/search.
 */
struct BookloreBookMatch {
  long bookId = 0;
  long bookFileId = 0;       // EPUB file ID (needed for progress update)
  float progressPercent = 0; // 0-100, from server (0 means no progress recorded)
};

/**
 * HTTP client for the Booklore REST API.
 *
 * Endpoints used:
 *   POST /api/v1/auth/login          → JWT access token
 *   GET  /api/v1/books/search        → find book by title+author
 *   POST /api/v1/books/progress      → update reading progress
 *
 * Auth: Bearer JWT in Authorization header.
 * TLS buffers: 2KB (same as KOReaderSyncClient) to fit ESP32-C3 heap.
 */
class BookloreClient {
 public:
  enum Error {
    OK = 0,
    NO_CREDENTIALS,
    NO_SERVER_URL,
    NETWORK_ERROR,
    AUTH_FAILED,
    SERVER_ERROR,
    JSON_ERROR,
    BOOK_NOT_FOUND,
    LOW_MEMORY
  };

  /**
   * Authenticate and obtain a JWT token.
   * Token is stored internally and used for subsequent calls.
   * @return OK on success
   */
  static Error authenticate();

  /**
   * Search for a book by title (and optionally author).
   * Normalises both strings to lowercase before sending.
   * @param title    EPUB title from BookMetadataCache (required)
   * @param author   EPUB author from BookMetadataCache (optional, pass empty string to skip)
   * @param outMatch Output: matched book ID, file ID, and remote progress percent
   * @return OK on success, BOOK_NOT_FOUND if no match
   */
  static Error searchBook(const std::string& title, const std::string& author, BookloreBookMatch& outMatch);

  /**
   * Update reading progress for a book.
   * @param match         Book and file IDs from searchBook()
   * @param progressPercent Progress percentage (0-100)
   * @return OK on success
   */
  static Error updateProgress(const BookloreBookMatch& match, float progressPercent);

  static const char* errorString(Error error);

  static int lastHttpCode;

};
