#ifndef CHROME_PLUS_SRC_SETTINGS_SERVER_H_
#define CHROME_PLUS_SRC_SETTINGS_SERVER_H_

#include <string>

// Starts the local loopback HTTP server used by the Chrome++ settings panel.
// Must be called in the browser process before PakPatch().
void StartSettingsServer();

// Returns the session-scoped loopback URL (http://127.0.0.1:<port>/?token=...)
// or an empty string if the server is not running.
const std::string& GetSettingsServerUrl();

#endif  // CHROME_PLUS_SRC_SETTINGS_SERVER_H_
