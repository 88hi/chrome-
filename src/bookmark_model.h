#ifndef CHROME_PLUS_SRC_BOOKMARK_MODEL_H_
#define CHROME_PLUS_SRC_BOOKMARK_MODEL_H_

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct BookmarkNode {
  std::wstring id;
  std::wstring title;
  std::wstring url;
  bool is_folder = false;
  std::vector<BookmarkNode> children;
};

struct FaviconBitmap {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> bgra_pixels;  // 32bpp premultiplied BGRA
};

class BookmarkModel {
 public:
  static BookmarkModel& Instance();

  // Checks the Chrome profile's Bookmarks file and reloads it if changed.
  // Returns true if the tree was reloaded.
  bool EnsureLoaded(bool show_other_bookmarks);

  // Thread-safe snapshot of the top-level items (bookmark bar children,
  // optionally followed by "Other Bookmarks" folder).
  std::shared_ptr<const std::vector<BookmarkNode>> GetRootItems() const;

  // Looks up a decoded favicon in the in-memory cache. Returns nullptr if not
  // yet decoded or unavailable.
  std::shared_ptr<const FaviconBitmap> GetFavicon(
      const std::wstring& url) const;

  // Triggers an asynchronous load of Favicons from the profile SQLite DB if
  // needed, posting `notify_msg` to `notify_hwnd` when new icons are ready.
  void RequestFaviconsAsync(HWND notify_hwnd, UINT notify_msg);

  // Returns diagnostic status when root items are empty.
  std::wstring GetDiagnosticInfo() const;

 private:
  BookmarkModel() = default;
  ~BookmarkModel() = default;
  BookmarkModel(const BookmarkModel&) = delete;
  BookmarkModel& operator=(const BookmarkModel&) = delete;
};

#endif  // CHROME_PLUS_SRC_BOOKMARK_MODEL_H_
