#pragma once

#include <stdint.h>

namespace portal {

// Starts the web server in its own task. With apMode, also runs a captive
// DNS server that resolves every name to the access point.
void begin(bool apMode);

// Switches captive DNS off once the station link is up and the AP is closed.
void setApMode(bool apMode);

// True while the setup page is testing new WiFi credentials; the main loop
// must leave the station interface alone until the device restarts.
bool setupActive();

// millis() of the last request from a browser; the frame stays online while
// the portal is in use.
uint32_t lastActivity();

}  // namespace portal
