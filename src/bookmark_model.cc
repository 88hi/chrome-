#include "bookmark_model.h"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "config.h"
#include "portable.h"
#include "sqlite3.h"
#include "utils.h"

namespace {

using Microsoft::WRL::ComPtr;

std::wstring Utf8ToWide(std::string_view utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int needed = ::MultiByteToWideChar(
      CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) {
    return {};
  }
  std::wstring out(static_cast<size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), needed);
  return out;
}

std::string WideToUtf8(std::wstring_view wide) {
  if (wide.empty()) {
    return {};
  }
  const int needed = ::WideCharToMultiByte(
      CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
      nullptr, nullptr);
  if (needed <= 0) {
    return {};
  }
  std::string out(static_cast<size_t>(needed), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        out.data(), needed, nullptr, nullptr);
  return out;
}

struct FileStamp {
  uint64_t write_time = 0;
  uint64_t file_size = 0;

  friend bool operator==(const FileStamp&, const FileStamp&) = default;
};

std::optional<FileStamp> GetFileStamp(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
    return std::nullopt;
  }
  FileStamp stamp;
  stamp.write_time =
      (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
      data.ftLastWriteTime.dwLowDateTime;
  stamp.file_size =
      (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
  return stamp;
}

std::optional<std::string> ReadFileSharedUtf8(const std::wstring& path,
                                               size_t max_bytes) {
  HANDLE file = ::CreateFileW(
      path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return std::nullopt;
  }
  LARGE_INTEGER size{};
  std::optional<std::string> out;
  if (::GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
      static_cast<uint64_t>(size.QuadPart) <= max_bytes) {
    std::string buf(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (::ReadFile(file, buf.data(), static_cast<DWORD>(buf.size()), &read,
                   nullptr) &&
        read == buf.size()) {
      out = std::move(buf);
    }
  }
  ::CloseHandle(file);
  return out;
}

bool CopyFileShared(const std::wstring& src, const std::wstring& dst) {
  HANDLE in = ::CreateFileW(
      src.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (in == INVALID_HANDLE_VALUE) {
    return false;
  }
  HANDLE out = ::CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (out == INVALID_HANDLE_VALUE) {
    ::CloseHandle(in);
    return false;
  }
  uint8_t buf[65536];
  DWORD read = 0;
  bool ok = true;
  while (::ReadFile(in, buf, sizeof(buf), &read, nullptr) && read > 0) {
    DWORD written = 0;
    if (!::WriteFile(out, buf, read, &written, nullptr) || written != read) {
      ok = false;
      break;
    }
  }
  ::CloseHandle(out);
  ::CloseHandle(in);
  if (!ok) {
    ::DeleteFileW(dst.c_str());
  }
  return ok;
}

// Exception-free JSON parser for Chrome's Bookmarks & Local State files.
class JsonCursor {
 public:
  explicit JsonCursor(std::string_view src) : src_(src) {}

  void SkipWs() {
    while (pos_ < src_.size()) {
      const char c = src_[pos_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  char Peek() {
    SkipWs();
    return pos_ < src_.size() ? src_[pos_] : '\0';
  }

  bool Consume(char expected) {
    SkipWs();
    if (pos_ < src_.size() && src_[pos_] == expected) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool ParseString(std::string& out) {
    SkipWs();
    if (pos_ >= src_.size() || src_[pos_] != '"') {
      return false;
    }
    ++pos_;
    out.clear();
    while (pos_ < src_.size()) {
      const char c = src_[pos_++];
      if (c == '"') {
        return true;
      }
      if (c == '\\') {
        if (pos_ >= src_.size()) {
          return false;
        }
        const char esc = src_[pos_++];
        switch (esc) {
          case '"':
          case '\\':
          case '/':
            out.push_back(esc);
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u': {
            uint32_t cp = 0;
            if (!ParseHex4(cp)) {
              return false;
            }
            if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 2 <= src_.size() &&
                src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
              pos_ += 2;
              uint32_t low = 0;
              if (!ParseHex4(low)) {
                return false;
              }
              if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
              }
            }
            AppendUtf8(out, cp);
            break;
          }
          default:
            out.push_back(esc);
            break;
        }
      } else {
        out.push_back(c);
      }
    }
    return false;
  }

  bool SkipValue(int depth = 0) {
    if (depth > 64) {
      return false;
    }
    const char c = Peek();
    if (c == '"') {
      std::string dummy;
      return ParseString(dummy);
    }
    if (c == '{') {
      ++pos_;
      if (Consume('}')) {
        return true;
      }
      std::string key;
      while (true) {
        if (!ParseString(key) || !Consume(':') || !SkipValue(depth + 1)) {
          return false;
        }
        if (Consume(',')) {
          continue;
        }
        return Consume('}');
      }
    }
    if (c == '[') {
      ++pos_;
      if (Consume(']')) {
        return true;
      }
      while (true) {
        if (!SkipValue(depth + 1)) {
          return false;
        }
        if (Consume(',')) {
          continue;
        }
        return Consume(']');
      }
    }
    if (c == 't' && src_.substr(pos_).starts_with("true")) {
      pos_ += 4;
      return true;
    }
    if (c == 'f' && src_.substr(pos_).starts_with("false")) {
      pos_ += 5;
      return true;
    }
    if (c == 'n' && src_.substr(pos_).starts_with("null")) {
      pos_ += 4;
      return true;
    }
    const size_t start = pos_;
    while (pos_ < src_.size()) {
      const char ch = src_[pos_];
      if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '+' || ch == '.' ||
          ch == 'e' || ch == 'E') {
        ++pos_;
      } else {
        break;
      }
    }
    return pos_ > start;
  }

 private:
  bool ParseHex4(uint32_t& value) {
    if (pos_ + 4 > src_.size()) {
      return false;
    }
    value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = src_[pos_++];
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return false;
      }
    }
    return true;
  }

  static void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
      out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  std::string_view src_;
  size_t pos_ = 0;
};

bool ParseBookmarkNode(JsonCursor& cur, BookmarkNode& node, int depth) {
  if (depth > 48 || !cur.Consume('{')) {
    return false;
  }
  if (cur.Consume('}')) {
    return true;
  }

  std::string type;
  std::string key;
  std::string val;
  while (true) {
    if (!cur.ParseString(key) || !cur.Consume(':')) {
      return false;
    }
    if (key == "id") {
      if (!cur.ParseString(val)) {
        return false;
      }
      node.id = Utf8ToWide(val);
    } else if (key == "name") {
      if (!cur.ParseString(val)) {
        return false;
      }
      node.title = Utf8ToWide(val);
    } else if (key == "url") {
      if (!cur.ParseString(val)) {
        return false;
      }
      node.url = Utf8ToWide(val);
    } else if (key == "type") {
      if (!cur.ParseString(type)) {
        return false;
      }
      node.is_folder = (type == "folder");
    } else if (key == "children") {
      if (!cur.Consume('[')) {
        return false;
      }
      if (!cur.Consume(']')) {
        while (true) {
          BookmarkNode child;
          if (!ParseBookmarkNode(cur, child, depth + 1)) {
            return false;
          }
          if (child.is_folder || !child.url.empty()) {
            node.children.push_back(std::move(child));
          }
          if (cur.Consume(',')) {
            continue;
          }
          if (!cur.Consume(']')) {
            return false;
          }
          break;
        }
      }
    } else {
      if (!cur.SkipValue(depth + 1)) {
        return false;
      }
    }

    if (cur.Consume(',')) {
      continue;
    }
    return cur.Consume('}');
  }
}

struct ParsedBookmarksRoots {
  BookmarkNode bookmark_bar;
  BookmarkNode other;
};

bool ParseBookmarksJson(std::string_view json, ParsedBookmarksRoots& roots) {
  if (json.starts_with("\xEF\xBB\xBF")) {
    json.remove_prefix(3);
  }
  JsonCursor cur(json);
  if (!cur.Consume('{')) {
    return false;
  }
  if (cur.Consume('}')) {
    return true;
  }

  std::string key;
  while (true) {
    if (!cur.ParseString(key) || !cur.Consume(':')) {
      return false;
    }
    if (key == "roots") {
      if (!cur.Consume('{')) {
        return false;
      }
      if (!cur.Consume('}')) {
        std::string root_key;
        while (true) {
          if (!cur.ParseString(root_key) || !cur.Consume(':')) {
            return false;
          }
          if (root_key == "bookmark_bar") {
            if (!ParseBookmarkNode(cur, roots.bookmark_bar, 1)) {
              return false;
            }
          } else if (root_key == "other") {
            if (!ParseBookmarkNode(cur, roots.other, 1)) {
              return false;
            }
          } else {
            if (!cur.SkipValue(1)) {
              return false;
            }
          }
          if (cur.Consume(',')) {
            continue;
          }
          if (!cur.Consume('}')) {
            return false;
          }
          break;
        }
      }
    } else {
      if (!cur.SkipValue(1)) {
        return false;
      }
    }

    if (cur.Consume(',')) {
      continue;
    }
    return cur.Consume('}');
  }
}

std::optional<std::wstring> ParseLastUsedProfile(std::string_view json) {
  if (json.starts_with("\xEF\xBB\xBF")) {
    json.remove_prefix(3);
  }
  JsonCursor cur(json);
  if (!cur.Consume('{')) {
    return std::nullopt;
  }
  if (cur.Consume('}')) {
    return std::nullopt;
  }
  std::string key;
  while (true) {
    if (!cur.ParseString(key) || !cur.Consume(':')) {
      return std::nullopt;
    }
    if (key == "profile") {
      if (!cur.Consume('{')) {
        return std::nullopt;
      }
      if (!cur.Consume('}')) {
        std::string pkey;
        std::string val;
        while (true) {
          if (!cur.ParseString(pkey) || !cur.Consume(':')) {
            return std::nullopt;
          }
          if (pkey == "last_used") {
            if (cur.ParseString(val) && !val.empty()) {
              return Utf8ToWide(val);
            }
            return std::nullopt;
          }
          if (!cur.SkipValue(1)) {
            return std::nullopt;
          }
          if (cur.Consume(',')) {
            continue;
          }
          if (!cur.Consume('}')) {
            return std::nullopt;
          }
          break;
        }
      }
    } else {
      if (!cur.SkipValue(1)) {
        return std::nullopt;
      }
    }
    if (cur.Consume(',')) {
      continue;
    }
    break;
  }
  return std::nullopt;
}

bool HasBookmarksFile(const std::wstring& dir) {
  return !dir.empty() &&
         ::GetFileAttributesW((dir + L"\\Bookmarks").c_str()) !=
             INVALID_FILE_ATTRIBUTES;
}

std::optional<std::wstring> GetUserDataDirectory() {
  // 1. Explicit --user-data-dir on browser command line (portable mode).
  if (auto cmd_dir = GetSwitchValue(::GetCommandLineW(), L"--user-data-dir");
      cmd_dir && !cmd_dir->empty()) {
    std::wstring resolved = GetAbsolutePath(*cmd_dir);
    if (::GetFileAttributesW(resolved.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return resolved;
    }
  }

  // 2. Ask chrome_elf.dll (standard Google Chrome).
  using GetUserDataDirectoryThunkFn =
      bool (*)(wchar_t*, size_t, wchar_t*, size_t, bool*);
  if (HMODULE chrome_elf = ::GetModuleHandleW(L"chrome_elf.dll")) {
    if (auto fn = reinterpret_cast<GetUserDataDirectoryThunkFn>(
            ::GetProcAddress(chrome_elf, "GetUserDataDirectoryThunk"))) {
      std::vector<wchar_t> dir(4096, L'\0');
      std::vector<wchar_t> invalid(4096, L'\0');
      if (fn(dir.data(), dir.size(), invalid.data(), invalid.size(), nullptr) &&
          dir[0] != L'\0') {
        return std::wstring(dir.data());
      }
    }
  }

  // 3. Configured data_dir from chrome++.ini.
  if (const auto& cfg_dir = config.GetUserDataDir();
      cfg_dir && !cfg_dir->empty() &&
      ::GetFileAttributesW(cfg_dir->c_str()) != INVALID_FILE_ATTRIBUTES) {
    return *cfg_dir;
  }

  // 4. Standard system locations fallback.
  for (const wchar_t* rel : {
           L"%LOCALAPPDATA%\\Google\\Chrome\\User Data",
           L"%LOCALAPPDATA%\\Chromium\\User Data",
           L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data",
       }) {
    std::wstring expanded = ExpandEnvironmentPath(rel);
    while (!expanded.empty() && expanded.back() == L'\0') {
      expanded.pop_back();
    }
    if (!expanded.empty() &&
        ::GetFileAttributesW(expanded.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return expanded;
    }
  }
  return std::nullopt;
}

std::optional<std::wstring> GetActiveProfileDir() {
  const auto user_data = GetUserDataDirectory();
  if (!user_data) {
    return std::nullopt;
  }

  // Try Local State's profile.last_used first.
  const std::wstring local_state = *user_data + L"\\Local State";
  if (const auto json = ReadFileSharedUtf8(local_state, 16 * 1024 * 1024)) {
    if (const auto last = ParseLastUsedProfile(*json)) {
      std::wstring candidate = *user_data + L"\\" + *last;
      if (HasBookmarksFile(candidate)) {
        return candidate;
      }
    }
  }

  // Common profile names.
  for (const wchar_t* name :
       {L"Default", L"Profile 1", L"Profile 2", L"Profile 3"}) {
    std::wstring candidate = *user_data + L"\\" + name;
    if (HasBookmarksFile(candidate)) {
      return candidate;
    }
  }

  // Scan subdirectories for the most recently modified Bookmarks file.
  WIN32_FIND_DATAW fd{};
  HANDLE find = ::FindFirstFileW((*user_data + L"\\*").c_str(), &fd);
  std::wstring best_dir;
  uint64_t best_time = 0;
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
          fd.cFileName[0] != L'.') {
        std::wstring dir = *user_data + L"\\" + fd.cFileName;
        if (auto stamp = GetFileStamp(dir + L"\\Bookmarks")) {
          if (stamp->write_time >= best_time) {
            best_time = stamp->write_time;
            best_dir = std::move(dir);
          }
        }
      }
    } while (::FindNextFileW(find, &fd));
    ::FindClose(find);
  }
  if (!best_dir.empty()) {
    return best_dir;
  }

  return *user_data + L"\\Default";
}

std::shared_ptr<FaviconBitmap> DecodePngToPbgra(
    IWICImagingFactory* factory,
    const uint8_t* data,
    size_t size) {
  if (!factory || !data || size == 0) {
    return nullptr;
  }

  ComPtr<IWICStream> stream;
  if (FAILED(factory->CreateStream(&stream)) ||
      FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data),
                                          static_cast<DWORD>(size)))) {
    return nullptr;
  }

  ComPtr<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromStream(
          stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) {
    return nullptr;
  }

  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, &frame))) {
    return nullptr;
  }

  UINT w = 0;
  UINT h = 0;
  if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0 || w > 256 ||
      h > 256) {
    return nullptr;
  }

  ComPtr<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
    return nullptr;
  }

  auto bmp = std::make_shared<FaviconBitmap>();
  bmp->width = static_cast<int>(w);
  bmp->height = static_cast<int>(h);
  const UINT stride = w * 4;
  bmp->bgra_pixels.resize(static_cast<size_t>(stride) * h);
  if (FAILED(converter->CopyPixels(
          nullptr, stride, static_cast<UINT>(bmp->bgra_pixels.size()),
          bmp->bgra_pixels.data()))) {
    return nullptr;
  }
  return bmp;
}

struct BookmarkStore {
  std::mutex mutex;
  std::wstring profile_dir;
  std::optional<FileStamp> bookmarks_stamp;
  bool last_show_other = true;
  std::shared_ptr<const std::vector<BookmarkNode>> root_items =
      std::make_shared<std::vector<BookmarkNode>>();

  std::mutex favicon_mutex;
  std::unordered_map<std::wstring, std::shared_ptr<const FaviconBitmap>>
      favicons;
  std::optional<FileStamp> favicons_stamp;
  std::atomic<bool> favicons_loading{false};
};

BookmarkStore& GetStore() {
  static BookmarkStore store;
  return store;
}

}  // namespace

BookmarkModel& BookmarkModel::Instance() {
  static BookmarkModel instance;
  return instance;
}

bool BookmarkModel::EnsureLoaded(bool show_other_bookmarks) {
  const auto profile_dir = GetActiveProfileDir();
  if (!profile_dir) {
    return false;
  }

  const std::wstring bookmarks_path = *profile_dir + L"\\Bookmarks";
  const auto stamp = GetFileStamp(bookmarks_path);
  if (!stamp) {
    return false;
  }

  auto& store = GetStore();
  {
    std::lock_guard lock(store.mutex);
    if (store.profile_dir == *profile_dir &&
        store.bookmarks_stamp == stamp &&
        store.last_show_other == show_other_bookmarks) {
      return false;
    }
  }

  const auto json = ReadFileSharedUtf8(bookmarks_path, 64 * 1024 * 1024);
  if (!json) {
    return false;
  }

  ParsedBookmarksRoots roots;
  if (!ParseBookmarksJson(*json, roots)) {
    return false;
  }

  auto items = std::make_shared<std::vector<BookmarkNode>>(
      std::move(roots.bookmark_bar.children));
  if (show_other_bookmarks && !roots.other.children.empty()) {
    BookmarkNode other_folder;
    other_folder.id = std::move(roots.other.id);
    other_folder.title =
        roots.other.title.empty() ? L"其他书签" : std::move(roots.other.title);
    other_folder.is_folder = true;
    other_folder.children = std::move(roots.other.children);
    items->push_back(std::move(other_folder));
  }

  std::lock_guard lock(store.mutex);
  store.profile_dir = *profile_dir;
  store.bookmarks_stamp = stamp;
  store.last_show_other = show_other_bookmarks;
  store.root_items = std::move(items);
  return true;
}

std::shared_ptr<const std::vector<BookmarkNode>> BookmarkModel::GetRootItems()
    const {
  auto& store = GetStore();
  std::lock_guard lock(store.mutex);
  return store.root_items;
}

std::shared_ptr<const FaviconBitmap> BookmarkModel::GetFavicon(
    const std::wstring& url) const {
  if (url.empty()) {
    return nullptr;
  }
  auto& store = GetStore();
  std::lock_guard lock(store.favicon_mutex);
  auto it = store.favicons.find(url);
  if (it != store.favicons.end()) {
    return it->second;
  }
  return nullptr;
}

void BookmarkModel::RequestFaviconsAsync(HWND notify_hwnd, UINT notify_msg) {
  auto& store = GetStore();
  std::wstring profile_dir;
  {
    std::lock_guard lock(store.mutex);
    profile_dir = store.profile_dir;
  }
  if (profile_dir.empty()) {
    return;
  }

  const std::wstring favicons_path = profile_dir + L"\\Favicons";
  const auto stamp = GetFileStamp(favicons_path);
  if (!stamp) {
    return;
  }

  {
    std::lock_guard lock(store.favicon_mutex);
    if (store.favicons_stamp == stamp) {
      return;
    }
  }

  bool expected = false;
  if (!store.favicons_loading.compare_exchange_strong(expected, true)) {
    return;
  }

  std::thread([favicons_path, stamp, notify_hwnd, notify_msg]() {
    auto& s = GetStore();

    wchar_t temp_dir[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, temp_dir);
    static std::atomic<uint32_t> seq{0};
    const std::wstring temp_db =
        std::wstring(temp_dir) + L"chrome_plus_fav_" +
        std::to_wstring(::GetCurrentProcessId()) + L"_" +
        std::to_wstring(++seq) + L".db";

    if (CopyFileShared(favicons_path, temp_db)) {
      // Copy WAL/SHM if Chrome uses WAL mode for Favicons.
      CopyFileShared(favicons_path + L"-wal", temp_db + L"-wal");
      CopyFileShared(favicons_path + L"-shm", temp_db + L"-shm");

      const HRESULT com_hr =
          ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      ComPtr<IWICImagingFactory> wic_factory;
      ::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                         CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_factory));

      sqlite3* db = nullptr;
      const std::string db_utf8 = WideToUtf8(temp_db);
      if (wic_factory &&
          sqlite3_open_v2(db_utf8.c_str(), &db, SQLITE_OPEN_READONLY,
                          nullptr) == SQLITE_OK &&
          db) {
        constexpr char kSql[] =
            "SELECT im.page_url, fb.image_data, fb.width "
            "FROM icon_mapping im "
            "JOIN favicon_bitmaps fb ON im.icon_id = fb.icon_id "
            "WHERE fb.image_data IS NOT NULL "
            "ORDER BY im.page_url, fb.width ASC";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) == SQLITE_OK &&
            stmt) {
          std::unordered_map<std::wstring, std::shared_ptr<const FaviconBitmap>>
              decoded_map;
          std::string last_url;
          int best_diff = 99999;

          while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* url_txt = sqlite3_column_text(stmt, 0);
            const void* blob = sqlite3_column_blob(stmt, 1);
            const int blob_len = sqlite3_column_bytes(stmt, 1);
            const int width = sqlite3_column_int(stmt, 2);
            if (!url_txt || !blob || blob_len <= 0) {
              continue;
            }
            const std::string_view url_sv(
                reinterpret_cast<const char*>(url_txt));
            // Prefer 32px or 16px closest to 24px (sharp at both 1x and 1.5x/2x DPI).
            const int diff = std::abs(width - 24);
            if (url_sv == last_url && diff >= best_diff) {
              continue;
            }
            if (auto bmp = DecodePngToPbgra(
                    wic_factory.Get(),
                    static_cast<const uint8_t*>(blob),
                    static_cast<size_t>(blob_len))) {
              last_url = std::string(url_sv);
              best_diff = diff;
              decoded_map[Utf8ToWide(url_sv)] = std::move(bmp);
            }
          }
          sqlite3_finalize(stmt);

          if (!decoded_map.empty()) {
            std::lock_guard lock(s.favicon_mutex);
            for (auto& [k, v] : decoded_map) {
              s.favicons[k] = std::move(v);
            }
            s.favicons_stamp = stamp;
          }
        }
        sqlite3_close(db);
      }

      if (SUCCEEDED(com_hr)) {
        ::CoUninitialize();
      }
      ::DeleteFileW(temp_db.c_str());
      ::DeleteFileW((temp_db + L"-wal").c_str());
      ::DeleteFileW((temp_db + L"-shm").c_str());
    }

    s.favicons_loading.store(false);
    if (notify_hwnd && ::IsWindow(notify_hwnd)) {
      ::PostMessageW(notify_hwnd, notify_msg, 0, 0);
    }
  }).detach();
}
