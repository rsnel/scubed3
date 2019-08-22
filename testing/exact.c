#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include "verbose.h"

/* calculate expected value of number of datablocks after an amount of iterations */
/* if the expected value converges, we will get no datablocks, if it keeps growing without bound, 
 * then we will have stable datablocks */
#define NO_BLOCKS 4
#define SIMULTANEOUS_BLOCKS 2
#define MAX_NODES 100000000UL

typedef struct node_s {
	unsigned int parent;
	unsigned long int count_new;
	unsigned long int count_old;
	unsigned int next[NO_BLOCKS];
	unsigned char prev_idx;
	unsigned char prev_max_idx;
} __attribute__ ((packed)) node_t;

double Es[100] = { };
int max_node_length = 1; /* length of nodes that current computing is based on */
size_t no_nodes = 1, cur_node = 0;
node_t *nodes;

void walk_nodes(node_t *node, void (*action)(node_t*)) {
	assert(node && action);
	if (node != nodes) walk_nodes(&nodes[node->parent], action);
	action(node);
}

void show_node(node_t *node) {
	void print(node_t *node) {
		printf("%x", node->prev_idx);
	}
	walk_nodes(node, print);
	printf("\n");
}

unsigned char max_idx(node_t *node) {
	assert(node);
	if (node->prev_max_idx == NO_BLOCKS - 1) return NO_BLOCKS - 1;

	if (node->prev_max_idx == node->prev_idx) return node->prev_max_idx + 1;
	else return node->prev_max_idx;
}

void remove_entry(unsigned char *array, int idx, int *len) {
	assert(idx < *len);
	assert(idx >= 0);
	memmove(array + idx, array + idx + 1, (*len)-- - idx - 1);
}

/* a very stupid and slow algorithm */
void normalize(unsigned char *array, int len) {
	unsigned char translate[NO_BLOCKS];
	int indices[NO_BLOCKS];

	for (int i = 0; i < NO_BLOCKS; i++) indices[i] = -1;

	for (int i = 0; i < len; i++) {
		if (indices[array[i]] == -1) indices[array[i]] = i;
	}

	unsigned char new = 0;
	for (int i = 0; i < len; i++) {
		for (int j = 0; j < NO_BLOCKS; j++) {
			if (indices[j] == i) {
				translate[j] = new++;
				break;
			}
		}
	}

	for (int i = 0; i < len; i++) {
		array[i] = translate[array[i]];
	}
}

node_t *find_node_internal(node_t *cur, unsigned char *array, int len) {
	if (len > 0) return find_node_internal(nodes + cur->next[*array], array + 1, len -1);
	else return cur;
}

node_t *find_node(unsigned char *array, int len) {
	return find_node_internal(nodes, array, len);
}

void compute_node(node_t *node, unsigned char *this)  {
	assert(node); // && node->count_old);
	unsigned char scratch[max_node_length + 1];
	unsigned char max = max_idx(node);

	//VERBOSE("maxindex of this node is %u", max);

	for (unsigned char i = 0; i <= max; i++) {
		int len = max_node_length + 1;
		memcpy(scratch, this, max_node_length);
		scratch[max_node_length] = i;
		//VERBOSE("try %x", i);
		for (int j = max_node_length - 1; j >= 0; j--) {
			for (int k = 1; k <= SIMULTANEOUS_BLOCKS && j + k < len; k++) {
				//VERBOSE("compare j+k=%d with j=%d", j + k, j);
				if (scratch[j+k] == scratch[j]) {
					remove_entry(scratch, j, &len);
					break;
				}
			}
		}
		normalize(scratch, len);
		/*
		printf("after ");
		for (int j = 0; j < len; j++) {
			printf("%x", scratch[j]);
		}
		printf("\n");
		*/
		// three things may have happenend
		if (len == max_node_length) { // no change (+1 and -1)
			node_t *next = find_node(scratch, len);
			node->next[i] = next - nodes;
		} else if (len == max_node_length + 1) {
			// make new node
			node_t *new = &nodes[no_nodes++];
			assert(no_nodes <= MAX_NODES);
			node->next[i] = new - nodes;
			new->parent = node - nodes;
			new->prev_idx = i;
			new->prev_max_idx = max;
		} else if (len < max_node_length) {
			node_t *next = find_node(scratch, len);
			node->next[i] = next - nodes;
		}
	}
}

int length(node_t *node) {
	int len = 0;
	void length_internal(node_t *node) {
		len++;
	}
	walk_nodes(node, length_internal);
	return len;
}

void compute() {
	unsigned char this[max_node_length];
	int len;
	void put(node_t *node) {
		this[len++] = node->prev_idx;
	}
	//VERBOSE("compute %lu nodes from %lu", no_nodes - cur_node, cur_node);
 	size_t old_no_nodes = no_nodes;

	/* creating new nodes and connecting */
	while (cur_node < old_no_nodes) {
		//VERBOSE("computing node %lu", cur_node);
		node_t *node = &nodes[cur_node++];
		len = 0;
		walk_nodes(node, put);
		assert(len == max_node_length);
		compute_node(node, this);
	}

	/* propagating counts */
	for (int i = 0; i < no_nodes; i++) {
		node_t *node = &nodes[i];
		unsigned char max = max_idx(node);
		for (int j = 0; j <= max; j++) {
			if (j == max) nodes[node->next[j]].count_new += node->count_old*(NO_BLOCKS - j);
			else nodes[node->next[j]].count_new += node->count_old;
		}
	}

	/* moving counts from new to old, and resetting new count */
	for (int i = 0; i < no_nodes; i++) {
		nodes[i].count_old = nodes[i].count_new;
		nodes[i].count_new = 0;
	}
}

void status() {
	long unsigned int total = 0, total_freq = 0, count[max_node_length + 1];
	int i;
	memset(count, 0, sizeof(*count)*(max_node_length + 1));
	//printf("verbose status at %d\n", max_node_length);

	for (i = 0; i < no_nodes; i++) {
		node_t *node = &nodes[i];
		//show_node(node);
		count[length(node) - 1] += node->count_old;
	}

	for (i = 0; i < max_node_length; i++) {
		total += count[i];
		total_freq += (i + 1)*count[i];
		//printf("%d %ld\n", i + 1, count[i]);
	}
	Es[max_node_length] = ((double)total_freq)/((double)total);
	if (max_node_length > 1) printf("E(X_%d)=%f, diff=%f, factor_diff=%f, memory=%lu\n", max_node_length, Es[max_node_length], Es[max_node_length] - Es[max_node_length-1], (Es[max_node_length]-Es[max_node_length-1])/(Es[max_node_length-1] - Es[max_node_length-2]), sizeof(node_t)*no_nodes);
	else  printf("E(X_%d)=%f, diff=%f, memory=%lu\n", max_node_length, Es[max_node_length], Es[max_node_length] - Es[max_node_length-1], sizeof(node_t)*no_nodes);

}

int main(int argc, char *argv[]) {
	node_t *root;

	verbose_init(argv[0]);

	nodes = calloc(MAX_NODES, sizeof(node_t));
	if (!nodes) FATAL("calloc(%lu, %lu): %s", MAX_NODES, sizeof(node_t), strerror(errno));

	/* define root node */
	root = nodes;
	root->parent = 0; /* dummy, root has no parent */
	root->prev_idx = 0;
	root->prev_max_idx = 0;
	root->count_old = 1;
	no_nodes = 1;

	/* show status at root */
	status();

	for (int i = 0; i < 40; i++) {
		compute();
		max_node_length++;
		status();
	}

	exit(0);
}

