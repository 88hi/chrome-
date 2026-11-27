#ifndef CHROME_PLUS_SRC_SETTINGS_SERVER_H_
#define CHROME_PLUS_SRC_SETTINGS_SERVER_H_

#include <string_view>

// Starts the local loopback HTTP server used by the Chrome++ settings panel.
// Must be called in the browser process.
void StartSettingsServer();

// Returns the fixed loopback URL (http://127.0.0.1:48321/) for the settings UI.
constexpr std::string_view GetSettingsServerUrl() {
  return "http://127.0.0.1:48321/";
}

#endif  // CHROME_PLUS_SRC_SETTINGS_SERVER_H_
