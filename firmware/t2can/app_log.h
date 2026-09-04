#pragma once
// Permanent event log (NVS flash). Lives in a header because the .ino
// preprocessor mangles struct-based code the same way it mangled the UI.
// Survives reboots and van power loss. The van has crashed three times with
// power loss; the last entries bound WHEN it died, and each pulse carries
// pack voltage/current so the trend into a death is recorded. Entry types:
// 1 boot, 2 cmd-before, 3 cmd-after, 4 pulse, 5 voltage/SoC disagreement
// tripped, 6 that disagreement cleared. Actions log before AND after;
// a 2 without a 3 means death mid-action. Wear: ~290 writes/day is decades
// of NVS life.
#include <Preferences.h>

// 192 entries: ~16 h of 5-min pulses, or ~13 h with action traffic -- covers
// an overnight unattended run. Sized to the NVS partition (20 KB): 192 keys x
// 64 B on flash = 12 KB live, within the ~16 KB usable while keeping a page
// free for compaction.
#define LOGN 192
struct __attribute__((packed)) LogE {
  uint32_t seq; uint32_t ms; uint8_t type; uint8_t pad;
  uint16_t vCenti; int16_t aDeci; char cmd[8];
};                                   // 24 bytes

static Preferences vlog;
static uint32_t logSeq = 0;
static uint32_t lastPulseLog = 0;

static void logWrite(uint8_t type, const char *cmd) {
  LogE e;
  e.seq = ++logSeq; e.ms = millis(); e.type = type; e.pad = 0;
  e.vCenti = (uint16_t)(battV * 100.0f);
  e.aDeci = (int16_t)(battA * 10.0f);
  memset(e.cmd, 0, sizeof e.cmd);
  if (cmd) strncpy(e.cmd, cmd, sizeof e.cmd - 1);
  char k[8]; snprintf(k, sizeof k, "e%u", (unsigned)(logSeq % LOGN));
  vlog.putBytes(k, &e, sizeof e);
}

static void logInit() {
  vlog.begin("vlog", false);
  LogE e; char k[8];
  for (unsigned i = 0; i < LOGN; i++) {
    snprintf(k, sizeof k, "e%u", i);
    size_t n = vlog.getBytesLength(k);
    if (n == sizeof e && vlog.getBytes(k, &e, sizeof e) && e.seq > logSeq)
      logSeq = e.seq;
  }
  logWrite(1, "boot");
  lastPulseLog = millis();
}

static String logDump() {
  LogE e; char k[8]; String out;
  const char *tn[7] = {"?", "BOOT", "CMD>", "CMD OK", "pulse",
                       "VSOC!", "VSOC ok"};
  for (unsigned i = 0; i < LOGN; i++) {
    snprintf(k, sizeof k, "e%u", (unsigned)((logSeq + 1 + i) % LOGN));
    size_t n = vlog.getBytesLength(k);
    if (n != sizeof e) continue;
    if (!vlog.getBytes(k, &e, sizeof e)) continue;
    char ln[128];
    snprintf(ln, sizeof ln, "%08lu  %8lums  %-7s  %5.2fV %6.1fA  %s\n",
             (unsigned long)e.seq, (unsigned long)e.ms,
             tn[e.type <= 6 ? e.type : 0], e.vCenti / 100.0f, e.aDeci / 10.0f,
             e.cmd);
    out += ln;
  }
  return out;
}
