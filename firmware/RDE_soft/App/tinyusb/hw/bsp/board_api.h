/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * This file is part of the TinyUSB stack.
 */

#ifndef BOARD_API_H_
#define BOARD_API_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "tusb.h"

#include <stm32g431xx.h>
#include <stm32g4xx_hal.h>

// Define the default baudrate
#ifndef CFG_BOARD_UART_BAUDRATE
#define CFG_BOARD_UART_BAUDRATE 9600   ///< Default baud rate
#endif

//--------------------------------------------------------------------+
// Board Porting API
//--------------------------------------------------------------------+

void USB_DRD_FS_IRQHandler(void) {
  tud_int_handler(0);
}

void USB_IRQHandler(void) {
  USB_DRD_FS_IRQHandler();
}

#if CFG_TUSB_OS == OPT_OS_NONE
// Get current milliseconds, must be implemented when no RTOS is used
uint32_t tusb_time_millis_api(void) {
	return HAL_GetTick();
}

uint32_t board_millis(void){
	return tusb_time_millis_api();
}

#else
  #error "board_millis() is not implemented for this OS"
#endif


#ifdef __cplusplus
}
#endif

#endif
