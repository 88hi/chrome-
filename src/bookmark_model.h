#ifndef CHROME_PLUS_SRC_BOOKMARK_MODEL_H_
#define CHROME_PLUS_SRC_BOOKMARK_MODEL_H_

#include <windows.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

enum class BookmarkSource {
  kLocal = 0,
  kAccount = 1,
};

struct BookmarkNode {
  std::wstring id;
  std::wstring guid;
  std::wstring title;
  std::wstring url;
  bool is_folder = false;
  bool is_virtual_root = false;
  BookmarkSource source = BookmarkSource::kLocal;
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

  // Checks the Chrome profile's Bookmarks/AccountBookmarks files and Chrome++'s
  // durable edit journal, reloading if changed (or if `force` is true).
  bool EnsureLoaded(bool show_other_bookmarks, bool force = false);

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

  // Moves or reorders a bookmark/folder. Records the edit in Chrome++'s durable
  // journal (so Chrome's in-memory BookmarkStorage can never revert it), also
  // updates Chrome's Bookmarks/AccountBookmarks file, and reloads immediately.
  bool MoveBookmark(BookmarkSource src_source,
                    const std::wstring& src_id,
                    const std::wstring& src_guid,
                    BookmarkSource dst_source,
                    const std::wstring& dst_folder_id,
                    const std::wstring& dst_folder_guid,
                    const std::wstring& anchor_id,
                    const std::wstring& anchor_guid,
                    bool insert_after,
                    bool show_other_bookmarks);

  bool UpdateBookmark(BookmarkSource source,
                      const std::wstring& node_id,
                      const std::wstring& node_guid,
                      const std::wstring& new_title,
                      const std::optional<std::wstring>& new_url,
                      bool show_other_bookmarks);

  bool AddBookmarkOrFolder(BookmarkSource dst_source,
                           const std::wstring& dst_folder_id,
                           const std::wstring& dst_folder_guid,
                           const std::wstring& anchor_id,
                           const std::wstring& anchor_guid,
                           bool insert_after,
                           bool is_folder,
                           const std::wstring& title,
                           const std::wstring& url,
                           bool show_other_bookmarks);

  bool DeleteBookmark(BookmarkSource source,
                      const std::wstring& node_id,
                      const std::wstring& node_guid,
                      bool show_other_bookmarks);

  // Builds a `chrome://bookmarks/#cp=<base64url>` URL carrying all unsynced
  // ops so patched `chrome://bookmarks` applies them to Chrome's live
  // BookmarkModel and native bookmark bar immediately.
  std::optional<std::wstring> BuildLiveSyncUrl(std::string_view base_url,
                                               std::string_view token,
                                               uint64_t* out_max_seq = nullptr);

  // Marks all pending ops with seq <= up_to_seq as synced into Chrome's live
  // BookmarkModel, remapping any temporary `cp_*` IDs to Chrome's assigned IDs.
  void MarkOpsSynced(
      uint64_t up_to_seq,
      const std::vector<std::pair<std::string, std::string>>& id_map);

  // Returns the number of unsynced operations currently queued in the journal.
  size_t GetPendingSyncOpCount() const;

  // Re-applies unsynced Chrome++ bookmark edits onto Chrome's on-disk
  // Bookmarks/AccountBookmarks files on browser exit as fallback.
  void FlushPendingOpsToChromeFiles();

  // Returns diagnostic status when root items are empty.
  std::wstring GetDiagnosticInfo() const;

 private:
  BookmarkModel() = default;
  ~BookmarkModel() = default;
  BookmarkModel(const BookmarkModel&) = delete;
  BookmarkModel& operator=(const BookmarkModel&) = delete;
};

#endif  // CHROME_PLUS_SRC_BOOKMARK_MODEL_H_
