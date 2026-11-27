#include "bookmark_model.h"

#include <windows.h>

#include <bcrypt.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
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
    } else if (key == "guid") {
      if (!cur.ParseString(val)) {
        return false;
      }
      node.guid = Utf8ToWide(val);
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
  BookmarkNode synced;
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
          } else if (root_key == "synced") {
            if (!ParseBookmarkNode(cur, roots.synced, 1)) {
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

constexpr wchar_t kOpsFileName[] = L"ChromePlus_BookmarkOps.json";

struct CombinedBookmarkStamp {
  std::optional<FileStamp> local_stamp;
  std::optional<FileStamp> account_stamp;
  std::optional<FileStamp> journal_stamp;

  bool HasAny() const {
    return local_stamp.has_value() || account_stamp.has_value();
  }
  uint64_t MaxWriteTime() const {
    const uint64_t t1 = local_stamp ? local_stamp->write_time : 0;
    const uint64_t t2 = account_stamp ? account_stamp->write_time : 0;
    return t1 >= t2 ? t1 : t2;
  }
  friend bool operator==(const CombinedBookmarkStamp&,
                         const CombinedBookmarkStamp&) = default;
};

CombinedBookmarkStamp GetProfileBookmarkStamp(const std::wstring& profile_dir) {
  CombinedBookmarkStamp s;
  if (profile_dir.empty()) {
    return s;
  }
  s.local_stamp = GetFileStamp(profile_dir + L"\\Bookmarks");
  s.account_stamp = GetFileStamp(profile_dir + L"\\AccountBookmarks");
  s.journal_stamp = GetFileStamp(profile_dir + L"\\" + kOpsFileName);
  if (!s.journal_stamp) {
    s.journal_stamp = GetFileStamp(GetAppDir() + L"\\" + kOpsFileName);
  }
  return s;
}

bool IsExistingDirectory(const std::wstring& path) {
  if (path.empty()) {
    return false;
  }
  const DWORD attrs = ::GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::optional<std::wstring> FindProcessSingletonUserDataDir() {
  constexpr wchar_t kWindowClass[] = L"Chrome_MessageWindow";
  const DWORD current_pid = ::GetCurrentProcessId();
  for (HWND window =
           ::FindWindowExW(HWND_MESSAGE, nullptr, kWindowClass, nullptr);
       window;
       window = ::FindWindowExW(HWND_MESSAGE, window, kWindowClass, nullptr)) {
    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    if (pid != current_pid) {
      continue;
    }
    wchar_t title[MAX_PATH] = {};
    const int len = ::GetWindowTextW(window, title, MAX_PATH);
    if (len > 0) {
      std::wstring candidate(title, static_cast<size_t>(len));
      if (IsExistingDirectory(candidate)) {
        return candidate;
      }
    }
  }
  return std::nullopt;
}

std::vector<std::wstring> CollectUserDataDirCandidates() {
  std::vector<std::wstring> out;
  auto add = [&](std::wstring dir) {
    while (!dir.empty() && (dir.back() == L'\0' || dir.back() == L'\\' ||
                            dir.back() == L'/')) {
      dir.pop_back();
    }
    if (!IsExistingDirectory(dir)) {
      return;
    }
    for (const auto& existing : out) {
      if (_wcsicmp(existing.c_str(), dir.c_str()) == 0) {
        return;
      }
    }
    out.push_back(std::move(dir));
  };

  // 1. Live ProcessSingleton message window title.
  if (auto singleton_dir = FindProcessSingletonUserDataDir()) {
    add(std::move(*singleton_dir));
  }

  // 2. chrome_elf.dll (already resolves UAC fallback if Program Files isn't writable).
  using GetUserDataDirectoryThunkFn =
      bool (*)(wchar_t*, size_t, wchar_t*, size_t, bool*);
  if (HMODULE chrome_elf = ::GetModuleHandleW(L"chrome_elf.dll")) {
    if (auto fn = reinterpret_cast<GetUserDataDirectoryThunkFn>(
            ::GetProcAddress(chrome_elf, "GetUserDataDirectoryThunk"))) {
      wchar_t dir[MAX_PATH] = {};
      wchar_t invalid[MAX_PATH] = {};
      if (fn(dir, std::size(dir), invalid, std::size(invalid), nullptr) &&
          dir[0] != L'\0') {
        add(std::wstring(dir));
      }
    }
  }

  // 3. Explicit --user-data-dir on browser command line.
  if (auto cmd_dir = GetSwitchValue(::GetCommandLineW(), L"--user-data-dir");
      cmd_dir && !cmd_dir->empty()) {
    add(GetAbsolutePath(*cmd_dir));
  }

  // 4. Configured data_dir from chrome++.ini.
  if (const auto& cfg_dir = config.GetUserDataDir();
      cfg_dir && !cfg_dir->empty()) {
    add(*cfg_dir);
  }

  // 5. Standard Windows Chrome/Chromium/Edge locations.
  for (const wchar_t* rel : {
           L"%LOCALAPPDATA%\\Google\\Chrome\\User Data",
           L"%LOCALAPPDATA%\\Google\\Chrome Beta\\User Data",
           L"%LOCALAPPDATA%\\Google\\Chrome SxS\\User Data",
           L"%LOCALAPPDATA%\\Chromium\\User Data",
           L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data",
       }) {
    add(ExpandEnvironmentPath(rel));
  }

  return out;
}

std::optional<std::wstring> FindProfileWithBookmarksInUserData(
    const std::wstring& user_data) {
  // Check if user_data itself directly contains Bookmarks/AccountBookmarks.
  if (GetProfileBookmarkStamp(user_data).HasAny()) {
    return user_data;
  }

  // Try Local State's profile.last_used first.
  const std::wstring local_state = user_data + L"\\Local State";
  if (const auto json = ReadFileSharedUtf8(local_state, 16 * 1024 * 1024)) {
    if (const auto last = ParseLastUsedProfile(*json)) {
      std::wstring candidate = user_data + L"\\" + *last;
      if (GetProfileBookmarkStamp(candidate).HasAny()) {
        return candidate;
      }
    }
  }

  // Common profile names.
  for (const wchar_t* name : {L"Default", L"Profile 1", L"Profile 2",
                              L"Profile 3", L"Profile 4", L"Profile 5"}) {
    std::wstring candidate = user_data + L"\\" + name;
    if (GetProfileBookmarkStamp(candidate).HasAny()) {
      return candidate;
    }
  }

  // Scan subdirectories for the most recently modified Bookmarks/AccountBookmarks.
  WIN32_FIND_DATAW fd{};
  HANDLE find = ::FindFirstFileW((user_data + L"\\*").c_str(), &fd);
  std::wstring best_dir;
  uint64_t best_time = 0;
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
          fd.cFileName[0] != L'.') {
        std::wstring dir = user_data + L"\\" + fd.cFileName;
        const auto stamp = GetProfileBookmarkStamp(dir);
        if (stamp.HasAny() && stamp.MaxWriteTime() >= best_time) {
          best_time = stamp.MaxWriteTime();
          best_dir = std::move(dir);
        }
      }
    } while (::FindNextFileW(find, &fd));
    ::FindClose(find);
  }
  if (!best_dir.empty()) {
    return best_dir;
  }
  return std::nullopt;
}

std::optional<std::wstring> FindBestActiveProfileDir(
    std::wstring* tried_summary) {
  const auto candidates = CollectUserDataDirCandidates();
  for (const auto& ud : candidates) {
    if (auto profile = FindProfileWithBookmarksInUserData(ud)) {
      return profile;
    }
    if (tried_summary) {
      if (!tried_summary->empty()) {
        *tried_summary += L" | ";
      }
      *tried_summary += ud;
    }
  }
  return std::nullopt;
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
  std::wstring last_diag = L"尚未加载";
  CombinedBookmarkStamp bookmarks_stamp;
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

void TagNodeSourceRecursive(BookmarkNode& node, BookmarkSource source) {
  node.source = source;
  for (auto& child : node.children) {
    TagNodeSourceRecursive(child, source);
  }
}

// Lossless, exception-free JSON AST for modifying Chrome's Bookmarks /
// AccountBookmarks files while preserving all metadata (guid, meta_info, etc.).
struct JsonValue {
  enum class Type {
    kNull,
    kBool,
    kNumber,
    kString,
    kArray,
    kObject,
  };
  Type type = Type::kNull;
  bool bool_val = false;
  std::string str_val;  // String content or verbatim number literal.
  std::vector<JsonValue> arr_val;
  std::vector<std::pair<std::string, JsonValue>> obj_val;

  JsonValue* Find(std::string_view key) {
    if (type != Type::kObject) {
      return nullptr;
    }
    for (auto& [k, v] : obj_val) {
      if (k == key) {
        return &v;
      }
    }
    return nullptr;
  }

  const JsonValue* Find(std::string_view key) const {
    if (type != Type::kObject) {
      return nullptr;
    }
    for (const auto& [k, v] : obj_val) {
      if (k == key) {
        return &v;
      }
    }
    return nullptr;
  }

  std::string GetString(std::string_view key) const {
    const JsonValue* v = Find(key);
    return (v && v->type == Type::kString) ? v->str_val : std::string();
  }

  void SetString(std::string_view key, std::string value) {
    if (type != Type::kObject) {
      return;
    }
    if (JsonValue* existing = Find(key)) {
      existing->type = Type::kString;
      existing->str_val = std::move(value);
      return;
    }
    JsonValue jv;
    jv.type = Type::kString;
    jv.str_val = std::move(value);
    obj_val.emplace_back(std::string(key), std::move(jv));
  }
};

class AstJsonParser {
 public:
  explicit AstJsonParser(std::string_view src) : src_(src) {
    if (src_.starts_with("\xEF\xBB\xBF")) {
      src_.remove_prefix(3);
    }
  }

  bool Parse(JsonValue& out, int depth = 0) {
    if (depth > 64) {
      return false;
    }
    SkipWs();
    if (pos_ >= src_.size()) {
      return false;
    }
    const char c = src_[pos_];
    if (c == '{') {
      ++pos_;
      out.type = JsonValue::Type::kObject;
      out.obj_val.clear();
      if (Consume('}')) {
        return true;
      }
      while (true) {
        std::string key;
        JsonValue val;
        if (!ParseString(key) || !Consume(':') || !Parse(val, depth + 1)) {
          return false;
        }
        out.obj_val.emplace_back(std::move(key), std::move(val));
        if (Consume(',')) {
          continue;
        }
        return Consume('}');
      }
    }
    if (c == '[') {
      ++pos_;
      out.type = JsonValue::Type::kArray;
      out.arr_val.clear();
      if (Consume(']')) {
        return true;
      }
      while (true) {
        JsonValue elem;
        if (!Parse(elem, depth + 1)) {
          return false;
        }
        out.arr_val.push_back(std::move(elem));
        if (Consume(',')) {
          continue;
        }
        return Consume(']');
      }
    }
    if (c == '"') {
      out.type = JsonValue::Type::kString;
      return ParseString(out.str_val);
    }
    if (c == 't' && src_.substr(pos_).starts_with("true")) {
      pos_ += 4;
      out.type = JsonValue::Type::kBool;
      out.bool_val = true;
      return true;
    }
    if (c == 'f' && src_.substr(pos_).starts_with("false")) {
      pos_ += 5;
      out.type = JsonValue::Type::kBool;
      out.bool_val = false;
      return true;
    }
    if (c == 'n' && src_.substr(pos_).starts_with("null")) {
      pos_ += 4;
      out.type = JsonValue::Type::kNull;
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
    if (pos_ == start) {
      return false;
    }
    out.type = JsonValue::Type::kNumber;
    out.str_val = std::string(src_.substr(start, pos_ - start));
    return true;
  }

 private:
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

  bool Consume(char ch) {
    SkipWs();
    if (pos_ < src_.size() && src_[pos_] == ch) {
      ++pos_;
      return true;
    }
    return false;
  }

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

  std::string_view src_;
  size_t pos_ = 0;
};

void SerializeJsonString(std::string& out, std::string_view s) {
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kHex[(c >> 4) & 0xF]);
          out.push_back(kHex[c & 0xF]);
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out.push_back('"');
}

void SerializeJsonValue(std::string& out, const JsonValue& v, int indent) {
  auto add_indent = [&](int n) { out.append(static_cast<size_t>(n), ' '); };
  switch (v.type) {
    case JsonValue::Type::kNull:
      out += "null";
      break;
    case JsonValue::Type::kBool:
      out += v.bool_val ? "true" : "false";
      break;
    case JsonValue::Type::kNumber:
      out += v.str_val;
      break;
    case JsonValue::Type::kString:
      SerializeJsonString(out, v.str_val);
      break;
    case JsonValue::Type::kArray:
      if (v.arr_val.empty()) {
        out += "[  ]";
      } else {
        out += "[\n";
        for (size_t i = 0; i < v.arr_val.size(); ++i) {
          add_indent(indent + 3);
          SerializeJsonValue(out, v.arr_val[i], indent + 3);
          if (i + 1 < v.arr_val.size()) {
            out.push_back(',');
          }
          out.push_back('\n');
        }
        add_indent(indent);
        out.push_back(']');
      }
      break;
    case JsonValue::Type::kObject:
      if (v.obj_val.empty()) {
        out += "{ }";
      } else {
        out += "{\n";
        for (size_t i = 0; i < v.obj_val.size(); ++i) {
          add_indent(indent + 3);
          SerializeJsonString(out, v.obj_val[i].first);
          out += ": ";
          SerializeJsonValue(out, v.obj_val[i].second, indent + 3);
          if (i + 1 < v.obj_val.size()) {
            out.push_back(',');
          }
          out.push_back('\n');
        }
        add_indent(indent);
        out.push_back('}');
      }
      break;
  }
}

void UpdateMd5ForNode(BCRYPT_HASH_HANDLE hash, const JsonValue& node) {
  if (node.type != JsonValue::Type::kObject) {
    return;
  }
  const std::string id = node.GetString("id");
  const std::string name_utf8 = node.GetString("name");
  const std::string type = node.GetString("type");
  const std::wstring name_u16 = Utf8ToWide(name_utf8);

  auto feed = [&](const void* data, size_t len) {
    if (len > 0) {
      ::BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)),
                       static_cast<ULONG>(len), 0);
    }
  };

  if (type == "url") {
    const std::string url = node.GetString("url");
    feed(id.data(), id.size());
    feed(name_u16.data(), name_u16.size() * sizeof(wchar_t));
    feed("url", 3);
    feed(url.data(), url.size());
  } else {
    feed(id.data(), id.size());
    feed(name_u16.data(), name_u16.size() * sizeof(wchar_t));
    feed("folder", 6);
    if (const JsonValue* children = node.Find("children");
        children && children->type == JsonValue::Type::kArray) {
      for (const auto& child : children->arr_val) {
        UpdateMd5ForNode(hash, child);
      }
    }
  }
}

std::string ComputeChromeBookmarksChecksum(const JsonValue& root_doc) {
  const JsonValue* roots = root_doc.Find("roots");
  if (!roots || roots->type != JsonValue::Type::kObject) {
    return {};
  }

  BCRYPT_ALG_HANDLE alg = nullptr;
  if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) !=
      0) {
    return {};
  }

  BCRYPT_HASH_HANDLE hash = nullptr;
  std::string hex;
  if (::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
    for (const char* key : {"bookmark_bar", "other", "synced"}) {
      if (const JsonValue* r = roots->Find(key)) {
        UpdateMd5ForNode(hash, *r);
      }
    }
    uint8_t digest[16] = {};
    if (::BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
      constexpr char kHex[] = "0123456789abcdef";
      hex.reserve(32);
      for (uint8_t b : digest) {
        hex.push_back(kHex[b >> 4]);
        hex.push_back(kHex[b & 0x0F]);
      }
    }
    ::BCryptDestroyHash(hash);
  }
  ::BCryptCloseAlgorithmProvider(alg, 0);
  return hex;
}

uint64_t GetChromeWebKitTimestamp() {
  FILETIME ft{};
  ::GetSystemTimeAsFileTime(&ft);
  const uint64_t win_100ns =
      (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
  return win_100ns / 10;  // Microseconds since 1601-01-01.
}

void FindMaxNumericId(const JsonValue& node, uint64_t& max_id) {
  if (node.type != JsonValue::Type::kObject) {
    return;
  }
  const std::string id = node.GetString("id");
  uint64_t num = 0;
  const auto [ptr, ec] = std::from_chars(id.data(), id.data() + id.size(), num);
  if (ec == std::errc{} && ptr == id.data() + id.size() && num > max_id) {
    max_id = num;
  }
  if (const JsonValue* children = node.Find("children");
      children && children->type == JsonValue::Type::kArray) {
    for (const auto& child : children->arr_val) {
      FindMaxNumericId(child, max_id);
    }
  }
}

bool WriteFileAtomicUtf8(const std::wstring& path, std::string_view content) {
  const std::wstring tmp_path =
      path + L".cpp_tmp_" + std::to_wstring(::GetCurrentProcessId());
  HANDLE file = ::CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    const bool ok =
        ::WriteFile(file, content.data(), static_cast<DWORD>(content.size()),
                    &written, nullptr) &&
        written == content.size();
    ::CloseHandle(file);
    if (ok && ::MoveFileExW(tmp_path.c_str(), path.c_str(),
                            MOVEFILE_REPLACE_EXISTING |
                                MOVEFILE_WRITE_THROUGH)) {
      return true;
    }
    ::DeleteFileW(tmp_path.c_str());
  }

  // Fallback: direct shared overwrite if MoveFileExW hit a sharing conflict.
  HANDLE direct = ::CreateFileW(
      path.c_str(), GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (direct == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  const bool ok =
      ::WriteFile(direct, content.data(), static_cast<DWORD>(content.size()),
                  &written, nullptr) &&
      written == content.size();
  ::CloseHandle(direct);
  return ok;
}

bool SaveBookmarkDoc(const std::wstring& path, JsonValue& doc) {
  const std::string checksum = ComputeChromeBookmarksChecksum(doc);
  if (!checksum.empty() && doc.Find("checksum") != nullptr) {
    doc.SetString("checksum", checksum);
  }
  std::string out;
  out.reserve(65536);
  SerializeJsonValue(out, doc, 0);
  out.push_back('\n');
  return WriteFileAtomicUtf8(path, out);
}

std::string GenerateUuidV4() {
  uint8_t b[16] = {};
  if (::BCryptGenRandom(nullptr, b, sizeof(b),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    const uint64_t t1 = ::GetTickCount64() ^
                        (static_cast<uint64_t>(::GetCurrentProcessId()) << 32);
    const uint64_t t2 = t1 * 6364136223846793005ULL + 1442695040888963407ULL;
    std::memcpy(b, &t1, 8);
    std::memcpy(b + 8, &t2, 8);
  }
  b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);  // version 4
  b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);  // variant 1

  constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out.push_back('-');
    }
    out.push_back(kHex[b[i] >> 4]);
    out.push_back(kHex[b[i] & 0x0F]);
  }
  return out;
}

// Durable edit journal stored independently of Chrome's Bookmarks file so
// Chrome's in-memory BookmarkStorage / Sync can never revert user edits.
struct BookmarkOp {
  enum class Kind {
    kMove,
    kUpdate,
    kAdd,
    kDelete,
  };
  Kind kind = Kind::kMove;
  BookmarkSource dst_source = BookmarkSource::kLocal;
  std::string id;
  std::string guid;
  std::string dst_folder_id;
  std::string dst_folder_guid;
  std::string anchor_id;
  std::string anchor_guid;
  bool insert_after = true;
  bool is_folder = false;
  bool has_url = false;
  std::string title;
  std::string url;
};

std::wstring GetJournalPath(const std::wstring& profile_dir) {
  if (!profile_dir.empty()) {
    return profile_dir + L"\\" + kOpsFileName;
  }
  return GetAppDir() + L"\\" + kOpsFileName;
}

bool MatchesNodeIdOrGuid(const BookmarkNode& node,
                         std::wstring_view target_id,
                         std::wstring_view target_guid) {
  if (!target_guid.empty() && !node.guid.empty()) {
    return node.guid == target_guid;
  }
  return !target_id.empty() && node.id == target_id;
}

bool MatchesAstIdOrGuid(const JsonValue& node,
                        std::string_view target_id,
                        std::string_view target_guid) {
  if (node.type != JsonValue::Type::kObject) {
    return false;
  }
  if (!target_guid.empty()) {
    const std::string g = node.GetString("guid");
    if (!g.empty()) {
      return g == target_guid;
    }
  }
  return !target_id.empty() && node.GetString("id") == target_id;
}

bool SameOpTarget(const BookmarkOp& a,
                  std::string_view id,
                  std::string_view guid) {
  if (!guid.empty() && !a.guid.empty()) {
    return a.guid == guid;
  }
  return !id.empty() && a.id == id;
}

std::vector<BookmarkOp> LoadJournalFromDisk(const std::wstring& profile_dir) {
  std::vector<BookmarkOp> ops;
  std::optional<std::string> text =
      ReadFileSharedUtf8(GetJournalPath(profile_dir), 16 * 1024 * 1024);
  if (!text) {
    text = ReadFileSharedUtf8(GetAppDir() + L"\\" + kOpsFileName,
                              16 * 1024 * 1024);
  }
  if (!text) {
    return ops;
  }

  JsonValue root;
  AstJsonParser parser(*text);
  if (!parser.Parse(root) || root.type != JsonValue::Type::kArray) {
    return ops;
  }

  for (const auto& item : root.arr_val) {
    if (item.type != JsonValue::Type::kObject) {
      continue;
    }
    const std::string kind_str = item.GetString("op");
    BookmarkOp op;
    if (kind_str == "move") {
      op.kind = BookmarkOp::Kind::kMove;
    } else if (kind_str == "update") {
      op.kind = BookmarkOp::Kind::kUpdate;
    } else if (kind_str == "add") {
      op.kind = BookmarkOp::Kind::kAdd;
    } else if (kind_str == "delete") {
      op.kind = BookmarkOp::Kind::kDelete;
    } else {
      continue;
    }
    op.dst_source = item.GetString("dst_source") == "account"
                        ? BookmarkSource::kAccount
                        : BookmarkSource::kLocal;
    op.id = item.GetString("id");
    op.guid = item.GetString("guid");
    op.dst_folder_id = item.GetString("dst_folder_id");
    op.dst_folder_guid = item.GetString("dst_folder_guid");
    op.anchor_id = item.GetString("anchor_id");
    op.anchor_guid = item.GetString("anchor_guid");
    if (const JsonValue* v = item.Find("insert_after");
        v && v->type == JsonValue::Type::kBool) {
      op.insert_after = v->bool_val;
    }
    if (const JsonValue* v = item.Find("is_folder");
        v && v->type == JsonValue::Type::kBool) {
      op.is_folder = v->bool_val;
    }
    if (const JsonValue* v = item.Find("has_url");
        v && v->type == JsonValue::Type::kBool) {
      op.has_url = v->bool_val;
    }
    op.title = item.GetString("title");
    op.url = item.GetString("url");
    ops.push_back(std::move(op));
  }
  return ops;
}

bool SaveJournalToDisk(const std::wstring& profile_dir,
                       const std::vector<BookmarkOp>& ops) {
  JsonValue root;
  root.type = JsonValue::Type::kArray;
  root.arr_val.reserve(ops.size());

  auto make_bool = [](bool b) {
    JsonValue jv;
    jv.type = JsonValue::Type::kBool;
    jv.bool_val = b;
    return jv;
  };

  for (const auto& op : ops) {
    JsonValue obj;
    obj.type = JsonValue::Type::kObject;
    switch (op.kind) {
      case BookmarkOp::Kind::kMove:
        obj.SetString("op", "move");
        break;
      case BookmarkOp::Kind::kUpdate:
        obj.SetString("op", "update");
        break;
      case BookmarkOp::Kind::kAdd:
        obj.SetString("op", "add");
        break;
      case BookmarkOp::Kind::kDelete:
        obj.SetString("op", "delete");
        break;
    }
    if (op.dst_source == BookmarkSource::kAccount) {
      obj.SetString("dst_source", "account");
    }
    if (!op.id.empty()) obj.SetString("id", op.id);
    if (!op.guid.empty()) obj.SetString("guid", op.guid);
    if (!op.dst_folder_id.empty()) {
      obj.SetString("dst_folder_id", op.dst_folder_id);
    }
    if (!op.dst_folder_guid.empty()) {
      obj.SetString("dst_folder_guid", op.dst_folder_guid);
    }
    if (!op.anchor_id.empty()) obj.SetString("anchor_id", op.anchor_id);
    if (!op.anchor_guid.empty()) obj.SetString("anchor_guid", op.anchor_guid);
    if (op.kind == BookmarkOp::Kind::kMove ||
        op.kind == BookmarkOp::Kind::kAdd) {
      obj.obj_val.emplace_back("insert_after", make_bool(op.insert_after));
    }
    if (op.kind == BookmarkOp::Kind::kAdd) {
      obj.obj_val.emplace_back("is_folder", make_bool(op.is_folder));
    }
    if (op.kind == BookmarkOp::Kind::kUpdate) {
      obj.obj_val.emplace_back("has_url", make_bool(op.has_url));
    }
    if (op.kind == BookmarkOp::Kind::kUpdate ||
        op.kind == BookmarkOp::Kind::kAdd) {
      obj.SetString("title", op.title);
      if (!op.url.empty() || op.has_url) {
        obj.SetString("url", op.url);
      }
    }
    root.arr_val.push_back(std::move(obj));
  }

  std::string out;
  out.reserve(8192);
  SerializeJsonValue(out, root, 0);
  out.push_back('\n');

  if (WriteFileAtomicUtf8(GetJournalPath(profile_dir), out)) {
    return true;
  }
  return WriteFileAtomicUtf8(GetAppDir() + L"\\" + kOpsFileName, out);
}

void AppendAndCompactOp(std::vector<BookmarkOp>& ops, BookmarkOp op) {
  if (op.kind == BookmarkOp::Kind::kUpdate) {
    // Update an earlier Chrome++ Add op or Update op in-place if present.
    for (auto it = ops.rbegin(); it != ops.rend(); ++it) {
      if (SameOpTarget(*it, op.id, op.guid)) {
        if (it->kind == BookmarkOp::Kind::kAdd) {
          it->title = op.title;
          if (op.has_url) {
            it->url = op.url;
          }
          return;
        }
        if (it->kind == BookmarkOp::Kind::kUpdate) {
          it->title = op.title;
          if (op.has_url) {
            it->has_url = true;
            it->url = op.url;
          }
          return;
        }
      }
    }
    ops.push_back(std::move(op));
    return;
  }

  if (op.kind == BookmarkOp::Kind::kDelete) {
    // Drop earlier ops targeting this node, then always keep kDelete so even a
    // previously-flushed local addition is removed from Chrome's disk file too.
    std::erase_if(ops, [&](const BookmarkOp& existing) {
      return SameOpTarget(existing, op.id, op.guid);
    });
    ops.push_back(std::move(op));
    return;
  }

  ops.push_back(std::move(op));
}

// --- In-memory tree replay helpers ---

bool TreeContainsNode(const std::vector<BookmarkNode>& list,
                      std::wstring_view id,
                      std::wstring_view guid) {
  for (const auto& n : list) {
    if (MatchesNodeIdOrGuid(n, id, guid)) {
      return true;
    }
    if (n.is_folder && TreeContainsNode(n.children, id, guid)) {
      return true;
    }
  }
  return false;
}

BookmarkNode* FindNodeInTree(std::vector<BookmarkNode>& list,
                             std::wstring_view id,
                             std::wstring_view guid) {
  for (auto& n : list) {
    if (MatchesNodeIdOrGuid(n, id, guid)) {
      return &n;
    }
    if (n.is_folder) {
      if (BookmarkNode* hit = FindNodeInTree(n.children, id, guid)) {
        return hit;
      }
    }
  }
  return nullptr;
}

bool ExtractNodeFromTree(std::vector<BookmarkNode>& list,
                         std::wstring_view id,
                         std::wstring_view guid,
                         BookmarkNode& out) {
  for (auto it = list.begin(); it != list.end(); ++it) {
    if (MatchesNodeIdOrGuid(*it, id, guid)) {
      out = std::move(*it);
      list.erase(it);
      return true;
    }
    if (it->is_folder && ExtractNodeFromTree(it->children, id, guid, out)) {
      return true;
    }
  }
  return false;
}

std::vector<BookmarkNode>* ResolveTreeFolderChildren(
    std::vector<BookmarkNode>& bar_items,
    std::vector<BookmarkNode>& other_items,
    std::wstring_view folder_id,
    std::wstring_view folder_guid) {
  if (folder_id == L"__bookmark_bar__") {
    return &bar_items;
  }
  if (folder_id == L"__other__") {
    return &other_items;
  }
  if (BookmarkNode* hit = FindNodeInTree(bar_items, folder_id, folder_guid);
      hit && hit->is_folder) {
    return &hit->children;
  }
  if (BookmarkNode* hit = FindNodeInTree(other_items, folder_id, folder_guid);
      hit && hit->is_folder) {
    return &hit->children;
  }
  return nullptr;
}

void InsertNodeAtAnchor(std::vector<BookmarkNode>& list,
                        BookmarkNode node,
                        std::wstring_view anchor_id,
                        std::wstring_view anchor_guid,
                        bool insert_after) {
  size_t pos = list.size();
  if (!anchor_id.empty() || !anchor_guid.empty()) {
    for (size_t i = 0; i < list.size(); ++i) {
      if (MatchesNodeIdOrGuid(list[i], anchor_id, anchor_guid)) {
        pos = insert_after ? (i + 1) : i;
        break;
      }
    }
  }
  list.insert(list.begin() + static_cast<ptrdiff_t>(pos), std::move(node));
}

void ApplyOpsToInMemoryTree(std::vector<BookmarkNode>& bar_items,
                            std::vector<BookmarkNode>& other_items,
                            BookmarkSource default_source,
                            const std::vector<BookmarkOp>& ops) {
  for (const auto& op : ops) {
    const std::wstring id_w = Utf8ToWide(op.id);
    const std::wstring guid_w = Utf8ToWide(op.guid);
    const std::wstring dst_id_w = Utf8ToWide(op.dst_folder_id);
    const std::wstring dst_guid_w = Utf8ToWide(op.dst_folder_guid);
    const std::wstring anc_id_w = Utf8ToWide(op.anchor_id);
    const std::wstring anc_guid_w = Utf8ToWide(op.anchor_guid);

    switch (op.kind) {
      case BookmarkOp::Kind::kAdd: {
        if (TreeContainsNode(bar_items, id_w, guid_w) ||
            TreeContainsNode(other_items, id_w, guid_w)) {
          break;
        }
        auto* dst = ResolveTreeFolderChildren(bar_items, other_items, dst_id_w,
                                              dst_guid_w);
        if (!dst) {
          break;
        }
        BookmarkNode node;
        node.id = id_w;
        node.guid = guid_w;
        node.is_folder = op.is_folder;
        node.title = Utf8ToWide(op.title);
        node.url = Utf8ToWide(op.url);
        node.source = op.dst_source == BookmarkSource::kAccount
                          ? BookmarkSource::kAccount
                          : default_source;
        InsertNodeAtAnchor(*dst, std::move(node), anc_id_w, anc_guid_w,
                           op.insert_after);
        break;
      }
      case BookmarkOp::Kind::kUpdate: {
        BookmarkNode* hit = FindNodeInTree(bar_items, id_w, guid_w);
        if (!hit) {
          hit = FindNodeInTree(other_items, id_w, guid_w);
        }
        if (hit) {
          hit->title = Utf8ToWide(op.title);
          if (op.has_url && !hit->is_folder) {
            hit->url = Utf8ToWide(op.url);
          }
        }
        break;
      }
      case BookmarkOp::Kind::kDelete: {
        BookmarkNode dummy;
        if (!ExtractNodeFromTree(bar_items, id_w, guid_w, dummy)) {
          ExtractNodeFromTree(other_items, id_w, guid_w, dummy);
        }
        break;
      }
      case BookmarkOp::Kind::kMove: {
        if (dst_id_w == id_w && !id_w.empty()) {
          break;
        }
        if (!dst_guid_w.empty() && dst_guid_w == guid_w) {
          break;
        }
        BookmarkNode moved;
        if (!ExtractNodeFromTree(bar_items, id_w, guid_w, moved) &&
            !ExtractNodeFromTree(other_items, id_w, guid_w, moved)) {
          break;
        }
        if (moved.is_folder &&
            TreeContainsNode(moved.children, dst_id_w, dst_guid_w)) {
          // Cycle guard: put back at end of bar_items.
          bar_items.push_back(std::move(moved));
          break;
        }
        auto* dst = ResolveTreeFolderChildren(bar_items, other_items, dst_id_w,
                                              dst_guid_w);
        if (!dst) {
          bar_items.push_back(std::move(moved));
          break;
        }
        InsertNodeAtAnchor(*dst, std::move(moved), anc_id_w, anc_guid_w,
                           op.insert_after);
        break;
      }
    }
  }
}

// --- On-disk Chrome JSON AST replay helpers ---

bool AstContainsIdOrGuid(const JsonValue& node,
                         std::string_view id,
                         std::string_view guid) {
  if (MatchesAstIdOrGuid(node, id, guid)) {
    return true;
  }
  if (const JsonValue* children = node.Find("children");
      children && children->type == JsonValue::Type::kArray) {
    for (const auto& child : children->arr_val) {
      if (AstContainsIdOrGuid(child, id, guid)) {
        return true;
      }
    }
  }
  return false;
}

JsonValue* FindAstFolder(JsonValue& node,
                         std::string_view id,
                         std::string_view guid) {
  if (node.type != JsonValue::Type::kObject) {
    return nullptr;
  }
  if (node.GetString("type") == "folder" &&
      MatchesAstIdOrGuid(node, id, guid)) {
    return &node;
  }
  if (JsonValue* children = node.Find("children");
      children && children->type == JsonValue::Type::kArray) {
    for (auto& child : children->arr_val) {
      if (JsonValue* hit = FindAstFolder(child, id, guid)) {
        return hit;
      }
    }
  }
  return nullptr;
}

JsonValue* ResolveAstFolder(JsonValue& doc,
                            std::string_view folder_id,
                            std::string_view folder_guid) {
  JsonValue* roots = doc.Find("roots");
  if (!roots || roots->type != JsonValue::Type::kObject) {
    return nullptr;
  }
  if (folder_id == "__bookmark_bar__") {
    return roots->Find("bookmark_bar");
  }
  if (folder_id == "__other__") {
    return roots->Find("other");
  }
  for (const char* key : {"bookmark_bar", "other", "synced"}) {
    if (JsonValue* r = roots->Find(key)) {
      if (JsonValue* hit = FindAstFolder(*r, folder_id, folder_guid)) {
        return hit;
      }
    }
  }
  return nullptr;
}

JsonValue* FindAstNodeInDoc(JsonValue& doc,
                            std::string_view id,
                            std::string_view guid,
                            JsonValue** out_parent) {
  auto walk = [&](auto&& self, JsonValue& parent) -> JsonValue* {
    JsonValue* children = parent.Find("children");
    if (!children || children->type != JsonValue::Type::kArray) {
      return nullptr;
    }
    for (auto& child : children->arr_val) {
      if (MatchesAstIdOrGuid(child, id, guid)) {
        if (out_parent) {
          *out_parent = &parent;
        }
        return &child;
      }
      if (child.GetString("type") == "folder") {
        if (JsonValue* hit = self(self, child)) {
          return hit;
        }
      }
    }
    return nullptr;
  };
  JsonValue* roots = doc.Find("roots");
  if (!roots || roots->type != JsonValue::Type::kObject) {
    return nullptr;
  }
  for (const char* key : {"bookmark_bar", "other", "synced"}) {
    if (JsonValue* r = roots->Find(key)) {
      if (JsonValue* hit = walk(walk, *r)) {
        return hit;
      }
    }
  }
  return nullptr;
}

bool ExtractAstNodeFromDoc(JsonValue& doc,
                           std::string_view id,
                           std::string_view guid,
                           JsonValue& extracted_out) {
  auto walk = [&](auto&& self, JsonValue& parent) -> bool {
    JsonValue* children = parent.Find("children");
    if (!children || children->type != JsonValue::Type::kArray) {
      return false;
    }
    for (auto it = children->arr_val.begin(); it != children->arr_val.end();
         ++it) {
      if (MatchesAstIdOrGuid(*it, id, guid)) {
        extracted_out = std::move(*it);
        children->arr_val.erase(it);
        parent.SetString("date_modified",
                         std::to_string(GetChromeWebKitTimestamp()));
        return true;
      }
      if (it->GetString("type") == "folder" && self(self, *it)) {
        return true;
      }
    }
    return false;
  };
  JsonValue* roots = doc.Find("roots");
  if (!roots || roots->type != JsonValue::Type::kObject) {
    return false;
  }
  for (const char* key : {"bookmark_bar", "other", "synced"}) {
    if (JsonValue* r = roots->Find(key)) {
      if (walk(walk, *r)) {
        return true;
      }
    }
  }
  return false;
}

bool ApplyOpsToChromeFile(const std::wstring& path,
                          BookmarkSource file_source,
                          bool has_other_file,
                          const std::vector<BookmarkOp>& ops) {
  if (ops.empty()) {
    return true;
  }
  const auto text = ReadFileSharedUtf8(path, 64 * 1024 * 1024);
  if (!text) {
    return false;
  }
  JsonValue doc;
  AstJsonParser parser(*text);
  if (!parser.Parse(doc)) {
    return false;
  }

  bool modified = false;
  const std::string now_ts = std::to_string(GetChromeWebKitTimestamp());

  for (const auto& op : ops) {
    switch (op.kind) {
      case BookmarkOp::Kind::kAdd: {
        if (has_other_file &&
            (op.dst_folder_id == "__bookmark_bar__" ||
             op.dst_folder_id == "__other__") &&
            op.dst_source != file_source) {
          break;
        }
        if (FindAstNodeInDoc(doc, op.id, op.guid, nullptr)) {
          break;
        }
        JsonValue* dst_folder =
            ResolveAstFolder(doc, op.dst_folder_id, op.dst_folder_guid);
        if (!dst_folder) {
          break;
        }
        JsonValue* dst_children = dst_folder->Find("children");
        if (!dst_children) {
          JsonValue arr;
          arr.type = JsonValue::Type::kArray;
          dst_folder->obj_val.emplace_back("children", std::move(arr));
          dst_children = dst_folder->Find("children");
        }
        if (!dst_children || dst_children->type != JsonValue::Type::kArray) {
          break;
        }

        uint64_t max_id = 0;
        if (const JsonValue* roots = doc.Find("roots")) {
          for (const char* key : {"bookmark_bar", "other", "synced"}) {
            if (const JsonValue* r = roots->Find(key)) {
              FindMaxNumericId(*r, max_id);
            }
          }
        }
        JsonValue new_node;
        new_node.type = JsonValue::Type::kObject;
        if (op.is_folder) {
          JsonValue empty_arr;
          empty_arr.type = JsonValue::Type::kArray;
          new_node.obj_val.emplace_back("children", std::move(empty_arr));
          new_node.SetString("date_added", now_ts);
          new_node.SetString("date_last_used", "0");
          new_node.SetString("date_modified", now_ts);
          new_node.SetString("guid", op.guid);
          new_node.SetString("id", std::to_string(max_id + 1));
          new_node.SetString("name", op.title);
          new_node.SetString("type", "folder");
        } else {
          new_node.SetString("date_added", now_ts);
          new_node.SetString("date_last_used", "0");
          new_node.SetString("guid", op.guid);
          new_node.SetString("id", std::to_string(max_id + 1));
          new_node.SetString("name", op.title.empty() ? op.url : op.title);
          new_node.SetString("type", "url");
          new_node.SetString("url", op.url);
        }

        size_t pos = dst_children->arr_val.size();
        if (!op.anchor_id.empty() || !op.anchor_guid.empty()) {
          for (size_t i = 0; i < dst_children->arr_val.size(); ++i) {
            if (MatchesAstIdOrGuid(dst_children->arr_val[i], op.anchor_id,
                                   op.anchor_guid)) {
              pos = op.insert_after ? (i + 1) : i;
              break;
            }
          }
        }
        dst_children->arr_val.insert(
            dst_children->arr_val.begin() + static_cast<ptrdiff_t>(pos),
            std::move(new_node));
        dst_folder->SetString("date_modified", now_ts);
        modified = true;
        break;
      }
      case BookmarkOp::Kind::kUpdate: {
        JsonValue* parent = nullptr;
        JsonValue* node = FindAstNodeInDoc(doc, op.id, op.guid, &parent);
        if (!node) {
          break;
        }
        node->SetString("name", op.title);
        if (op.has_url && node->GetString("type") == "url") {
          node->SetString("url", op.url);
        }
        if (node->GetString("type") == "folder") {
          node->SetString("date_modified", now_ts);
        }
        if (parent) {
          parent->SetString("date_modified", now_ts);
        }
        modified = true;
        break;
      }
      case BookmarkOp::Kind::kDelete: {
        JsonValue removed;
        if (ExtractAstNodeFromDoc(doc, op.id, op.guid, removed)) {
          modified = true;
        }
        break;
      }
      case BookmarkOp::Kind::kMove: {
        JsonValue* dst_folder =
            ResolveAstFolder(doc, op.dst_folder_id, op.dst_folder_guid);
        if (!dst_folder) {
          break;
        }
        JsonValue moved;
        if (!ExtractAstNodeFromDoc(doc, op.id, op.guid, moved)) {
          break;
        }
        if (AstContainsIdOrGuid(moved, op.dst_folder_id, op.dst_folder_guid)) {
          break;
        }
        dst_folder =
            ResolveAstFolder(doc, op.dst_folder_id, op.dst_folder_guid);
        if (!dst_folder) {
          break;
        }
        JsonValue* dst_children = dst_folder->Find("children");
        if (!dst_children) {
          JsonValue arr;
          arr.type = JsonValue::Type::kArray;
          dst_folder->obj_val.emplace_back("children", std::move(arr));
          dst_children = dst_folder->Find("children");
        }
        if (!dst_children || dst_children->type != JsonValue::Type::kArray) {
          break;
        }
        size_t pos = dst_children->arr_val.size();
        if (!op.anchor_id.empty() || !op.anchor_guid.empty()) {
          for (size_t i = 0; i < dst_children->arr_val.size(); ++i) {
            if (MatchesAstIdOrGuid(dst_children->arr_val[i], op.anchor_id,
                                   op.anchor_guid)) {
              pos = op.insert_after ? (i + 1) : i;
              break;
            }
          }
        }
        dst_children->arr_val.insert(
            dst_children->arr_val.begin() + static_cast<ptrdiff_t>(pos),
            std::move(moved));
        dst_folder->SetString("date_modified", now_ts);
        modified = true;
        break;
      }
    }
  }

  if (!modified) {
    return true;
  }
  return SaveBookmarkDoc(path, doc);
}

void FlushJournalToProfileFiles(const std::wstring& profile_dir,
                                const std::vector<BookmarkOp>& ops) {
  if (profile_dir.empty() || ops.empty()) {
    return;
  }
  const std::wstring account_path = profile_dir + L"\\AccountBookmarks";
  const std::wstring local_path = profile_dir + L"\\Bookmarks";
  const bool has_account = GetFileStamp(account_path).has_value();
  const bool has_local = GetFileStamp(local_path).has_value();
  if (has_account) {
    ApplyOpsToChromeFile(account_path, BookmarkSource::kAccount, has_local,
                         ops);
  }
  if (has_local) {
    ApplyOpsToChromeFile(local_path, BookmarkSource::kLocal, has_account, ops);
  }
}

}  // namespace

BookmarkModel& BookmarkModel::Instance() {
  static BookmarkModel instance;
  return instance;
}

bool BookmarkModel::EnsureLoaded(bool show_other_bookmarks, bool force) {
  auto& store = GetStore();
  std::wstring tried;
  const auto profile_dir = FindBestActiveProfileDir(&tried);
  if (!profile_dir) {
    std::lock_guard lock(store.mutex);
    store.last_diag =
        tried.empty() ? L"未找到 Chrome 数据目录" : (L"未找到书签文件: " + tried);
    return false;
  }

  const CombinedBookmarkStamp stamp = GetProfileBookmarkStamp(*profile_dir);
  if (!stamp.HasAny()) {
    std::lock_guard lock(store.mutex);
    store.last_diag = L"未找到书签文件: " + *profile_dir;
    return false;
  }

  if (!force) {
    std::lock_guard lock(store.mutex);
    if (store.profile_dir == *profile_dir &&
        store.bookmarks_stamp == stamp &&
        store.last_show_other == show_other_bookmarks) {
      return false;
    }
  }

  auto items = std::make_shared<std::vector<BookmarkNode>>();
  std::vector<BookmarkNode> merged_other;
  std::vector<BookmarkNode> merged_synced;
  std::wstring other_title = L"其他书签";
  BookmarkSource default_source = BookmarkSource::kLocal;
  BookmarkSource other_default_source = BookmarkSource::kLocal;
  bool any_parsed = false;

  auto load_one_file = [&](const std::wstring& path, BookmarkSource src) {
    const auto json = ReadFileSharedUtf8(path, 64 * 1024 * 1024);
    if (!json) {
      return;
    }
    ParsedBookmarksRoots roots;
    if (!ParseBookmarksJson(*json, roots)) {
      return;
    }
    any_parsed = true;
    TagNodeSourceRecursive(roots.bookmark_bar, src);
    TagNodeSourceRecursive(roots.other, src);
    TagNodeSourceRecursive(roots.synced, src);

    if (!roots.bookmark_bar.children.empty()) {
      default_source = src;
    }
    for (auto& n : roots.bookmark_bar.children) {
      items->push_back(std::move(n));
    }
    if (!roots.other.title.empty()) {
      other_title = roots.other.title;
    }
    if (!roots.other.children.empty()) {
      other_default_source = src;
    }
    for (auto& n : roots.other.children) {
      merged_other.push_back(std::move(n));
    }
    for (auto& n : roots.synced.children) {
      merged_synced.push_back(std::move(n));
    }
  };

  // Load AccountBookmarks first (Chrome 130+ signed-in account storage), then local Bookmarks.
  if (stamp.account_stamp) {
    load_one_file(*profile_dir + L"\\AccountBookmarks",
                  BookmarkSource::kAccount);
  }
  if (stamp.local_stamp) {
    load_one_file(*profile_dir + L"\\Bookmarks", BookmarkSource::kLocal);
  }

  if (!any_parsed) {
    std::lock_guard lock(store.mutex);
    store.last_diag = L"书签 JSON 解析失败: " + *profile_dir;
    return false;
  }

  // Always replay Chrome++'s durable edit journal on top of Chrome's files so
  // Chrome's in-memory BookmarkStorage / Sync never reverts user edits.
  const std::vector<BookmarkOp> journal_ops = LoadJournalFromDisk(*profile_dir);
  if (!journal_ops.empty()) {
    ApplyOpsToInMemoryTree(*items, merged_other, default_source, journal_ops);
  }

  if (show_other_bookmarks && !merged_other.empty()) {
    BookmarkNode other_folder;
    other_folder.id = L"__other__";
    other_folder.title = other_title;
    other_folder.is_folder = true;
    other_folder.is_virtual_root = true;
    other_folder.source = other_default_source;
    other_folder.children = std::move(merged_other);
    items->push_back(std::move(other_folder));
  } else if (items->empty() && !merged_other.empty()) {
    *items = std::move(merged_other);
  }
  if (items->empty() && !merged_synced.empty()) {
    *items = std::move(merged_synced);
  }

  std::lock_guard lock(store.mutex);
  store.profile_dir = *profile_dir;
  store.bookmarks_stamp = stamp;
  store.last_show_other = show_other_bookmarks;
  store.last_diag = items->empty()
                        ? (L"书签栏为空: " + *profile_dir)
                        : (L"已加载: " + *profile_dir);
  store.root_items = std::move(items);
  return true;
}

bool BookmarkModel::MoveBookmark(BookmarkSource /*src_source*/,
                                 const std::wstring& src_id,
                                 const std::wstring& src_guid,
                                 BookmarkSource /*dst_source*/,
                                 const std::wstring& dst_folder_id,
                                 const std::wstring& dst_folder_guid,
                                 const std::wstring& anchor_id,
                                 const std::wstring& anchor_guid,
                                 bool insert_after,
                                 bool show_other_bookmarks) {
  if ((src_id.empty() && src_guid.empty()) || src_id == L"__other__" ||
      src_id == L"__empty__") {
    return false;
  }

  std::wstring profile_dir;
  {
    auto& store = GetStore();
    std::lock_guard lock(store.mutex);
    profile_dir = store.profile_dir;
  }
  if (profile_dir.empty()) {
    return false;
  }

  BookmarkOp op;
  op.kind = BookmarkOp::Kind::kMove;
  op.id = WideToUtf8(src_id);
  op.guid = WideToUtf8(src_guid);
  op.dst_folder_id = WideToUtf8(dst_folder_id);
  op.dst_folder_guid = WideToUtf8(dst_folder_guid);
  op.anchor_id = WideToUtf8(anchor_id);
  op.anchor_guid = WideToUtf8(anchor_guid);
  op.insert_after = insert_after;

  auto ops = LoadJournalFromDisk(profile_dir);
  AppendAndCompactOp(ops, std::move(op));
  if (!SaveJournalToDisk(profile_dir, ops)) {
    return false;
  }

  // Best-effort mirror to Chrome's own Bookmarks/AccountBookmarks files.
  FlushJournalToProfileFiles(profile_dir, ops);
  EnsureLoaded(show_other_bookmarks, /*force=*/true);
  return true;
}

bool BookmarkModel::UpdateBookmark(BookmarkSource /*source*/,
                                   const std::wstring& node_id,
                                   const std::wstring& node_guid,
                                   const std::wstring& new_title,
                                   const std::optional<std::wstring>& new_url,
                                   bool show_other_bookmarks) {
  if ((node_id.empty() && node_guid.empty()) || node_id == L"__other__" ||
      node_id == L"__empty__") {
    return false;
  }

  std::wstring profile_dir;
  {
    auto& store = GetStore();
    std::lock_guard lock(store.mutex);
    profile_dir = store.profile_dir;
  }
  if (profile_dir.empty()) {
    return false;
  }

  BookmarkOp op;
  op.kind = BookmarkOp::Kind::kUpdate;
  op.id = WideToUtf8(node_id);
  op.guid = WideToUtf8(node_guid);
  op.title = WideToUtf8(new_title);
  op.has_url = new_url.has_value();
  if (new_url.has_value()) {
    op.url = WideToUtf8(*new_url);
  }

  auto ops = LoadJournalFromDisk(profile_dir);
  AppendAndCompactOp(ops, std::move(op));
  if (!SaveJournalToDisk(profile_dir, ops)) {
    return false;
  }

  FlushJournalToProfileFiles(profile_dir, ops);
  EnsureLoaded(show_other_bookmarks, /*force=*/true);
  return true;
}

bool BookmarkModel::AddBookmarkOrFolder(BookmarkSource dst_source,
                                        const std::wstring& dst_folder_id,
                                        const std::wstring& dst_folder_guid,
                                        const std::wstring& anchor_id,
                                        const std::wstring& anchor_guid,
                                        bool insert_after,
                                        bool is_folder,
                                        const std::wstring& title,
                                        const std::wstring& url,
                                        bool show_other_bookmarks) {
  if (!is_folder && url.empty()) {
    return false;
  }

  std::wstring profile_dir;
  {
    auto& store = GetStore();
    std::lock_guard lock(store.mutex);
    profile_dir = store.profile_dir;
  }
  if (profile_dir.empty()) {
    return false;
  }

  static std::atomic<uint32_t> add_seq{0};
  const std::string new_guid = GenerateUuidV4();
  const std::string new_id =
      "cp_" + std::to_string(::GetTickCount64()) + "_" +
      std::to_string(++add_seq);

  BookmarkOp op;
  op.kind = BookmarkOp::Kind::kAdd;
  op.dst_source = dst_source;
  op.id = new_id;
  op.guid = new_guid;
  op.dst_folder_id = WideToUtf8(dst_folder_id);
  op.dst_folder_guid = WideToUtf8(dst_folder_guid);
  op.anchor_id = WideToUtf8(anchor_id);
  op.anchor_guid = WideToUtf8(anchor_guid);
  op.insert_after = insert_after;
  op.is_folder = is_folder;
  op.title = WideToUtf8(title.empty() ? url : title);
  op.url = WideToUtf8(url);

  auto ops = LoadJournalFromDisk(profile_dir);
  AppendAndCompactOp(ops, std::move(op));
  if (!SaveJournalToDisk(profile_dir, ops)) {
    return false;
  }

  FlushJournalToProfileFiles(profile_dir, ops);
  EnsureLoaded(show_other_bookmarks, /*force=*/true);
  return true;
}

bool BookmarkModel::DeleteBookmark(BookmarkSource /*source*/,
                                   const std::wstring& node_id,
                                   const std::wstring& node_guid,
                                   bool show_other_bookmarks) {
  if ((node_id.empty() && node_guid.empty()) || node_id == L"__other__" ||
      node_id == L"__empty__") {
    return false;
  }

  std::wstring profile_dir;
  {
    auto& store = GetStore();
    std::lock_guard lock(store.mutex);
    profile_dir = store.profile_dir;
  }
  if (profile_dir.empty()) {
    return false;
  }

  BookmarkOp op;
  op.kind = BookmarkOp::Kind::kDelete;
  op.id = WideToUtf8(node_id);
  op.guid = WideToUtf8(node_guid);

  auto ops = LoadJournalFromDisk(profile_dir);
  AppendAndCompactOp(ops, std::move(op));
  if (!SaveJournalToDisk(profile_dir, ops)) {
    return false;
  }

  FlushJournalToProfileFiles(profile_dir, ops);
  EnsureLoaded(show_other_bookmarks, /*force=*/true);
  return true;
}

void BookmarkModel::FlushPendingOpsToChromeFiles() {
  std::wstring profile_dir;
  {
    auto& store = GetStore();
    std::lock_guard lock(store.mutex);
    profile_dir = store.profile_dir;
  }
  if (profile_dir.empty()) {
    return;
  }
  const auto ops = LoadJournalFromDisk(profile_dir);
  if (!ops.empty()) {
    FlushJournalToProfileFiles(profile_dir, ops);
  }
}

std::wstring BookmarkModel::GetDiagnosticInfo() const {
  auto& store = GetStore();
  std::lock_guard lock(store.mutex);
  return store.last_diag;
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
