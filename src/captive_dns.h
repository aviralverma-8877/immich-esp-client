#pragma once

#include <stddef.h>
#include <stdint.h>

// Builds the reply to one DNS query packet `q` (n bytes) into `r` (512 bytes)
// and returns its length, or 0 to stay silent. Pure, so it can be tested on
// a PC.
size_t captiveDnsReply(const uint8_t* q, size_t n, const uint8_t ip[4], uint8_t* r);

#ifdef ARDUINO
#include <WiFiUdp.h>

// Minimal DNS responder for the setup access point: answers every A query
// with the AP's address so phones open their captive-portal sheet.
//
// Replaces the core's DNSServer, which in Arduino-ESP32 2.0.x rejects any
// query carrying an additional record. Android and iOS add an EDNS0 OPT
// record to every query, so they got errors back and never saw the portal.
class CaptiveDns {
 public:
  bool start(IPAddress ip);
  void stop();
  void process();  // call often; handles all queued queries

 private:
  WiFiUDP udp_;
  IPAddress ip_;
  bool running_ = false;
};
#endif
