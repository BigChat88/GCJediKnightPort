/**
	libsmacker - A C library for decoding .smk Smacker Video files
	Copyright (C) 2012-2017 Greg Kennedy

	See smacker.h for more information.

	smk_hufftree.c
		Implementation of Smacker Huffman coding trees.
*/

#include "smk_hufftree.h"

/* malloc and friends */
#include "smk_malloc.h"

/**
	8-bit Tree node structure.
	If b0 is non-null, this is a branch, and b1 from the union should be used.
	If b0 is null, this is a leaf, and val / escape code from union should be used.
*/
struct smk_huff8_t
{
	struct smk_huff8_t* b0;
	union
	{
		struct smk_huff8_t* b1;
		struct
		{
			uint16_t value;
			uint8_t escapecode;
		} leaf;
	} u;
};

/**
	16-bit Tree root struct: holds a huff8_t structure,
	as well as a cache of three 16-bit values.
*/
struct smk_huff16_t
{
	struct smk_huff8_t* t;
	uint16_t cache[3];
};

#ifdef TARGET_GAMECUBE
/* Added: tree nodes come from a pool. A video's trees are ~290000 nodes of 8
	bytes, each one its own calloc: libogc2's dlmalloc spends ~32 bytes on a
	block that small, so opening the intro took 9 MB and the menu buffers
	failed (DSI on hardware). Nodes are carved out of 64 KB slabs, freed nodes
	go to a free list, and the slabs go back to the heap when no node is left.
	(Only the main thread decodes videos.) */
#define SMK_NODE_SLAB 8192
struct smk_node_slab { struct smk_node_slab* next; struct smk_huff8_t nodes[SMK_NODE_SLAB]; };
static struct smk_node_slab* smk_node_slabs = NULL;
static struct smk_huff8_t* smk_node_free = NULL;
static unsigned long smk_node_used = 0, smk_node_live = 0;

static struct smk_huff8_t* smk_node_alloc(void)
{
	struct smk_huff8_t* n;
	if (smk_node_free)
	{
		n = smk_node_free;
		smk_node_free = n->b0;
	}
	else
	{
		if (!smk_node_slabs || smk_node_used == SMK_NODE_SLAB)
		{
			struct smk_node_slab* s = (struct smk_node_slab*)malloc(sizeof(struct smk_node_slab));
			if (!s) return NULL;
			s->next = smk_node_slabs;
			smk_node_slabs = s;
			smk_node_used = 0;
		}
		n = &smk_node_slabs->nodes[smk_node_used++];
	}
	memset(n, 0, sizeof(*n));
	smk_node_live++;
	return n;
}

static void smk_node_release(struct smk_huff8_t* n)
{
	n->b0 = smk_node_free;
	smk_node_free = n;
	if (--smk_node_live == 0)
	{
		while (smk_node_slabs)
		{
			struct smk_node_slab* s = smk_node_slabs;
			smk_node_slabs = s->next;
			free(s);
		}
		smk_node_free = NULL;
		smk_node_used = 0;
	}
}

#define smk_node_malloc(p) \
{ \
	if (!(p = smk_node_alloc())) \
	{ \
		fprintf(stderr, "libsmacker::smk_node_malloc - ERROR: out of memory\n"); \
		goto error; \
	} \
}
#define smk_node_free_one(p) { smk_node_release(p); p = NULL; }
#else
#define smk_node_malloc(p) smk_malloc(p, sizeof(struct smk_huff8_t))
#define smk_node_free_one(p) smk_free(p)
#endif

/*********************** 8-BIT HUFF-TREE FUNCTIONS ***********************/
/** safe build with built-in error jump */
#define smk_huff8_build_rec(bs,p) \
{ \
	if (!(p = _smk_huff8_build_rec(bs))) \
	{ \
		fprintf(stderr, "libsmacker::smk_huff8_build_rec(" #bs ", " #p ") - ERROR (file: %s, line: %lu)\n", __FILE__, (unsigned long)__LINE__); \
		goto error; \
	} \
}
/** Recursive tree-building function. */
static struct smk_huff8_t* _smk_huff8_build_rec(struct smk_bit_t* bs)
{
	struct smk_huff8_t* ret = NULL;
	char bit;

	/* sanity check - removed: bs cannot be null, because it was checked at smk_huff8_build below */
	/* smk_assert(bs); */

	/* Read the bit */
	smk_bs_read_1(bs, bit);

	/* Malloc a structure. */
	smk_node_malloc(ret);

	if (bit)
	{
		/* Bit set: this forms a Branch node. */
		/* Recursively attempt to build the Left branch. */
		smk_huff8_build_rec(bs, ret->b0);

		/* Everything is still OK: attempt to build the Right branch. */
		smk_huff8_build_rec(bs, ret->u.b1);

		/* return branch pointer here */
		return ret;
	}

	/* Bit unset signifies a Leaf node. */
	/* Attempt to read value */
	smk_bs_read_8(bs, ret->u.leaf.value);

	/* smk_malloc sets entries to 0 by default */
	/* ret->b0 = NULL; */
	ret->u.leaf.escapecode = 0xFF;

	return ret;

error:
	/* In case of error, undo the subtree we were building, and return NULL. */
	smk_huff8_free(ret);
	return NULL;
}

/* Look up an 8-bit value from a basic huff tree.
	Return -1 on error. */
int16_t _smk_huff8_lookup(struct smk_bit_t* bs, const struct smk_huff8_t* t)
{
	char bit;

	/* sanity check */
	smk_assert(bs);
	smk_assert(t);

	if (!t->b0)
	{
		/* Reached a Leaf node. Return its value. */
		return t->u.leaf.value;
	}

	/* Read the next bit from bitstream to determine path */
	smk_bs_read_1(bs, bit);

	if (bit)
	{
		/* get_bit returned Set, follow Right branch. */
		return _smk_huff8_lookup(bs, t->u.b1);
	}

	/* follow Left branch */
	return _smk_huff8_lookup(bs, t->b0);

error:
	return -1;
}

/**
	Entry point for huff8 build. Basically just checks the start/end tags
	and calls smk_huff8_build_rec recursive function.
*/
struct smk_huff8_t* _smk_huff8_build(struct smk_bit_t* bs)
{
	struct smk_huff8_t* ret = NULL;
	char bit;

	/* sanity check */
	smk_assert(bs);

	/* Smacker huff trees begin with a set-bit. */
	smk_bs_read_1(bs, bit);

	if (!bit)
	{
		/* Got a bit, but it was not 1. In theory, there could be a smk file
			without this particular tree. */
		fputs("libsmacker::_smk_huff8_build(bs) - Warning: initial get_bit returned 0\n", stderr);
		goto error;
	}

	/* Begin parsing the tree data. */
	smk_huff8_build_rec(bs, ret);

	/* huff trees end with an unset-bit */
	smk_bs_read_1(bs, bit);

	if (bit)
	{
		fputs("libsmacker::_smk_huff8_build(bs) - ERROR: final get_bit returned 1\n", stderr);
		goto error;
	}

	return ret;

error:
	smk_huff8_free(ret);
	return NULL;
}

/* function to recursively delete a huffman tree */
void smk_huff8_free(struct smk_huff8_t* t)
{
	/* Sanity check: do not double-free */
	smk_assert(t);

	/* If this is not a leaf node, free child trees first */
	if (t->b0)
	{
		smk_huff8_free(t->b0);
		smk_huff8_free(t->u.b1);
	}

	/* Safe-delete tree node. */
	smk_node_free_one(t);

error: ;
}

/*********************** 16-BIT HUFF-TREE FUNCTIONS ***********************/
/* safe bigtree build with built-in error jump */
#define smk_huff16_build_rec(bs,cache,low8,hi8,p) \
{ \
	if (!(p = _smk_huff16_build_rec(bs, cache, low8, hi8))) \
	{ \
		fprintf(stderr, "libsmacker::smk_huff16_build_rec(" #bs ", " #cache ", " #low8 ", " #hi8 ", " #p ") - ERROR (file: %s, line: %lu)\n", __FILE__, (unsigned long)__LINE__); \
		goto error; \
	} \
}
/* Recursively builds a Big tree. */
static struct smk_huff8_t* _smk_huff16_build_rec(struct smk_bit_t* bs, const uint16_t cache[3], const struct smk_huff8_t* low8, const struct smk_huff8_t* hi8)
{
	struct smk_huff8_t* ret = NULL;

	char bit;
	int16_t lowval;

	/* sanity check - removed: these cannot be null, because they were checked at smk_huff16_build below */
	/* smk_assert(bs);
	smk_assert(cache);
	smk_assert(low8);
	smk_assert(hi8); */

	/* Get the first bit */
	smk_bs_read_1(bs, bit);

	/* Malloc a structure. */
	smk_node_malloc(ret);

	if (bit)
	{
		/* Recursively attempt to build the Left branch. */
		smk_huff16_build_rec(bs, cache, low8, hi8, ret->b0);

		/* Recursively attempt to build the Left branch. */
		smk_huff16_build_rec(bs, cache, low8, hi8, ret->u.b1);

		/* return branch pointer here */
		return ret;
	}

	/* Bit unset signifies a Leaf node. */
	smk_huff8_lookup(bs, low8, lowval);
	smk_huff8_lookup(bs, hi8, ret->u.leaf.value);

	/* Looks OK: we got low and hi values. Return a new LEAF */
	/* ret->b0 = NULL; */
	ret->u.leaf.value = lowval | (ret->u.leaf.value << 8);

	/* Last: when building the tree, some Values may correspond to cache positions.
		Identify these values and set the Escape code byte accordingly. */
	if (ret->u.leaf.value == cache[0])
	{
		ret->u.leaf.escapecode = 0;
	}
	else if (ret->u.leaf.value == cache[1])
	{
		ret->u.leaf.escapecode = 1;
	}
	else if (ret->u.leaf.value == cache[2])
	{
		ret->u.leaf.escapecode = 2;
	}
	else
	{
		ret->u.leaf.escapecode = 0xFF;
	}

	return ret;

error:
	smk_huff8_free(ret);
	return NULL;
}

/* Entry point for building a big 16-bit tree. */
struct smk_huff16_t* _smk_huff16_build(struct smk_bit_t* bs)
{
	struct smk_huff16_t* big = NULL;

	struct smk_huff8_t* low8 = NULL;
	struct smk_huff8_t* hi8 = NULL;

	int16_t lowval;

	char bit;
	uint8_t i;

	/* sanity check */
	smk_assert(bs);

	/* Smacker huff trees begin with a set-bit. */
	smk_bs_read_1(bs, bit);

	if (!bit)
	{
		fputs("libsmacker::smk_huff16_build(bs) - ERROR: initial get_bit returned 0\n", stderr);
		goto error;
	}

	/* build low-8-bits tree */
	smk_huff8_build(bs, low8);
	/* build hi-8-bits tree */
	smk_huff8_build(bs, hi8);

	/* Everything looks OK so far. Time to malloc structure. */
	smk_malloc(big, sizeof(struct smk_huff16_t));

	/* Init the escape code cache. */
	for (i = 0; i < 3; i ++)
	{
		smk_bs_read_8(bs, lowval);
		smk_bs_read_8(bs, big->cache[i]);
		big->cache[i] = lowval | (big->cache[i] << 8);
	}

	/* Finally, call recursive function to retrieve the Bigtree. */
	smk_huff16_build_rec(bs, big->cache, low8, hi8, big->t);

	/* Done with 8-bit hufftrees, free them. */
	smk_huff8_free(hi8);
	smk_huff8_free(low8);

	/* Check final end tag. */
	smk_bs_read_1(bs, bit);

	if (bit)
	{
		fputs("libsmacker::smk_huff16_build(bs) - ERROR: final get_bit returned 1\n", stderr);
		goto error;
	}

	return big;

error:
	smk_huff16_free(big);
	smk_huff8_free(hi8);
	smk_huff8_free(low8);
	return NULL;
}

static uint16_t _smk_huff16_lookup_rec(struct smk_bit_t* bs, uint16_t cache[3], const struct smk_huff8_t* t)
{
	uint16_t val;
	char bit;

	/* sanity check */
	/* smk_assert(bs);
	smk_assert(cache);
	smk_assert(t); */

	/* Reached a Leaf node */
	if (!t->b0)
	{
		if (t->u.leaf.escapecode != 0xFF)
		{
			/* Found escape code. Retrieve value from Cache. */
			val = cache[t->u.leaf.escapecode];
		}
		else
		{
			/* Use value directly. */
			val = t->u.leaf.value;
		}

		if (cache[0] != val)
		{
			/* Update the cache, by moving val to the front of the queue,
				if it isn't already there. */
			cache[2] = cache[1];
			cache[1] = cache[0];
			cache[0] = val;
		}

		return val;
	}

	/* Read the next bit from bitstream to determine path */
	smk_bs_read_1(bs, bit);

	if (bit)
	{
		/* get_bit returned Set, follow Right branch. */
		return _smk_huff16_lookup_rec(bs, cache, t->u.b1);
	}

	return _smk_huff16_lookup_rec(bs, cache, t->b0);

error:
	return -1;
}

/* Convenience call-out for recursive bigtree lookup function */
uint16_t _smk_huff16_lookup(struct smk_bit_t* bs, struct smk_huff16_t* big)
{
	/* sanity check */
	smk_assert(bs);
	smk_assert(big);

	return _smk_huff16_lookup_rec(bs, big->cache, big->t);

error:
	return -1;
}

/* Resets a Big hufftree cache */
void smk_huff16_reset(struct smk_huff16_t* big)
{
	/* sanity check */
	smk_assert(big);

	big->cache[0] = 0;
	big->cache[1] = 0;
	big->cache[2] = 0;

error: ;
}

/* delete a (big) huffman tree */
void smk_huff16_free(struct smk_huff16_t* big)
{
	/* Sanity check: do not double-free */
	smk_assert(big);
 
	/* free the subtree */
	if (big->t)
		smk_huff8_free(big->t);

	/* free the bigtree */
	smk_free(big);

error: ;
};
