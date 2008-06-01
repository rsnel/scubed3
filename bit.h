#ifndef INCLUDE_LSBD_BIT_H
#define INCLUDE_LSBD_BIT_H 1

#include <stdint.h>

uint16_t bit_get_size(uint16_t, int);

uint32_t bit_pack(uint32_t*, const uint32_t*, uint16_t, int);

uint32_t bit_unpack(uint32_t*, const uint32_t*, uint16_t, int);

#endif /* INCLUDE_LSBD_BIT_H */
