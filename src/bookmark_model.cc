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

struct CombinedBookmarkStamp {
  std::optional<FileStamp> local_stamp;
  std::optional<FileStamp> account_stamp;

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

bool AstContainsNodeId(const JsonValue& node, std::string_view target_id) {
  if (node.type != JsonValue::Type::kObject) {
    return false;
  }
  if (node.GetString("id") == target_id) {
    return true;
  }
  if (const JsonValue* children = node.Find("children");
      children && children->type == JsonValue::Type::kArray) {
    for (const auto& child : children->arr_val) {
      if (AstContainsNodeId(child, target_id)) {
        return true;
      }
    }
  }
  return false;
}

JsonValue* FindFolderObjectById(JsonValue& node, std::string_view folder_id) {
  if (node.type != JsonValue::Type::kObject) {
    return nullptr;
  }
  if (node.GetString("id") == folder_id &&
      node.GetString("type") == "folder") {
    return &node;
  }
  if (JsonValue* children = node.Find("children");
      children && children->type == JsonValue::Type::kArray) {
    for (auto& child : children->arr_val) {
      if (JsonValue* hit = FindFolderObjectById(child, folder_id)) {
        return hit;
      }
    }
  }
  return nullptr;
}

JsonValue* ResolveDestinationFolder(JsonValue& doc,
                                    std::string_view dst_folder_id) {
  JsonValue* roots = doc.Find("roots");
  if (!roots || roots->type != JsonValue::Type::kObject) {
    return nullptr;
  }
  if (dst_folder_id == "__bookmark_bar__") {
    return roots->Find("bookmark_bar");
  }
  if (dst_folder_id == "__other__") {
    return roots->Find("other");
  }
  for (const char* key : {"bookmark_bar", "other", "synced"}) {
    if (JsonValue* r = roots->Find(key)) {
      if (JsonValue* hit = FindFolderObjectById(*r, dst_folder_id)) {
        return hit;
      }
    }
  }
  return nullptr;
}

bool ExtractNodeById(JsonValue& folder,
                     std::string_view target_id,
                     JsonValue& extracted_out) {
  JsonValue* children = folder.Find("children");
  if (!children || children->type != JsonValue::Type::kArray) {
    return false;
  }
  for (auto it = children->arr_val.begin(); it != children->arr_val.end();
       ++it) {
    if (it->GetString("id") == target_id) {
      extracted_out = std::move(*it);
      children->arr_val.erase(it);
      folder.SetString("date_modified",
                       std::to_string(GetChromeWebKitTimestamp()));
      return true;
    }
    if (it->GetString("type") == "folder" &&
        ExtractNodeById(*it, target_id, extracted_out)) {
      return true;
    }
  }
  return false;
}

bool ExtractNodeFromDoc(JsonValue& doc,
                        std::string_view target_id,
                        JsonValue& extracted_out) {
  JsonValue* roots = doc.Find("roots");
  if (!roots || roots->type != JsonValue::Type::kObject) {
    return false;
  }
  for (const char* key : {"bookmark_bar", "other", "synced"}) {
    if (JsonValue* r = roots->Find(key)) {
      if (ExtractNodeById(*r, target_id, extracted_out)) {
        return true;
      }
    }
  }
  return false;
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

void ReassignSubtreeIds(JsonValue& node, uint64_t& next_id) {
  if (node.type != JsonValue::Type::kObject) {
    return;
  }
  node.SetString("id", std::to_string(++next_id));
  if (JsonValue* children = node.Find("children");
      children && children->type == JsonValue::Type::kArray) {
    for (auto& child : children->arr_val) {
      ReassignSubtreeIds(child, next_id);
    }
  }
}

bool WriteFileAtomicUtf8(const std::wstring& path, std::string_view content) {
  const std::wstring tmp_path =
      path + L".cpp_tmp_" + std::to_wstring(::GetCurrentProcessId());
  HANDLE file = ::CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  const bool ok =
      ::WriteFile(file, content.data(), static_cast<DWORD>(content.size()),
                  &written, nullptr) &&
      written == content.size();
  ::CloseHandle(file);
  if (!ok) {
    ::DeleteFileW(tmp_path.c_str());
    return false;
  }
  if (!::MoveFileExW(tmp_path.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    ::DeleteFileW(tmp_path.c_str());
    return false;
  }
  return true;
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

std::wstring BookmarkSourcePath(const std::wstring& profile_dir,
                                BookmarkSource source) {
  return profile_dir +
         (source == BookmarkSource::kAccount ? L"\\AccountBookmarks"
                                             : L"\\Bookmarks");
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

JsonValue* FindNodeByIdRecursive(JsonValue& node,
                                 std::string_view target_id,
                                 JsonValue** out_parent) {
  if (node.type != JsonValue::Type::kObject) {
    return nullptr;
  }
  if (JsonValue* children = node.Find("children");
      children && children->type == JsonValue::Type::kArray) {
    for (auto& child : children->arr_val) {
      if (child.GetString("id") == target_id) {
        if (out_parent) {
          *out_parent = &node;
        }
        return &child;
      }
      if (child.GetString("type") == "folder") {
        if (JsonValue* hit =
                FindNodeByIdRecursive(child, target_id, out_parent)) {
          return hit;
        }
      }
    }
  }
  return nullptr;
}

JsonValue* FindNodeByIdInDoc(JsonValue& doc,
                             std::string_view target_id,
                             JsonValue** out_parent) {
  JsonValue* roots = doc.Find("roots");
  if (!roots || roots->type != JsonValue::Type::kObject) {
    return nullptr;
  }
  for (const char* key : {"bookmark_bar", "other", "synced"}) {
    if (JsonValue* r = roots->Find(key)) {
      if (JsonValue* hit = FindNodeByIdRecursive(*r, target_id, out_parent)) {
        return hit;
      }
    }
  }
  return nullptr;
}

}  // namespace

BookmarkModel& BookmarkModel::Instance() {
  static BookmarkModel instance;
  return instance;
}

bool BookmarkModel::EnsureLoaded(bool show_other_bookmarks) {
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

  {
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

bool BookmarkModel::MoveBookmark(BookmarkSource src_source,
                                 const std::wstring& src_id,
                                 BookmarkSource dst_source,
                                 const std::wstring& dst_folder_id,
                                 const std::wstring& anchor_id,
                                 bool insert_after,
                                 bool show_other_bookmarks) {
  if (src_id.empty() || src_id == L"__other__" || src_id == L"__empty__") {
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

  const std::string src_id_u8 = WideToUtf8(src_id);
  const std::string dst_folder_u8 = WideToUtf8(dst_folder_id);
  const std::string anchor_u8 = WideToUtf8(anchor_id);

  const std::wstring src_path = BookmarkSourcePath(profile_dir, src_source);
  const auto src_text = ReadFileSharedUtf8(src_path, 64 * 1024 * 1024);
  if (!src_text) {
    return false;
  }
  JsonValue src_doc;
  AstJsonParser src_parser(*src_text);
  if (!src_parser.Parse(src_doc)) {
    return false;
  }

  const bool same_file = (src_source == dst_source);
  JsonValue dst_doc_separate;
  if (!same_file) {
    const std::wstring dst_path = BookmarkSourcePath(profile_dir, dst_source);
    const auto dst_text = ReadFileSharedUtf8(dst_path, 64 * 1024 * 1024);
    if (!dst_text) {
      return false;
    }
    AstJsonParser dst_parser(*dst_text);
    if (!dst_parser.Parse(dst_doc_separate)) {
      return false;
    }
  }
  JsonValue& dst_doc = same_file ? src_doc : dst_doc_separate;

  // Verify destination folder exists before modifying anything.
  if (!ResolveDestinationFolder(dst_doc, dst_folder_u8)) {
    return false;
  }

  // Prevent moving a folder into itself or its own descendant.
  if (same_file && dst_folder_u8 == src_id_u8) {
    return false;
  }

  JsonValue moved_node;
  if (!ExtractNodeFromDoc(src_doc, src_id_u8, moved_node)) {
    return false;
  }

  if (same_file && AstContainsNodeId(moved_node, dst_folder_u8)) {
    return false;
  }

  JsonValue* dst_folder = ResolveDestinationFolder(dst_doc, dst_folder_u8);
  if (!dst_folder) {
    return false;
  }
  JsonValue* dst_children = dst_folder->Find("children");
  if (!dst_children) {
    JsonValue empty_arr;
    empty_arr.type = JsonValue::Type::kArray;
    dst_folder->obj_val.emplace_back("children", std::move(empty_arr));
    dst_children = dst_folder->Find("children");
  }
  if (!dst_children || dst_children->type != JsonValue::Type::kArray) {
    return false;
  }

  if (!same_file) {
    uint64_t max_id = 0;
    if (const JsonValue* roots = dst_doc.Find("roots")) {
      for (const char* key : {"bookmark_bar", "other", "synced"}) {
        if (const JsonValue* r = roots->Find(key)) {
          FindMaxNumericId(*r, max_id);
        }
      }
    }
    ReassignSubtreeIds(moved_node, max_id);
  }

  size_t insert_pos = dst_children->arr_val.size();
  if (!anchor_u8.empty()) {
    for (size_t i = 0; i < dst_children->arr_val.size(); ++i) {
      if (dst_children->arr_val[i].GetString("id") == anchor_u8) {
        insert_pos = insert_after ? (i + 1) : i;
        break;
      }
    }
  }

  dst_children->arr_val.insert(
      dst_children->arr_val.begin() + static_cast<ptrdiff_t>(insert_pos),
      std::move(moved_node));
  dst_folder->SetString("date_modified",
                        std::to_string(GetChromeWebKitTimestamp()));

  bool saved = SaveBookmarkDoc(src_path, src_doc);
  if (saved && !same_file) {
    const std::wstring dst_path = BookmarkSourcePath(profile_dir, dst_source);
    saved = SaveBookmarkDoc(dst_path, dst_doc);
  }
  if (!saved) {
    return false;
  }

  // Force immediate reload into memory.
  {
    auto& store = GetStore();
    std::lock_guard lock(store.mutex);
    store.bookmarks_stamp = CombinedBookmarkStamp{};
  }
  EnsureLoaded(show_other_bookmarks);
  return true;
}

bool BookmarkModel::UpdateBookmark(BookmarkSource source,
                                   const std::wstring& node_id,
                                   const std::wstring& new_title,
                                   const std::optional<std::wstring>& new_url,
                                   bool show_other_bookmarks) {
  if (node_id.empty() || node_id == L"__other__" || node_id == L"__empty__") {
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

  const std::wstring path = BookmarkSourcePath(profile_dir, source);
  const auto text = ReadFileSharedUtf8(path, 64 * 1024 * 1024);
  if (!text) {
    return false;
  }
  JsonValue doc;
  AstJsonParser parser(*text);
  if (!parser.Parse(doc)) {
    return false;
  }

  JsonValue* parent = nullptr;
  JsonValue* node = FindNodeByIdInDoc(doc, WideToUtf8(node_id), &parent);
  if (!node) {
    return false;
  }

  node->SetString("name", WideToUtf8(new_title));
  if (new_url.has_value() && node->GetString("type") == "url") {
    node->SetString("url", WideToUtf8(*new_url));
  }
  const std::string now_ts = std::to_string(GetChromeWebKitTimestamp());
  if (node->GetString("type") == "folder") {
    node->SetString("date_modified", now_ts);
  }
  if (parent) {
    parent->SetString("date_modified", now_ts);
  }

  if (!SaveBookmarkDoc(path, doc)) {
    return false;
  }

  {
    auto& store = GetStore();
    std::lock_guard lock(store.mutex);
    store.bookmarks_stamp = CombinedBookmarkStamp{};
  }
  EnsureLoaded(show_other_bookmarks);
  return true;
}

bool BookmarkModel::AddBookmarkOrFolder(BookmarkSource dst_source,
                                        const std::wstring& dst_folder_id,
                                        const std::wstring& anchor_id,
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

  const std::wstring path = BookmarkSourcePath(profile_dir, dst_source);
  const auto text = ReadFileSharedUtf8(path, 64 * 1024 * 1024);
  if (!text) {
    return false;
  }
  JsonValue doc;
  AstJsonParser parser(*text);
  if (!parser.Parse(doc)) {
    return false;
  }

  JsonValue* dst_folder =
      ResolveDestinationFolder(doc, WideToUtf8(dst_folder_id));
  if (!dst_folder) {
    return false;
  }
  JsonValue* dst_children = dst_folder->Find("children");
  if (!dst_children) {
    JsonValue empty_arr;
    empty_arr.type = JsonValue::Type::kArray;
    dst_folder->obj_val.emplace_back("children", std::move(empty_arr));
    dst_children = dst_folder->Find("children");
  }
  if (!dst_children || dst_children->type != JsonValue::Type::kArray) {
    return false;
  }

  uint64_t max_id = 0;
  if (const JsonValue* roots = doc.Find("roots")) {
    for (const char* key : {"bookmark_bar", "other", "synced"}) {
      if (const JsonValue* r = roots->Find(key)) {
        FindMaxNumericId(*r, max_id);
      }
    }
  }

  const std::string now_ts = std::to_string(GetChromeWebKitTimestamp());
  JsonValue new_node;
  new_node.type = JsonValue::Type::kObject;
  if (is_folder) {
    JsonValue empty_arr;
    empty_arr.type = JsonValue::Type::kArray;
    new_node.obj_val.emplace_back("children", std::move(empty_arr));
    new_node.SetString("date_added", now_ts);
    new_node.SetString("date_last_used", "0");
    new_node.SetString("date_modified", now_ts);
    new_node.SetString("guid", GenerateUuidV4());
    new_node.SetString("id", std::to_string(max_id + 1));
    new_node.SetString("name", WideToUtf8(title));
    new_node.SetString("type", "folder");
  } else {
    new_node.SetString("date_added", now_ts);
    new_node.SetString("date_last_used", "0");
    new_node.SetString("guid", GenerateUuidV4());
    new_node.SetString("id", std::to_string(max_id + 1));
    new_node.SetString("name", WideToUtf8(title.empty() ? url : title));
    new_node.SetString("type", "url");
    new_node.SetString("url", WideToUtf8(url));
  }

  const std::string anchor_u8 = WideToUtf8(anchor_id);
  size_t insert_pos = dst_children->arr_val.size();
  if (!anchor_u8.empty()) {
    for (size_t i = 0; i < dst_children->arr_val.size(); ++i) {
      if (dst_children->arr_val[i].GetString("id") == anchor_u8) {
        insert_pos = insert_after ? (i + 1) : i;
        break;
      }
    }
  }
  dst_children->arr_val.insert(
      dst_children->arr_val.begin() + static_cast<ptrdiff_t>(insert_pos),
      std::move(new_node));
  dst_folder->SetString("date_modified", now_ts);

  if (!SaveBookmarkDoc(path, doc)) {
    return false;
  }

  {
    auto& store = GetStore();
    std::lock_guard lock(store.mutex);
    store.bookmarks_stamp = CombinedBookmarkStamp{};
  }
  EnsureLoaded(show_other_bookmarks);
  return true;
}

bool BookmarkModel::DeleteBookmark(BookmarkSource source,
                                   const std::wstring& node_id,
                                   bool show_other_bookmarks) {
  if (node_id.empty() || node_id == L"__other__" || node_id == L"__empty__") {
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

  const std::wstring path = BookmarkSourcePath(profile_dir, source);
  const auto text = ReadFileSharedUtf8(path, 64 * 1024 * 1024);
  if (!text) {
    return false;
  }
  JsonValue doc;
  AstJsonParser parser(*text);
  if (!parser.Parse(doc)) {
    return false;
  }

  JsonValue removed;
  if (!ExtractNodeFromDoc(doc, WideToUtf8(node_id), removed)) {
    return false;
  }

  if (!SaveBookmarkDoc(path, doc)) {
    return false;
  }

  {
    auto& store = GetStore();
    std::lock_guard lock(store.mutex);
    store.bookmarks_stamp = CombinedBookmarkStamp{};
  }
  EnsureLoaded(show_other_bookmarks);
  return true;
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
