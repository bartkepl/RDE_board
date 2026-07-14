/*
 * utils.h – serial number generation from MCU UID (FNV-1a hash)
 */

#ifndef APP_UTILS_UTILS_H_
#define APP_UTILS_UTILS_H_

#include <stdint.h>
#include <stddef.h>
#include <string.h>

const char *serial_get(void);
const char *serial_get_full(void);

#endif /* APP_UTILS_UTILS_H_ */
