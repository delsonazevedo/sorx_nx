/* patch.c -- game-specific patches for libopenbor.so.
 *
 * Empty for the first bring-up, following the same wait-for-on-device-need
 * approach as the NBA Jam wrapper this loader is descended from: the Switch
 * so_util hook_arm64() is a destructive first-instructions overwrite with no
 * continue support, so only add hooks here once real hardware testing shows
 * they're needed. MIT license; see LICENSE. */

#include "patch.h"
#include "so_util.h"

void so_patch(so_module *mod) {
  (void)mod;
}
