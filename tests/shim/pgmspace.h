#pragma once
#include <stdint.h>
#ifndef PROGMEM
#define PROGMEM
#endif
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
