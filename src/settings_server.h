#ifndef CHROME_PLUS_SRC_SETTINGS_SERVER_H_
#define CHROME_PLUS_SRC_SETTINGS_SERVER_H_

#include <string>
#include <string_view>

// Starts the local loopback HTTP server used by the Chrome++ settings panel.
// Must be called in the browser process.
void StartSettingsServer();

// Returns the fixed loopback URL (http://127.0.0.1:19876/) for the settings UI.
constexpr std::string_view GetSettingsServerUrl() {
  return "http://127.0.0.1:19876/";
}

// Returns the per-session random auth token for loopback API requests.
std::string GetSettingsSessionToken();

// Returns true once the local loopback server is bound and listening.
bool IsSettingsServerReady();

#endif  // CHROME_PLUS_SRC_SETTINGS_SERVER_H_
