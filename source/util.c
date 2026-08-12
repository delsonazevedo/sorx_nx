/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "config.h"

#if DEBUG_LOG

static int s_nxlinkSock = -1;
static FILE *s_log = NULL; // persistent log handle (fast; fflush per line)
static uint64_t s_boot_tick; // armGetSystemTick() as of userAppInit(), our earliest hookable point

static void initNxLink(void) {
  if (R_FAILED(socketInitializeDefault()))
    return;
  s_nxlinkSock = nxlinkStdio();
  if (s_nxlinkSock < 0)
    socketExit();
}

static void deinitNxLink(void) {
  if (s_nxlinkSock >= 0) {
    close(s_nxlinkSock);
    socketExit();
    s_nxlinkSock = -1;
  }
}

// sdmc is mounted by the time userAppInit runs, so open the log once here
// instead of reopening it per line.
void userAppInit(void) {
  s_boot_tick = armGetSystemTick();
  initNxLink();
  s_log = fopen(LOG_PATH, "w");
  if (!s_log) s_log = fopen(LOG_NAME, "w"); // fall back to the launch CWD
  if (s_log) {
    fputs("== sorx log open ==\n", s_log);
    fflush(s_log);
  }
}

void userAppExit(void) {
  if (s_log) { fclose(s_log); s_log = NULL; }
  deinitNxLink();
}

#endif

int debugPrintf(char *text, ...) {
#if DEBUG_LOG
  va_list list;

  // Real-elapsed-seconds-since-boot prefix: this codebase's various targeted
  // timing diagnostics (audio.c's content/real ratio, egl_shim.c's swap
  // timing, ...) each had to invent their own clock reference to answer
  // "how long did that actually take" -- prefixing every line here instead
  // makes ANY two lines in the log (including OpenBOR's own "Loading ..."
  // progress messages, which we never had to add a single instrumentation
  // call for) directly comparable, retroactively, without needing to have
  // guessed in advance which milestone would matter. Cheap: one extra
  // armGetSystemTick()+armTicksToNs() per line, dwarfed by the fflush()
  // already happening right after.
  double elapsed_s = (double)armTicksToNs(armGetSystemTick() - s_boot_tick) / 1e9;

  if (s_log) {
    fprintf(s_log, "[%9.3f] ", elapsed_s);
    va_start(list, text);
    vfprintf(s_log, text, list);
    va_end(list);
    fflush(s_log); // flush each line so a crash still leaves a complete log
  }

  printf("[%9.3f] ", elapsed_s);
  va_start(list, text);
  vprintf(text, list);
  va_end(list);
#endif
  return 0;
}

// Shared TLS block for the loaded modules' stack-protector guard at
// tpidr_el0 + 0x28. Written directly via MSR: this libnx doesn't expose a
// tpidr_el0 (read-write TLS) setter of its own -- armGetTls()/the kernel's
// TLS slot APIs all operate on tpidr**ro**_el0, the separate IPC buffer
// register bionic doesn't touch.
static uint8_t s_tls_block[0x1000] __attribute__((aligned(16)));

void tls_setup_guard(void) {
  *(uint64_t *)(s_tls_block + 0x28) = 0x0123456789ABCDEFull;
  __asm__ __volatile__("msr tpidr_el0, %x0" :: "r"(s_tls_block));
}

// boost the CPU to 1785MHz while loading
//
// On-device evidence (main.c's adaptive-boost logging showed FastLoad
// covering 100% of a whole ~190s loading window, yet the load took just as
// long as an earlier run that only had ~86% coverage) contradicts the
// assumption that CPU clock was the bottleneck here at all -- possible
// explanations include ApmCpuBoostMode_FastLoad not actually raising the
// CPU clock the way requested (libnx docs describe it purely in terms of
// "boost mode", not a guaranteed frequency) or its side effect of
// throttling the GPU to minimum (per libnx's apm.h: "Boost CPU.
// Additionally, throttle GPU to minimum") costing back whatever the CPU
// side gains if the load also does meaningful GPU work. Query and log the
// actual CpuBus clock rate via clkrst (not just our own "boosted" flag)
// whenever this is called, so the next capture gives real hardware
// evidence either way instead of another assumption.
static int s_clkrst_ready = -1; // -1 = not yet tried, 0 = unavailable, 1 = ready
static ClkrstSession s_clkrst_cpu;

static void log_cpu_clock(const char *when) {
  if (s_clkrst_ready == -1) {
    s_clkrst_ready = 0;
    if (R_SUCCEEDED(clkrstInitialize())) {
      if (R_SUCCEEDED(clkrstOpenSession(&s_clkrst_cpu, PcvModuleId_CpuBus, 3)))
        s_clkrst_ready = 1;
      else
        clkrstExit();
    }
  }
  if (s_clkrst_ready != 1) {
    debugPrintf("[clock] %s: clkrst unavailable, can't confirm actual CPU Hz\n", when);
    return;
  }
  u32 hz = 0;
  Result rc = clkrstGetClockRate(&s_clkrst_cpu, &hz);
  if (R_SUCCEEDED(rc))
    debugPrintf("[clock] %s: CpuBus clock = %u Hz (%.1f MHz)\n", when, hz, hz / 1000000.0);
  else
    debugPrintf("[clock] %s: clkrstGetClockRate failed: 0x%x\n", when, rc);
}

void cpu_boost(int on) {
  // The one and only clock sample the previous capture produced (right at
  // process start, immediately after the very first cpu_boost(1)) read
  // 1020 MHz -- the Switch's normal, non-boosted CPU clock -- suggesting
  // appletSetCpuBoostMode(FastLoad) isn't actually raising it. Checking the
  // call's own Result (never done before) is the obvious next thing that
  // was still unverified: if THIS is failing outright, that alone would
  // fully explain a clock reading that never moves.
  Result rc = appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
  char label[64];
  snprintf(label, sizeof(label), "after cpu_boost(%d) rc=0x%x", on, rc);
  log_cpu_clock(label);
}

// Independent of any boost state transition: main.c's loop calls this
// periodically so the log has clock samples spread through the whole
// loading window, not just the two (or, on-device, exactly one) moments a
// transition happened to log one. A clock that's genuinely stuck at 1020
// MHz throughout -- boosted or not -- is a very different finding from one
// that only fails to move right at the very first call.
void log_cpu_clock_periodic(void) {
  log_cpu_clock("periodic sample");
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }
