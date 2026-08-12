/* util.h -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __UTIL_H__
#define __UTIL_H__

#if DEBUG_LOG
void userAppInit(void);
void userAppExit(void);
#endif

int debugPrintf(char *text, ...);
void tls_setup_guard(void);
void cpu_boost(int on);
void log_cpu_clock_periodic(void);
int ret0(void);
int retm1(void);

#endif
