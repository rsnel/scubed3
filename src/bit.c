/* bit.c - simple and flexible bit-(un)packer
 *
 * Copyright (C) 2008  Rik Snel <rik@snel.it>
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
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "verbose.h"
#include "bit.h"
#include "binio.h"

uint16_t bit_get_size(uint16_t i, int strip) {
	assert(strip >= 0 && strip < 8*sizeof(uint32_t));

	return ((8*sizeof(uint32_t)-strip)*(uint32_t)i +
			8*sizeof(uint32_t) - 1)/(8*sizeof(uint32_t));
}

uint32_t bit_pack(uint32_t *p, const uint32_t *u, uint16_t i, int strip) {
	uint32_t tmp = 0, j, m = 0, ret;

	assert(p && u);

	ret = j = bit_get_size(i, strip);

	while (i-- > 0) {
		assert(!(u[i]&0xFFFFFFFF<<(8*sizeof(uint32_t)-strip)));
		tmp |= u[i]<<(8*sizeof(uint32_t)-m);

		if (m > 0 && m <= 8*sizeof(uint32_t) - strip) {
			//p[--j] = tmp;
			binio_write_uint32_be(p + --j, tmp);
			tmp = 0;
		}
		tmp |= u[i]>>m;
		m = (m+strip)%(8*sizeof(uint32_t));
	}
	/* flush data in tmp */
	if (m > 0 && m <= 8*sizeof(uint32_t) - strip) {
		binio_write_uint32_be(p + --j, tmp);
		//p[--j] = tmp;
	}
	return ret;
}

uint32_t bit_unpack(uint32_t *u, const uint32_t *p, uint16_t i, int strip) {
	uint32_t tmp = 0, j, m = 0, ret;
	assert(p && u);
	ret = j = bit_get_size(i, strip);

	while (i-- > 0) {
		if (m != 0)
			tmp = binio_read_uint32_be(p+j)>>(8*sizeof(uint32_t)-m);
		else tmp = 0;
		if (m < 8*sizeof(uint32_t) - strip) j--;
		u[i] = (tmp|binio_read_uint32_be(p + j)<<m)&(0xFFFFFFFF>>strip);
		m = (m+strip)%(8*sizeof(uint32_t));
	}

	return ret;
}
