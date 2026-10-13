/* jstat: statistical test of juggler.c: is every draw uniform over the
 * pool at that moment, also while blocks are added and removed? */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "blockio.h"
#include "juggler.h"

int main(int argc, char **argv) {
	int total = 64, steps = atoi(argv[1]), t, i;
	blockio_info_t *bi = calloc(total, sizeof(*bi));
	char *in = calloc(total, 1);
	double chi2 = 0, df = 0, rep_exp = 0;
	long rep = 0, runs[12] = { 0 }, runlen = 1;
	long count[64][64]; /* count[poolsize][position of drawn block] */
	random_t r; juggler_t j;
	blockio_info_t *prev = NULL;
	memset(count, 0, sizeof(count));
	random_init(&r); juggler_init(&j, &r);
	for (i = 0; i < 16; i++) { juggler_add_macroblock(&j, &bi[i]); in[i] = 1; }
	for (t = 0; t < steps; t++) {
		blockio_info_t *cur = juggler_get_devblock(&j), *next = j.next;
		uint32_t n = juggler_count(&j);
		/* the drawn block, by position in the pool: uniform? */
		count[n][next->pool_index]++;
		if (next == cur) rep++;
		rep_exp += 1.0/n;
		if (cur == prev) runlen++; else { if (prev) runs[runlen < 11 ? runlen : 11]++; runlen = 1; }
		prev = cur;
		/* every 1000 steps: grow or shrink the pool (like resize) */
		if (t % 1000 == 999) {
			int target = 2 + random_custom(&r, 40);
			while (juggler_count(&j) < target) {
				do i = random_custom(&r, total); while (in[i]);
				juggler_add_macroblock(&j, &bi[i]); in[i] = 1;
			}
			while (juggler_count(&j) > target) {
				blockio_info_t *v = juggler_nth(&j, random_custom(&r, juggler_count(&j)));
				if (v == j.next) continue;
				juggler_remove_macroblock(&j, v); in[v - bi] = 0;
			}
		}
	}
	/* chi-square over all pool sizes with enough samples */
	for (int n = 2; n < 64; n++) {
		long tot = 0; for (i = 0; i < n; i++) tot += count[n][i];
		if (tot < 50*n) continue;
		double e = (double)tot/n;
		for (i = 0; i < n; i++) chi2 += (count[n][i] - e)*(count[n][i] - e)/e;
		df += n - 1;
	}
	printf("chi-square %.1f with %.0f degrees of freedom (z = %.2f)\n",
			chi2, df, (chi2 - df)/sqrt(2*df));
	printf("same block drawn twice: %ld, expected %.1f (z = %.2f)\n",
			rep, rep_exp, (rep - rep_exp)/sqrt(rep_exp));
	printf("run lengths:");
	for (i = 1; i < 12; i++) printf(" %ld", runs[i]);
	printf("\n");
	if (fabs((chi2 - df)/sqrt(2*df)) > 4 ||
			fabs((rep - rep_exp)/sqrt(rep_exp)) > 4) {
		printf("JSTAT FAILED\n");
		return 1;
	}
	printf("JSTAT OK\n");
	return 0;
}
