#include "captive_dns.h"

#include <string.h>

namespace {

constexpr uint16_t kPort = 53;
constexpr size_t kMaxPacket = 512;
constexpr uint16_t kTypeA = 1, kTypeAny = 255;

uint16_t rd16(const uint8_t* p) { return (p[0] << 8) | p[1]; }
void wr16(uint8_t* p, uint16_t v) { p[0] = v >> 8, p[1] = v & 0xFF; }

}  // namespace

#ifdef ARDUINO
bool CaptiveDns::start(IPAddress ip) {
  ip_ = ip;
  running_ = udp_.begin(kPort);
  return running_;
}

void CaptiveDns::stop() {
  if (running_) udp_.stop();
  running_ = false;
}
#endif

size_t captiveDnsReply(const uint8_t* q, size_t n, const uint8_t ip[4], uint8_t* r) {
  if (n < 12 || n > kMaxPacket) return 0;
  // Queries only (QR = 0, opcode QUERY) with at least one question.
  if ((q[2] & 0x80) || ((q[2] >> 3) & 0x0F) != 0 || rd16(q + 4) == 0) return 0;

  // Walk the first question's name (plain labels; queries don't compress).
  size_t i = 12;
  while (i < n && q[i] != 0) {
    if (q[i] & 0xC0) break;
    i += q[i] + 1;
  }
  if (i + 5 > n || q[i] != 0) return 0;
  const size_t qEnd = i + 5;  // terminating zero, QTYPE, QCLASS
  const uint16_t qtype = rd16(q + i + 1);
  const bool answer = qtype == kTypeA || qtype == kTypeAny;

  // Reply: same id and question, no authority/additional records, and an
  // A record for A/ANY. Other types (AAAA, HTTPS...) get NOERROR with no
  // answer, which makes clients fall back to IPv4.
  memcpy(r, q, qEnd);
  r[2] = 0x80 | 0x04 | (q[2] & 0x01);  // QR, AA, copy RD
  r[3] = 0x00;                          // RA = 0, RCODE = NOERROR
  wr16(r + 4, 1);
  wr16(r + 6, answer ? 1 : 0);
  wr16(r + 8, 0);
  wr16(r + 10, 0);
  size_t len = qEnd;
  if (answer) {
    static const uint8_t kRecord[] = {0xC0, 0x0C,  // name: pointer to the question
                                      0x00, 0x01, 0x00, 0x01,  // A, IN
                                      0x00, 0x00, 0x00, 0x3C,  // TTL 60 s
                                      0x00, 0x04};             // 4-byte address
    memcpy(r + len, kRecord, sizeof(kRecord));
    len += sizeof(kRecord);
    for (int b = 0; b < 4; b++) r[len++] = ip[b];
  }
  return len;
}

#ifdef ARDUINO
void CaptiveDns::process() {
  if (!running_) return;
  const uint8_t ip[4] = {ip_[0], ip_[1], ip_[2], ip_[3]};
  for (int n = udp_.parsePacket(); n > 0; n = udp_.parsePacket()) {
    uint8_t q[kMaxPacket], r[kMaxPacket];
    if (n > (int)kMaxPacket) {
      udp_.flush();
      continue;
    }
    udp_.read(q, n);
    const size_t len = captiveDnsReply(q, n, ip, r);
#ifdef CAPTIVE_DEBUG
    char name[96] = "";
    for (size_t i = 12, o = 0; i < (size_t)n && q[i] && o < sizeof(name) - 65; i += q[i] + 1) {
      if (o) name[o++] = '.';
      for (int k = 1; k <= q[i] && i + k < (size_t)n; k++) name[o++] = q[i + k];
      name[o] = 0;
    }
    Serial.printf("[dns] %s from %s -> %s\n", name, udp_.remoteIP().toString().c_str(),
                  len > 0 && r[7] ? "A" : (len ? "no answer" : "ignored"));
#endif
    if (!len) continue;
    udp_.beginPacket(udp_.remoteIP(), udp_.remotePort());
    udp_.write(r, len);
    udp_.endPacket();
  }
}
#endif
