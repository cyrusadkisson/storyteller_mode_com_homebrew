#pragma once
// Permanent event log (NVS flash). Lives in a header because the .ino
// preprocessor mangles struct-based code the same way it mangled the UI.
// Survives reboots and van power loss. The van has crashed three times with
// power loss; the last entries bound WHEN it died, and each pulse carries
// pack voltage/current so the trend into a death is recorded. Entry types:
// 1 boot, 2 cmd-before, 3 cmd-after, 4 pulse, 5 voltage/SoC disagreement
// tripped, 6 that disagreement cleared. Actions log before AND after;
// a 2 without a 3 means death mid-action.
#include <Preferences.h>

// ---------------------------------------------------------------------------
// SIZING -- one entry per NVS key does NOT fit. Writes began failing with
// NOT_ENOUGH_SPACE on 2026-09-04, the first time the log actually filled.
//
// The real cost of a 24-byte blob is THREE 32-byte NVS entries, not the 64 B
// this header used to claim: a blob-index entry, a data-chunk header, and one
// entry of data. The partition is 0x5000 = 20 KB = five 4 KB pages; NVS holds
// a page free to compact into and fits 126 entries per page, so ~504 entries
// are usable. 192 keys x 3 = 576. It never fit -- and putBytes' return value
// is not checked, so the log just stopped recording without saying so.
//
// Entries are therefore PACKED, LOGCHUNK to an NVS key: 24 keys x (index +
// header + 6 data entries) = 192 entries in ~6 KB, well inside the partition
// with room to compact. A write rewrites its whole 192-byte chunk, ~74 KB/day
// at ~290 writes/day, which is nothing to NVS wear levelling.
//
// Keep LOGN a multiple of LOGCHUNK.
// ---------------------------------------------------------------------------
#define LOGN 192                     // ~16 h of 5-min pulses, less with traffic
#define LOGCHUNK 8                   // entries packed into one NVS key
#define LOGKEYS (LOGN / LOGCHUNK)
#define LOGVER 2                     // bump to discard an incompatible layout

struct __attribute__((packed)) LogE {
  uint32_t seq; uint32_t ms; uint8_t type; uint8_t pad;
  uint16_t vCenti; int16_t aDeci; char cmd[8];
};                                   // 24 bytes

static Preferences vlog;
static uint32_t logSeq = 0;
static uint32_t lastPulseLog = 0;

// Read one packed chunk, zero-filling when the key is absent or the wrong
// size. seq == 0 then marks a slot that has never been written.
static void logChunkRead(uint32_t key, LogE *chunk) {
  char k[8];
  snprintf(k, sizeof k, "c%u", (unsigned)key);
  if (vlog.getBytesLength(k) != sizeof(LogE) * LOGCHUNK ||
      !vlog.getBytes(k, chunk, sizeof(LogE) * LOGCHUNK))
    memset(chunk, 0, sizeof(LogE) * LOGCHUNK);
}

static void logWrite(uint8_t type, const char *cmd) {
  LogE e;
  e.seq = ++logSeq; e.ms = millis(); e.type = type; e.pad = 0;
  e.vCenti = (uint16_t)(battV * 100.0f);
  e.aDeci = (int16_t)(battA * 10.0f);
  memset(e.cmd, 0, sizeof e.cmd);
  if (cmd) strncpy(e.cmd, cmd, sizeof e.cmd - 1);

  uint32_t slot = logSeq % LOGN;
  LogE chunk[LOGCHUNK];
  logChunkRead(slot / LOGCHUNK, chunk);
  chunk[slot % LOGCHUNK] = e;
  char k[8];
  snprintf(k, sizeof k, "c%u", (unsigned)(slot / LOGCHUNK));
  vlog.putBytes(k, chunk, sizeof chunk);
}

static void logInit() {
  vlog.begin("vlog", false);
  // The old one-key-per-entry layout would otherwise leave 192 stale keys
  // occupying exactly the space that caused the overflow. Reclaim it once.
  if (vlog.getUChar("ver", 0) != LOGVER) {
    vlog.clear();
    vlog.putUChar("ver", LOGVER);
  }
  LogE chunk[LOGCHUNK];
  for (uint32_t key = 0; key < LOGKEYS; key++) {
    logChunkRead(key, chunk);
    for (uint32_t i = 0; i < LOGCHUNK; i++)
      if (chunk[i].seq > logSeq) logSeq = chunk[i].seq;
  }
  logWrite(1, "boot");
  lastPulseLog = millis();
}

static String logDump() {
  const char *tn[7] = {"?", "BOOT", "CMD>", "CMD OK", "pulse",
                       "VSOC!", "VSOC ok"};
  String out;
  LogE chunk[LOGCHUNK];
  uint32_t loaded = 0xFFFFFFFFUL;               // which key `chunk` holds
  for (unsigned i = 0; i < LOGN; i++) {
    uint32_t slot = (logSeq + 1 + i) % LOGN;    // oldest first
    uint32_t key = slot / LOGCHUNK;
    if (key != loaded) { logChunkRead(key, chunk); loaded = key; }
    const LogE &e = chunk[slot % LOGCHUNK];
    if (e.seq == 0) continue;                   // never written
    char ln[128];
    snprintf(ln, sizeof ln, "%08lu  %8lums  %-7s  %5.2fV %6.1fA  %s\n",
             (unsigned long)e.seq, (unsigned long)e.ms,
             tn[e.type <= 6 ? e.type : 0], e.vCenti / 100.0f, e.aDeci / 10.0f,
             e.cmd);
    out += ln;
  }
  return out;
}
