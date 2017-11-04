/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"
#include "mem_pool.h"
#include "fstring.h"
#include "logger.h"
#include "ottery.h"
#include "unix-std.h"
#include "khash.h"
#include "cryptobox.h"

#ifdef HAVE_SCHED_YIELD
#include <sched.h>
#endif

/* Sleep time for spin lock in nanoseconds */
#define MUTEX_SLEEP_TIME 10000000L
#define MUTEX_SPIN_COUNT 100

#define POOL_MTX_LOCK() do { } while (0)
#define POOL_MTX_UNLOCK()   do { } while (0)

/*
 * This define specify whether we should check all pools for free space for new object
 * or just begin scan from current (recently attached) pool
 * If MEMORY_GREEDY is defined, then we scan all pools to find free space (more CPU usage, slower
 * but requires less memory). If it is not defined check only current pool and if object is too large
 * to place in it allocate new one (this may cause huge CPU usage in some cases too, but generally faster than
 * greedy method)
 */
#undef MEMORY_GREEDY


#define ENTRY_LEN 128
#define ENTRY_NELTS 64

struct entry_elt {
	guint32 fragmentation;
	guint32 leftover;
};

struct rspamd_mempool_entry_point {
	gchar src[ENTRY_LEN];
	guint32 cur_suggestion;
	guint32 cur_elts;
	struct entry_elt elts[ENTRY_NELTS];
};


static inline uint32_t
rspamd_entry_hash (const char *str)
{
	return rspamd_cryptobox_fast_hash (str, strlen (str), rspamd_hash_seed ());
}

static inline int
rspamd_entry_equal (const char *k1, const char *k2)
{
	return strcmp (k1, k2) == 0;
}


KHASH_INIT(mempool_entry, const gchar *, struct rspamd_mempool_entry_point *,
		1, rspamd_entry_hash, rspamd_entry_equal)

static khash_t(mempool_entry) *mempool_entries = NULL;


/* Internal statistic */
static rspamd_mempool_stat_t *mem_pool_stat = NULL;
/* Environment variable */
static gboolean env_checked = FALSE;
static gboolean always_malloc = FALSE;

/**
 * Function that return free space in pool page
 * @param x pool page struct
 */
static gsize
pool_chain_free (struct _pool_chain *chain)
{
	gint64 occupied = chain->pos - chain->begin + MEM_ALIGNMENT;

	return (occupied < (gint64)chain->len ?
			chain->len - occupied : 0);
}

/* By default allocate 8Kb chunks of memory */
#define FIXED_POOL_SIZE 4096

static inline struct rspamd_mempool_entry_point *
rspamd_mempool_entry_new (const gchar *loc)
{
	struct rspamd_mempool_entry_point **pentry, *entry;
	gint r;
	khiter_t k;

	k = kh_put (mempool_entry, mempool_entries, loc, &r);

	if (r >= 0) {
		pentry = &kh_value (mempool_entries, k);
		entry = g_malloc0 (sizeof (*entry));
		*pentry = entry;
		memset (entry, 0, sizeof (*entry));
		rspamd_strlcpy (entry->src, loc, sizeof (entry->src));
#ifdef HAVE_GETPAGESIZE
		entry->cur_suggestion =  MAX (getpagesize (), FIXED_POOL_SIZE);
#else
		entry->cur_suggestion =  MAX (sysconf (_SC_PAGESIZE), FIXED_POOL_SIZE);
#endif
	}
	else {
		g_assert_not_reached ();
	}

	return entry;
}

static inline struct rspamd_mempool_entry_point *
rspamd_mempool_get_entry (const gchar *loc)
{
	khiter_t k;
	struct rspamd_mempool_entry_point *elt;

	if (mempool_entries == NULL) {
		mempool_entries = kh_init (mempool_entry);
	}
	else {
		k = kh_get (mempool_entry, mempool_entries, loc);

		if (k != kh_end (mempool_entries)) {
			elt = kh_value (mempool_entries, k);

			return elt;
		}
	}

	return rspamd_mempool_entry_new (loc);
}

static struct _pool_chain *
rspamd_mempool_chain_new (gsize size, enum rspamd_mempool_chain_type pool_type)
{
	struct _pool_chain *chain;
	gpointer map;

	g_return_val_if_fail (size > 0, NULL);

	if (pool_type == RSPAMD_MEMPOOL_SHARED) {
#if defined(HAVE_MMAP_ANON)
		map = mmap (NULL,
				size + sizeof (struct _pool_chain),
				PROT_READ | PROT_WRITE,
				MAP_ANON | MAP_SHARED,
				-1,
				0);
		if (map == MAP_FAILED) {
			msg_err ("cannot allocate %z bytes of shared memory, aborting", size +
					sizeof (struct _pool_chain));
			abort ();
		}
		chain = map;
		chain->begin = ((guint8 *) chain) + sizeof (struct _pool_chain);
#elif defined(HAVE_MMAP_ZERO)
		gint fd;

		fd = open ("/dev/zero", O_RDWR);
		if (fd == -1) {
			return NULL;
		}
		map = mmap (NULL,
				size + sizeof (struct _pool_chain),
				PROT_READ | PROT_WRITE,
				MAP_SHARED,
				fd,
				0);
		if (map == MAP_FAILED) {
			msg_err ("cannot allocate %z bytes, aborting", size +
					sizeof (struct _pool_chain));
			abort ();
		}
		chain = map;
		chain->begin = ((guint8 *) chain) + sizeof (struct _pool_chain);
#else
#error No mmap methods are defined
#endif
		g_atomic_int_inc (&mem_pool_stat->shared_chunks_allocated);
		g_atomic_int_add (&mem_pool_stat->bytes_allocated, size);
	}
	else {
		map = g_malloc (sizeof (struct _pool_chain) + size);
		chain = map;
		chain->begin = ((guint8 *) chain) + sizeof (struct _pool_chain);
		g_atomic_int_add (&mem_pool_stat->bytes_allocated, size);
		g_atomic_int_inc (&mem_pool_stat->chunks_allocated);
	}

	chain->pos = align_ptr (chain->begin, MEM_ALIGNMENT);
	chain->len = size;
	chain->lock = NULL;

	return chain;
}

static void
rspamd_mempool_create_pool_type (rspamd_mempool_t * pool,
		enum rspamd_mempool_chain_type pool_type)
{
	gsize preallocated_len;

	switch (pool_type) {
	case RSPAMD_MEMPOOL_NORMAL:
		preallocated_len = 32;
		break;
	case RSPAMD_MEMPOOL_SHARED:
	case RSPAMD_MEMPOOL_TMP:
	default:
		preallocated_len = 2;
		break;
	}

	pool->pools[pool_type] = g_ptr_array_sized_new (preallocated_len);
}

/**
 * Get the current pool of the specified type, creating the corresponding
 * array if it's absent
 * @param pool
 * @param pool_type
 * @return
 */
static struct _pool_chain *
rspamd_mempool_get_chain (rspamd_mempool_t * pool,
		enum rspamd_mempool_chain_type pool_type)
{
	gsize len;
	g_assert (pool_type >= 0 && pool_type < RSPAMD_MEMPOOL_MAX);

	if (pool->pools[pool_type] == NULL) {
		rspamd_mempool_create_pool_type (pool, pool_type);
	}

	len = pool->pools[pool_type]->len;

	if (len == 0) {
		return NULL;
	}

	return (g_ptr_array_index (pool->pools[pool_type], len - 1));
}

static void
rspamd_mempool_append_chain (rspamd_mempool_t * pool,
		struct _pool_chain *chain,
		enum rspamd_mempool_chain_type pool_type)
{
	g_assert (pool_type >= 0 && pool_type < RSPAMD_MEMPOOL_MAX);
	g_assert (chain != NULL);

	if (pool->pools[pool_type] == NULL) {
		rspamd_mempool_create_pool_type (pool, pool_type);
	}

	g_ptr_array_add (pool->pools[pool_type], chain);
}

/**
 * Allocate new memory poll
 * @param size size of pool's page
 * @return new memory pool object
 */
rspamd_mempool_t *
rspamd_mempool_new_ (gsize size, const gchar *tag, const gchar *loc)
{
	rspamd_mempool_t *new;
	gpointer map;
	unsigned char uidbuf[10];
	const gchar hexdigits[] = "0123456789abcdef";
	unsigned i;

	/* Allocate statistic structure if it is not allocated before */
	if (mem_pool_stat == NULL) {
#if defined(HAVE_MMAP_ANON)
		map = mmap (NULL,
				sizeof (rspamd_mempool_stat_t),
				PROT_READ | PROT_WRITE,
				MAP_ANON | MAP_SHARED,
				-1,
				0);
		if (map == MAP_FAILED) {
			msg_err ("cannot allocate %z bytes, aborting",
				sizeof (rspamd_mempool_stat_t));
			abort ();
		}
		mem_pool_stat = (rspamd_mempool_stat_t *)map;
#elif defined(HAVE_MMAP_ZERO)
		gint fd;

		fd = open ("/dev/zero", O_RDWR);
		g_assert (fd != -1);
		map = mmap (NULL,
				sizeof (rspamd_mempool_stat_t),
				PROT_READ | PROT_WRITE,
				MAP_SHARED,
				fd,
				0);
		if (map == MAP_FAILED) {
			msg_err ("cannot allocate %z bytes, aborting",
				sizeof (rspamd_mempool_stat_t));
			abort ();
		}
		mem_pool_stat = (rspamd_mempool_stat_t *)map;
#else
#       error No mmap methods are defined
#endif
		memset (map, 0, sizeof (rspamd_mempool_stat_t));
	}

	if (!env_checked) {
		/* Check G_SLICE=always-malloc to allow memory pool debug */
		const char *g_slice;

		g_slice = getenv ("VALGRIND");
		if (g_slice != NULL) {
			always_malloc = TRUE;
		}
		env_checked = TRUE;
	}

	new = g_malloc0 (sizeof (rspamd_mempool_t));
	new->entry = rspamd_mempool_get_entry (loc);
	new->destructors = g_array_sized_new (FALSE, FALSE,
			sizeof (struct _pool_destructors), 32);
	rspamd_mempool_create_pool_type (new, RSPAMD_MEMPOOL_NORMAL);
	/* Set it upon first call of set variable */
	new->elt_len = new->entry->cur_suggestion;

	if (tag) {
		rspamd_strlcpy (new->tag.tagname, tag, sizeof (new->tag.tagname));
	}
	else {
		new->tag.tagname[0] = '\0';
	}

	/* Generate new uid */
	ottery_rand_bytes (uidbuf, sizeof (uidbuf));
	for (i = 0; i < G_N_ELEMENTS (uidbuf); i ++) {
		new->tag.uid[i * 2] = hexdigits[(uidbuf[i] >> 4) & 0xf];
		new->tag.uid[i * 2 + 1] = hexdigits[uidbuf[i] & 0xf];
	}
	new->tag.uid[19] = '\0';

	mem_pool_stat->pools_allocated++;

	return new;
}

static void *
memory_pool_alloc_common (rspamd_mempool_t * pool, gsize size,
		enum rspamd_mempool_chain_type pool_type)
{
	guint8 *tmp;
	struct _pool_chain *new, *cur;
	gsize free = 0;

	if (pool) {
		POOL_MTX_LOCK ();
		if (always_malloc && pool_type != RSPAMD_MEMPOOL_SHARED) {
			void *ptr;

			ptr = g_malloc (size);
			POOL_MTX_UNLOCK ();

			if (pool->trash_stack == NULL) {
				pool->trash_stack = g_ptr_array_sized_new (128);
			}

			g_ptr_array_add (pool->trash_stack, ptr);

			return ptr;
		}

		cur = rspamd_mempool_get_chain (pool, pool_type);

		/* Find free space in pool chain */
		if (cur) {
			free = pool_chain_free (cur);
		}

		if (cur == NULL || free < size) {
			/* Allocate new chain element */
			if (pool->elt_len >= size + MEM_ALIGNMENT) {
				pool->entry->elts[pool->entry->cur_elts].fragmentation += size;
				new = rspamd_mempool_chain_new (pool->elt_len + MEM_ALIGNMENT,
						pool_type);
			}
			else {
				mem_pool_stat->oversized_chunks++;
				g_atomic_int_add (&mem_pool_stat->fragmented_size,
						free);
				pool->entry->elts[pool->entry->cur_elts].fragmentation += free;
				new = rspamd_mempool_chain_new (
						size + pool->elt_len + MEM_ALIGNMENT, pool_type);
			}

			/* Connect to pool subsystem */
			rspamd_mempool_append_chain (pool, new, pool_type);
			/* No need to align again */
			tmp = new->pos;
			new->pos = tmp + size;
			POOL_MTX_UNLOCK ();

			return tmp;
		}

		/* No need to allocate page */
		tmp = align_ptr (cur->pos, MEM_ALIGNMENT);
		cur->pos = tmp + size;
		POOL_MTX_UNLOCK ();

		return tmp;
	}

	return NULL;
}


void *
rspamd_mempool_alloc (rspamd_mempool_t * pool, gsize size)
{
	return memory_pool_alloc_common (pool, size, RSPAMD_MEMPOOL_NORMAL);
}

void *
rspamd_mempool_alloc_tmp (rspamd_mempool_t * pool, gsize size)
{
	return memory_pool_alloc_common (pool, size, RSPAMD_MEMPOOL_TMP);
}

void *
rspamd_mempool_alloc0 (rspamd_mempool_t * pool, gsize size)
{
	void *pointer = rspamd_mempool_alloc (pool, size);
	if (pointer) {
		memset (pointer, 0, size);
	}
	return pointer;
}

void *
rspamd_mempool_alloc0_tmp (rspamd_mempool_t * pool, gsize size)
{
	void *pointer = rspamd_mempool_alloc_tmp (pool, size);
	if (pointer) {
		memset (pointer, 0, size);
	}
	return pointer;
}

void *
rspamd_mempool_alloc0_shared (rspamd_mempool_t * pool, gsize size)
{
	void *pointer = rspamd_mempool_alloc_shared (pool, size);
	if (pointer) {
		memset (pointer, 0, size);
	}
	return pointer;
}

void *
rspamd_mempool_alloc_shared (rspamd_mempool_t * pool, gsize size)
{
	return memory_pool_alloc_common (pool, size, RSPAMD_MEMPOOL_SHARED);
}


gchar *
rspamd_mempool_strdup (rspamd_mempool_t * pool, const gchar *src)
{
	gsize len;
	gchar *newstr;

	if (src == NULL) {
		return NULL;
	}

	len = strlen (src);
	newstr = rspamd_mempool_alloc (pool, len + 1);
	memcpy (newstr, src, len);
	newstr[len] = '\0';

	return newstr;
}

gchar *
rspamd_mempool_fstrdup (rspamd_mempool_t * pool, const struct f_str_s *src)
{
	gchar *newstr;

	if (src == NULL) {
		return NULL;
	}

	newstr = rspamd_mempool_alloc (pool, src->len + 1);
	memcpy (newstr, src->str, src->len);
	newstr[src->len] = '\0';

	return newstr;
}

gchar *
rspamd_mempool_ftokdup (rspamd_mempool_t *pool, const rspamd_ftok_t *src)
{
	gchar *newstr;

	if (src == NULL) {
		return NULL;
	}

	newstr = rspamd_mempool_alloc (pool, src->len + 1);
	memcpy (newstr, src->begin, src->len);
	newstr[src->len] = '\0';

	return newstr;
}

void
rspamd_mempool_add_destructor_full (rspamd_mempool_t * pool,
	rspamd_mempool_destruct_t func,
	void *data,
	const gchar *function,
	const gchar *line)
{
	struct _pool_destructors cur;

	POOL_MTX_LOCK ();
	cur.func = func;
	cur.data = data;
	cur.function = function;
	cur.loc = line;

	g_array_append_val (pool->destructors, cur);
	POOL_MTX_UNLOCK ();
}

void
rspamd_mempool_replace_destructor (rspamd_mempool_t * pool,
	rspamd_mempool_destruct_t func,
	void *old_data,
	void *new_data)
{
	struct _pool_destructors *tmp;
	guint i;

	for (i = 0; i < pool->destructors->len; i ++) {
		tmp = &g_array_index (pool->destructors, struct _pool_destructors, i);

		if (tmp->func == func && tmp->data == old_data) {
			tmp->func = func;
			tmp->data = new_data;
			break;
		}
	}
}

static gint
cmp_int (gconstpointer a, gconstpointer b)
{
	gint i1 = *(const gint *)a, i2 = *(const gint *)b;

	return i1 - i2;
}

static void
rspamd_mempool_adjust_entry (struct rspamd_mempool_entry_point *e)
{
	gint sz[G_N_ELEMENTS (e->elts)], sel_pos, sel_neg;
	guint i, jitter;

	for (i = 0; i < G_N_ELEMENTS (sz); i ++) {
		sz[i] = e->elts[i].fragmentation - (gint)e->elts[i].leftover;
	}

	qsort (sz, G_N_ELEMENTS (sz), sizeof (gint), cmp_int);
	jitter = rspamd_random_uint64_fast () % 10;
	/*
	 * Take stochaistic quantiles
	 */
	sel_pos = sz[50 + jitter];
	sel_neg = sz[4 + jitter];

	if (sel_neg > 0) {
		/* We need to increase our suggestion */
		e->cur_suggestion *= (1 + (((double)sel_pos) / e->cur_suggestion)) * 1.5;
	}
	else if (-sel_neg > sel_pos) {
		/* We need to reduce current suggestion */
		e->cur_suggestion /= (1 + (((double)-sel_neg) / e->cur_suggestion)) * 1.5;
	}
	else {
		/* We still want to grow */
		e->cur_suggestion *= (1 + (((double)sel_pos) / e->cur_suggestion)) * 1.5;
	}

	/* Some sane limits counting mempool architecture */
	if (e->cur_suggestion < 1024) {
		e->cur_suggestion = 1024;
	}
	else if (e->cur_suggestion > 1024 * 1024 * 10) {
		e->cur_suggestion = 1024 * 1024 * 10;
	}

	memset (e->elts, 0, sizeof (e->elts));
}

void
rspamd_mempool_delete (rspamd_mempool_t * pool)
{
	struct _pool_chain *cur;
	struct _pool_destructors *destructor;
	gpointer ptr;
	guint i, j;
	gsize len;

	POOL_MTX_LOCK ();

	/* Find free space in pool chain */
	cur = NULL;

	if (pool->pools[RSPAMD_MEMPOOL_NORMAL] != NULL &&
			pool->pools[RSPAMD_MEMPOOL_NORMAL]->len > 0) {
		cur = g_ptr_array_index (pool->pools[RSPAMD_MEMPOOL_NORMAL],
				pool->pools[RSPAMD_MEMPOOL_NORMAL]->len - 1);
	}

	if (cur) {
		pool->entry->elts[pool->entry->cur_elts].leftover =
				pool_chain_free (cur);

		pool->entry->cur_elts = (pool->entry->cur_elts + 1) %
				G_N_ELEMENTS (pool->entry->elts);

		if (pool->entry->cur_elts == 0) {
			rspamd_mempool_adjust_entry (pool->entry);
		}
	}

	/* Call all pool destructors */
	for (i = 0; i < pool->destructors->len; i ++) {
		destructor = &g_array_index (pool->destructors, struct _pool_destructors, i);
		/* Avoid calling destructors for NULL pointers */
		if (destructor->data != NULL) {
			destructor->func (destructor->data);
		}
	}

	g_array_free (pool->destructors, TRUE);

	for (i = 0; i < G_N_ELEMENTS (pool->pools); i ++) {
		if (pool->pools[i]) {
			for (j = 0; j < pool->pools[i]->len; j++) {
				cur = g_ptr_array_index (pool->pools[i], j);
				g_atomic_int_add (&mem_pool_stat->bytes_allocated,
						-((gint)cur->len));
				g_atomic_int_add (&mem_pool_stat->chunks_allocated, -1);

				len = cur->len + sizeof (struct _pool_chain);

				if (i == RSPAMD_MEMPOOL_SHARED) {
					munmap ((void *)cur, len);
				}
				else {
					g_free (cur);
				}
			}

			g_ptr_array_free (pool->pools[i], TRUE);
		}
	}

	if (pool->variables) {
		g_hash_table_destroy (pool->variables);
	}

	if (pool->trash_stack) {
		for (i = 0; i < pool->trash_stack->len; i++) {
			ptr = g_ptr_array_index (pool->trash_stack, i);
			g_free (ptr);
		}

		g_ptr_array_free (pool->trash_stack, TRUE);
	}

	g_atomic_int_inc (&mem_pool_stat->pools_freed);
	POOL_MTX_UNLOCK ();
	g_free (pool);
}

void
rspamd_mempool_cleanup_tmp (rspamd_mempool_t * pool)
{
	struct _pool_chain *cur;
	guint i;

	POOL_MTX_LOCK ();

	if (pool->pools[RSPAMD_MEMPOOL_TMP]) {
		for (i = 0; i < pool->pools[RSPAMD_MEMPOOL_TMP]->len; i++) {
			cur = g_ptr_array_index (pool->pools[RSPAMD_MEMPOOL_TMP], i);
			g_atomic_int_add (&mem_pool_stat->bytes_allocated,
					-((gint)cur->len));
			g_atomic_int_add (&mem_pool_stat->chunks_allocated, -1);

			g_free (cur);
		}

		g_ptr_array_free (pool->pools[RSPAMD_MEMPOOL_TMP], TRUE);
		pool->pools[RSPAMD_MEMPOOL_TMP] = NULL;
	}

	g_atomic_int_inc (&mem_pool_stat->pools_freed);
	POOL_MTX_UNLOCK ();
}

void
rspamd_mempool_stat (rspamd_mempool_stat_t * st)
{
	if (mem_pool_stat != NULL) {
		st->pools_allocated = mem_pool_stat->pools_allocated;
		st->pools_freed = mem_pool_stat->pools_freed;
		st->shared_chunks_allocated = mem_pool_stat->shared_chunks_allocated;
		st->bytes_allocated = mem_pool_stat->bytes_allocated;
		st->chunks_allocated = mem_pool_stat->chunks_allocated;
		st->shared_chunks_allocated = mem_pool_stat->shared_chunks_allocated;
		st->chunks_freed = mem_pool_stat->chunks_freed;
		st->oversized_chunks = mem_pool_stat->oversized_chunks;
	}
}

void
rspamd_mempool_stat_reset (void)
{
	if (mem_pool_stat != NULL) {
		memset (mem_pool_stat, 0, sizeof (rspamd_mempool_stat_t));
	}
}

gsize
rspamd_mempool_suggest_size_ (const char *loc)
{
	return 0;
}

#if !defined(HAVE_PTHREAD_PROCESS_SHARED) || defined(DISABLE_PTHREAD_MUTEX)
/*
 * Own emulation
 */
static inline gint
__mutex_spin (rspamd_mempool_mutex_t * mutex)
{
	/* check spin count */
	if (g_atomic_int_dec_and_test (&mutex->spin)) {
		/* This may be deadlock, so check owner of this lock */
		if (mutex->owner == getpid ()) {
			/* This mutex was locked by calling process, so it is just double lock and we can easily unlock it */
			g_atomic_int_set (&mutex->spin, MUTEX_SPIN_COUNT);
			return 0;
		}
		else if (kill (mutex->owner, 0) == -1) {
			/* Owner process was not found, so release lock */
			g_atomic_int_set (&mutex->spin, MUTEX_SPIN_COUNT);
			return 0;
		}
		/* Spin again */
		g_atomic_int_set (&mutex->spin, MUTEX_SPIN_COUNT);
	}

#ifdef HAVE_SCHED_YIELD
	(void)sched_yield ();
#elif defined(HAVE_NANOSLEEP)
	struct timespec ts;
	ts.tv_sec = 0;
	ts.tv_nsec = MUTEX_SLEEP_TIME;
	/* Spin */
	while (nanosleep (&ts, &ts) == -1 && errno == EINTR) ;
#else
#       error No methods to spin are defined
#endif
	return 1;
}

static void
memory_pool_mutex_spin (rspamd_mempool_mutex_t * mutex)
{
	while (!g_atomic_int_compare_and_exchange (&mutex->lock, 0, 1)) {
		if (!__mutex_spin (mutex)) {
			return;
		}
	}
}

rspamd_mempool_mutex_t *
rspamd_mempool_get_mutex (rspamd_mempool_t * pool)
{
	rspamd_mempool_mutex_t *res;
	if (pool != NULL) {
		res =
			rspamd_mempool_alloc_shared (pool, sizeof (rspamd_mempool_mutex_t));
		res->lock = 0;
		res->owner = 0;
		res->spin = MUTEX_SPIN_COUNT;
		return res;
	}
	return NULL;
}

void
rspamd_mempool_lock_mutex (rspamd_mempool_mutex_t * mutex)
{
	memory_pool_mutex_spin (mutex);
	mutex->owner = getpid ();
}

void
rspamd_mempool_unlock_mutex (rspamd_mempool_mutex_t * mutex)
{
	mutex->owner = 0;
	(void)g_atomic_int_compare_and_exchange (&mutex->lock, 1, 0);
}

rspamd_mempool_rwlock_t *
rspamd_mempool_get_rwlock (rspamd_mempool_t * pool)
{
	rspamd_mempool_rwlock_t *lock;

	lock = rspamd_mempool_alloc_shared (pool, sizeof (rspamd_mempool_rwlock_t));
	lock->__r_lock = rspamd_mempool_get_mutex (pool);
	lock->__w_lock = rspamd_mempool_get_mutex (pool);

	return lock;
}

void
rspamd_mempool_rlock_rwlock (rspamd_mempool_rwlock_t * lock)
{
	/* Spin on write lock */
	while (g_atomic_int_get (&lock->__w_lock->lock)) {
		if (!__mutex_spin (lock->__w_lock)) {
			break;
		}
	}

	g_atomic_int_inc (&lock->__r_lock->lock);
	lock->__r_lock->owner = getpid ();
}

void
rspamd_mempool_wlock_rwlock (rspamd_mempool_rwlock_t * lock)
{
	/* Spin on write lock first */
	rspamd_mempool_lock_mutex (lock->__w_lock);
	/* Now we have write lock set up */
	/* Wait all readers */
	while (g_atomic_int_get (&lock->__r_lock->lock)) {
		__mutex_spin (lock->__r_lock);
	}
}

void
rspamd_mempool_runlock_rwlock (rspamd_mempool_rwlock_t * lock)
{
	if (g_atomic_int_get (&lock->__r_lock->lock)) {
		(void)g_atomic_int_dec_and_test (&lock->__r_lock->lock);
	}
}

void
rspamd_mempool_wunlock_rwlock (rspamd_mempool_rwlock_t * lock)
{
	rspamd_mempool_unlock_mutex (lock->__w_lock);
}
#else

/*
 * Pthread bases shared mutexes
 */
rspamd_mempool_mutex_t *
rspamd_mempool_get_mutex (rspamd_mempool_t * pool)
{
	rspamd_mempool_mutex_t *res;
	pthread_mutexattr_t mattr;

	if (pool != NULL) {
		res =
			rspamd_mempool_alloc_shared (pool, sizeof (rspamd_mempool_mutex_t));

		pthread_mutexattr_init (&mattr);
		pthread_mutexattr_setpshared (&mattr, PTHREAD_PROCESS_SHARED);
		pthread_mutexattr_setrobust (&mattr, PTHREAD_MUTEX_ROBUST);
		pthread_mutex_init (res, &mattr);
		rspamd_mempool_add_destructor (pool,
				(rspamd_mempool_destruct_t)pthread_mutex_destroy, res);
		pthread_mutexattr_destroy (&mattr);

		return res;
	}
	return NULL;
}

void
rspamd_mempool_lock_mutex (rspamd_mempool_mutex_t * mutex)
{
	pthread_mutex_lock (mutex);
}

void
rspamd_mempool_unlock_mutex (rspamd_mempool_mutex_t * mutex)
{
	pthread_mutex_unlock (mutex);
}

rspamd_mempool_rwlock_t *
rspamd_mempool_get_rwlock (rspamd_mempool_t * pool)
{
	rspamd_mempool_rwlock_t *res;
	pthread_rwlockattr_t mattr;

	if (pool != NULL) {
		res =
			rspamd_mempool_alloc_shared (pool, sizeof (rspamd_mempool_rwlock_t));

		pthread_rwlockattr_init (&mattr);
		pthread_rwlockattr_setpshared (&mattr, PTHREAD_PROCESS_SHARED);
		pthread_rwlock_init (res, &mattr);
		rspamd_mempool_add_destructor (pool,
				(rspamd_mempool_destruct_t)pthread_rwlock_destroy, res);
		pthread_rwlockattr_destroy (&mattr);

		return res;
	}
	return NULL;
}

void
rspamd_mempool_rlock_rwlock (rspamd_mempool_rwlock_t * lock)
{
	pthread_rwlock_rdlock (lock);
}

void
rspamd_mempool_wlock_rwlock (rspamd_mempool_rwlock_t * lock)
{
	pthread_rwlock_wrlock (lock);
}

void
rspamd_mempool_runlock_rwlock (rspamd_mempool_rwlock_t * lock)
{
	pthread_rwlock_unlock (lock);
}

void
rspamd_mempool_wunlock_rwlock (rspamd_mempool_rwlock_t * lock)
{
	pthread_rwlock_unlock (lock);
}
#endif

void
rspamd_mempool_set_variable (rspamd_mempool_t *pool,
	const gchar *name,
	gpointer value,
	rspamd_mempool_destruct_t destructor)
{
	if (pool->variables == NULL) {
		pool->variables = g_hash_table_new (rspamd_str_hash, rspamd_str_equal);
	}

	g_hash_table_insert (pool->variables, rspamd_mempool_strdup (pool,
		name), value);
	if (destructor != NULL) {
		rspamd_mempool_add_destructor (pool, destructor, value);
	}
}

gpointer
rspamd_mempool_get_variable (rspamd_mempool_t *pool, const gchar *name)
{
	if (pool->variables == NULL) {
		return NULL;
	}

	return g_hash_table_lookup (pool->variables, name);
}

void
rspamd_mempool_remove_variable (rspamd_mempool_t *pool, const gchar *name)
{
	if (pool->variables != NULL) {
		g_hash_table_remove (pool->variables, name);
	}
}

GList *
rspamd_mempool_glist_prepend (rspamd_mempool_t *pool, GList *l, gpointer p)
{
	GList *cell;

	cell = rspamd_mempool_alloc (pool, sizeof (*cell));
	cell->prev = NULL;
	cell->data = p;

	if (l == NULL) {
		cell->next = NULL;
	}
	else {
		cell->next = l;
		l->prev = cell;
	}

	return cell;
}

GList *
rspamd_mempool_glist_append (rspamd_mempool_t *pool, GList *l, gpointer p)
{
	GList *cell, *cur;

	cell = rspamd_mempool_alloc (pool, sizeof (*cell));
	cell->next = NULL;
	cell->data = p;

	if (l) {
		for (cur = l; cur->next != NULL; cur = cur->next) {}
		cur->next = cell;
		cell->prev = cur;
	}
	else {
		l = cell;
		l->prev = NULL;
	}

	return l;
}
