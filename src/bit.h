/* bit.h - simple and flexible bit packer/unpacker
 *
 * Copyright (C) 2009  Rik Snel <rik@snel.it>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef INCLUDE_SCUBED3_BIT_H
#define INCLUDE_SCUBED3_BIT_H 1

#include <stdint.h>

uint16_t bit_get_size(uint16_t, int);

uint32_t bit_pack(uint32_t*, const uint32_t*, uint16_t, int);

uint32_t bit_unpack(uint32_t*, const uint32_t*, uint16_t, int);

#endif /* INCLUDE_SCUBED3_BIT_H */
