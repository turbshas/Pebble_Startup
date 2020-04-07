#include "alloc.h"
#include "mem_mgr.h"
/*
 * For memoy allocation, a skip list will be used with 4 different free lists:
 * 0 B, 16 B, 64 B, and 1024 B
 *
 * A block of memory greater than or equal to the size of the ith list will be
 * placed on that list and will point to the next item in that list. Each list
 * will be singly-linked and only contain free blocks of memory.
 *
 * For example: (* denotes NULL)
 *          ______     _______     ______     ______     ______     ______
 *    0 -->|      |-->|       |-->|      |-->|      |-->|      |-->|   *  |
 *         | 1024 |   |    8  |   |  32  |   |  128 |   | 4096 |   |  40  |
 *   16 -->|      |---|       |-->|      |-->|      |-->|      |-->|   *  |
 *         |      |   |       |   |      |   |      |   |      |   |      |
 *   64 -->|      |---|       |---|      |-->|      |-->|   *  |   |      |
 *         |      |   |       |   |      |   |      |   |      |   |      |
 * 1024 -->|      |---|       |---|      |---|      |-->|   *  |   |      |
 *         |______|   |_______|   |______|   |______|   |______|   |______|
 */

#define SRAM_SIZE (128 * 1024)
#define NUM_FREE_LISTS 4u
#define MIN_ALLOC_SIZE 8u

#define MALLOC_HEADER_SIZE (2 * sizeof(size_t))
#define ALIGNMENT (sizeof(size_t))
#define ALIGNMENT_MASK (ALIGNMENT - 1u)
#define UNALIGNED(p) (((uintptr_t)p) & ALIGNMENT_MASK)
#define ROUND_UP_TO_ALIGN(size) ((((size) - 1u) & ~ALIGNMENT_MASK) + ALIGNMENT)

extern unsigned int _ALLOCABLE_MEM;
extern unsigned int _DATA_RAM_START;

//TODO: can do this with a lookup table?
static unsigned
which_skiplist_by_size(const size_t size) {
    if (size >= 1024) {
        return 3;
    } else if (size >= 64) {
        return 2;
    } else if (size >= 16) {
        return 1;
    } else {
        return 0;
    }
}

class Skiplist {
    public:
        void *malloc(const size_t size);
        void *resize(const size_t old_size, const size_t new_size, void *const p);
        void free(const size_t size, void *const p);

    private:
        /*
         * This struct will be placed at the beginning of each free entry.
         * Thus, it will never be allocated explicitly. A pointer to the
         * beginning of the entry will be cast to a pointer this header.
         *
         * Therefore, the size of the "next" array will depend on the size
         * of the entry itself. The array is declared as being of length
         * NUM_FREE_LISTS to supress compiler warnings.
         */
        struct free_entry {
            size_t size;
            free_entry *next[NUM_FREE_LISTS];

            public:
                free_entry(const free_entry& fe);
                void copy_from(const free_entry& fe);
                unsigned skiplist() const { return which_skiplist_by_size(size); };
        };

        struct list_links {
            /* Each pointer points to a next value of a free_entry element */
            free_entry **lists[NUM_FREE_LISTS];

            public:
                list_links(const Skiplist &list);
        };

        struct list_walker {
            unsigned skiplist_num;
            free_entry *curr_block;
            list_links links;

            private:
                void advance_links();

            public:
                list_walker(const unsigned skip_list, const Skiplist &list_start);
                bool fits_size(const size_t size) const { return curr_block->size >= size; };
                void move_next();
        };

        size_t total_mem;
        size_t total_free;
        free_entry *heads[NUM_FREE_LISTS];

        list_walker get_walker(const unsigned skip_list) const;

        void allocate_entire_block(list_walker& lw);
        void *allocate_current(list_walker& lw, const size_t size);

        void insert_new_block(list_walker& lw, free_entry& new_block, const size_t size);

        void insert_and_coalesce_with_current(list_walker& lw, free_entry& entry, const size_t size);
        void free_current(list_walker& lw, const size_t size);

        void copy_and_resize(list_walker& lw, free_entry& dest, const free_entry& src, const size_t new_size);
        void expand_entry(list_walker& lw, free_entry& entry, const size_t expand_amt);
        void shrink_entry(list_walker& lw, free_entry& entry, const size_t shrink_amt);

        void resize_allocated_block(list_walker& lw, const free_entry *const allocated_block, const size_t old_size, const size_t new_size);

        /* TODO: clean these function up */
};

Skiplist::free_entry::free_entry(const free_entry& fe)
{
    copy_from(fe);
}

void
Skiplist::free_entry::copy_from(const free_entry& fe)
{
    size = fe.size;
    for (unsigned i = 0; i < fe.skiplist(); i++) {
        next[i] = fe.next[i];
    }
}

Skiplist::list_links::list_links(const Skiplist &list)
{
    for (unsigned i = 0; i < NUM_FREE_LISTS; i++) {
        lists[i] = const_cast<free_entry **>(&list.heads[i]);
    }
}

Skiplist::list_walker::list_walker(const unsigned skip_list, const Skiplist &list_start)
    : skiplist_num(skip_list),
      curr_block(list_start.heads[skip_list]),
      links(list_start) { }

void
Skiplist::list_walker::move_next()
{
    curr_block = curr_block->next[skiplist_num];
    advance_links();
}

void
Skiplist::list_walker::advance_links()
{
    for (unsigned i = 0; i < NUM_FREE_LISTS; i++) {
        if (*(links.lists[i]) < curr_block) {
            /* Advance the links forward, but only if they don't pass p.
             * This is because the links will be used to update the next
             * pointers in the list once an entry is allocated, so we need
             * to stay behind p.
             */
            struct free_entry *next_entry = *(links.lists[i]);
            links.lists[i] = &next_entry->next[i];
        }
    }
}

void
Skiplist::insert_and_coalesce_with_current(list_walker& lw, free_entry& entry, const size_t size)
{
    entry.size = size + lw.curr_block->size;
    const unsigned curr_block_skiplist = lw.curr_block->skiplist();
    const unsigned new_skiplist = entry.skiplist();

    /* Update next pointers:
     *   - Need to copy the next pointers the curr_block had
     *   - For the other next pointers, copy the pointers from the links
     *   - Update links to point to entry up to its skiplist
     */
    for (unsigned i = 0; i <= curr_block_skiplist; i++) {
        entry.next[i] = lw.curr_block->next[i];
        *(lw.links.lists[i]) = &entry;
    }

    for (unsigned i = (curr_block_skiplist + 1); i <= new_skiplist; i++) {
        entry.next[i] = *(lw.links.lists[i]);
        *(lw.links.lists[i]) = &entry;
    }

    lw.curr_block = &entry;
}

void
Skiplist::expand_entry(list_walker& lw, free_entry& entry, const size_t expand_amt)
{
    const unsigned old_skiplist = entry.skiplist();
    entry.size += expand_amt;
    const unsigned new_skiplist = entry.skiplist();

    for (unsigned i = (old_skiplist + 1); i <= new_skiplist; i++) {
        entry.next[i] = *(lw.links.lists[i]);
        *(lw.links.lists[i]) = &entry;
    }
}

void
Skiplist::shrink_entry(list_walker& lw, free_entry& entry, const size_t shrink_amt)
{
    const unsigned old_skiplist = entry.skiplist();
    entry.size -= shrink_amt;
    const unsigned new_skiplist = entry.skiplist();

    for (unsigned i = (new_skiplist + 1); i <= old_skiplist; i++) {
        *(lw.links.lists[i]) = entry.next[i];
    }
}

void
Skiplist::insert_new_block(list_walker& lw, free_entry& new_block, const size_t size)
{
    new_block.size = size;
    for (unsigned i = 0; i <= new_block.skiplist(); i++) {
        new_block.next[i] = *(lw.links.lists[i]);
        *(lw.links.lists[i]) = &new_block;
    }
}

void
Skiplist::allocate_entire_block(list_walker& lw)
{
    for (unsigned i = 0; i <= lw.curr_block->skiplist(); i++) {
        *(lw.links.lists[i]) = lw.curr_block->next[i];
    }
}

void *
Skiplist::allocate_current(list_walker& lw, const size_t size)
{
    if (lw.curr_block->size < (size + MIN_ALLOC_SIZE)) {
        /*
         * If we allocated this block, the leftover would
         * be too small. Allocate the whole thing
         */

        /* Update pointers that pointed at p to point to the block after p */
        allocate_entire_block(lw);
    } else {
        /* We need to split the block */

        /* Copy values to intermediate in case of overlap */
        free_entry temp_entry(*(lw.curr_block));

        /* Set the values for the new entry */
        const uintptr_t curr_block_int = reinterpret_cast<uintptr_t>(lw.curr_block);
        free_entry *new_entry = reinterpret_cast<free_entry *>(curr_block_int + size);
        copy_and_resize(lw, *new_entry, temp_entry, temp_entry.size - size);
    }

    return lw.curr_block;
}

void
Skiplist::copy_and_resize(list_walker& lw, free_entry& dest, const free_entry& src, const size_t new_size)
{
    if (new_size == src.size) {
        dest.copy_from(src);
        return;
    }

    dest.size = new_size;
    const bool expanding = new_size > src.size;
    const unsigned old_skiplist = src.skiplist();
    const unsigned new_skiplist = dest.skiplist();

    if (expanding) {
        for (unsigned i = 0; i <= old_skiplist; i++) {
            dest.next[i] = src.next[i];
            *(lw.links.lists[i]) = &dest;
        }
        for (unsigned i = (old_skiplist + 1); i <= new_skiplist; i++) {
            dest.next[i] = *(lw.links.lists[i]);
            *(lw.links.lists[i]) = &dest;
        }
    } else {
        for (unsigned i = 0; i <= new_skiplist; i++) {
            dest.next[i] = src.next[i];
            *(lw.links.lists[i]) = &dest;
        }
        for (unsigned i = (new_skiplist + 1); i <= old_skiplist; i++) {
            *(lw.links.lists[i]) = src.next[i];
        }
    }
}

void
Skiplist::resize_allocated_block(list_walker& lw, const free_entry *const allocated_block, const size_t old_size, const size_t new_size)
{
    const uintptr_t ab_int = reinterpret_cast<uintptr_t>(allocated_block);
    const uintptr_t curr_block_int = reinterpret_cast<uintptr_t>(lw.curr_block);
    if ((ab_int + old_size) != curr_block_int) {
        /* allocated_block must be adjacent to the currently selected block */
        return;
    }

    if (old_size > new_size) {
        /* Shrinking p */
        const unsigned size_diff = old_size - new_size;
        /* Copy values over using temp as intermediary */
        free_entry temp(*(lw.curr_block));

        free_entry *new_block = reinterpret_cast<free_entry *>(curr_block_int - size_diff);
        copy_and_resize(lw, *new_block, temp, temp.size - size_diff);

        lw.curr_block = new_block;
    } else {
        /* Extending p */
        const unsigned size_diff = new_size - old_size;
        if ((lw.curr_block->size - size_diff) < MIN_ALLOC_SIZE) {
            /* Allocate all of curr_block */
            allocate_entire_block(lw);
        } else {
            /* Copy data into temp to avoid overlap */
            free_entry temp(*(lw.curr_block));

            free_entry *new_block = reinterpret_cast<free_entry *>(curr_block_int + size_diff);
            copy_and_resize(lw, *new_block, temp, temp.size - size_diff);

            lw.curr_block = new_block;
        }
    }
}

void
Skiplist::free_current(list_walker& lw, const size_t size)
{
}

Skiplist::list_walker
Skiplist::get_walker(const unsigned skip_list) const
{
    list_walker lw(skip_list, *this);
    return lw;
}

/* Start of allocable memory block (and size), maybe make this a macro? */
static void *const ALLOCABLE_MEM_START = &_ALLOCABLE_MEM;
static size_t ALLOCABLE_MEM_SIZE;

/* Entry point for each skip list */
static Skiplist free_list_start;

/*
 * Planned interface:
 *  1) ker_malloc:
 *    - params: size of memory
 *    - traverse free list until past start
 *    - find first free entry of sufficient size to allocate request
 *    - if exact match or leftover would be too small: allocate entire entry
 *    - else fragment entry, allocate first chunk
 *    - previous entries need to be updated to point to correct next entry
 *      - use 2 list_links objects, 1 behind the other
 *  2) ker_calloc:
 *    - params: size of memory
 *    - calls ker_malloc, zeroes out memory
 *  3) ker_realloc:
 *    - params: old size of memory, new size of memory, old pointer
 *    - if there is a block directly next to old one (and it fits), expand it
 *    - else, allocate new block, copy data over, and free old block
 *  4) ker_free:
 *    - params: size, pointer
 *    - find spot where block should go in free list
 *    - if contiguous with next block, merge with next block
 *    - if contiguous with previous block, merge with previous
 *    - else, make it a new block and insert into list, update pointers
 *
 * Notes:
 *  - Need minimum alloc size (size of size_t + size of void *) - in this case, 8 B
 *  - For processes allocating memory, may need functions that allocate requested
 *    size + header size, and the header stores the size of the memory allocated
 *    for convenient freeing by process (free would only need the pointer)
 *      - Allocates size + MALLOC_HEADER_SIZE, stores the allocated size in the 4 B
 *        preceding the returned pointer
 *  - May need similar thing for realloc - inner function accepts old memory size
 *    as well as new memory size
 *  - Start location of allocable memory will be communicated by the linker script
 *    (by communicating end of memory used in the OS binary) and size will be
 *    computed from this
 * TODO: make sure allocated blocks are multiples of 4 B for alignment and also
 * that freed pointers are aligned correctly (and realloc'd ones too)
 */

/*
 * The ker_* functions expect proper input values, should only be called
 * from the _* functions at the bottom of the file
 */
void *
Skiplist::malloc(const size_t size) {
    const unsigned skip_list = which_skiplist_by_size(size);

    list_walker lw = get_walker(skip_list);

    while (lw.curr_block) {
        if (lw.fits_size(size)) {
            /* We can use this block of memory */
            return allocate_current(lw, size);
        }
        lw.move_next();
    }

    /* Didn't find a valid spot */
    return nullptr;
}

void
Skiplist::free(const size_t size, void *const pointer_to_free)
{
    free_entry *const p = static_cast<free_entry *>(pointer_to_free);

    /* Go through lowest skip list to make sure we don't skip our spot */
    list_walker lw = get_walker(0);

    while (lw.curr_block && (lw.curr_block <= p)) {
        lw.move_next();
    }

    /* Get pointer to block previous to p */
    const uintptr_t prev_int = reinterpret_cast<uintptr_t>(lw.links.lists[0]) - offsetof(free_entry, next);
    free_entry *const prev = reinterpret_cast<free_entry *>(prev_int);
    /* uint versions of pointers for comparisons */
    const uintptr_t p_int = reinterpret_cast<uintptr_t>(p);
    const uintptr_t curr_block_int = reinterpret_cast<uintptr_t>(lw.curr_block);

    if (lw.curr_block != nullptr) {
        /* Freed memory block belongs just before curr_block */

        if ((prev_int + prev->size) == p_int) {
            /* Can coalesce freed block with previous block */

            if ((p_int + size) == curr_block_int) {
                const unsigned prev_skip_list = prev->skiplist();
                prev->size += size;
                /* Can coalesce with next block */
                prev->size += lw.curr_block->size;
                const unsigned curr_block_skip_list = lw.curr_block->skiplist();
                const unsigned new_skip_list = prev->skiplist();

                /* Update next pointers */
                for (unsigned i = 0; i <= curr_block_skip_list; i++) {
                    prev->next[i] = lw.curr_block->next[i];
                }

                for (unsigned i = (curr_block_skip_list + 1); i <= new_skip_list; i++) {
                    prev->next[i] = *(lw.links.lists[i]);
                }

                /* Set previous entries to point to prev */
                for (unsigned i = (prev_skip_list + 1); i <= new_skip_list; i++) {
                    *(lw.links.lists[i]) = prev;
                }
            } else {
                /* Expand previous to fill the space */
                expand_entry(lw, *prev, size);
            }
        } else {
            /* Can't coalesce with previous, set values */
            if ((p_int + size) == curr_block_int) {
                /* Can coalesce with next block */
                insert_and_coalesce_with_current(lw, *p, size);
            } else {
                /* Can't coalesce with any blocks, insert new one */
                insert_new_block(lw, *p, size);
            }
        }
    } else {
        /* We got to the end of the list, so this block must belong on the end */
        if ((prev_int + size) == p_int) {
            /* Can coalesce with previous */
            expand_entry(lw, *prev, size);
        } else {
            /* Need to create new block */
            insert_new_block(lw, *p, size);
        }
    }
}

void *
Skiplist::resize(const size_t old_size, const size_t new_size, void *const pointer_to_resize)
{
    //TODO: this can be simplified a lot.
    // if (expanding) {
    //     new_p = malloc(new_size);
    //     copy_data();
    //     return new_p;
    // } else {
    //     if (size_diff < MIN_ALLOC_SIZE) return p;
    //     else {
    //         free(p + size_diff, size_diff);
    //         p->size -= size_diff;
    //     }
    // }
    free_entry *const p = static_cast<free_entry *>(pointer_to_resize);
    const unsigned old_skip_list = which_skiplist_by_size(old_size);
    const bool expanding = new_size > old_size;

    list_walker lw = get_walker(old_skip_list);

    /* Find free block following p */
    while (lw.curr_block && (lw.curr_block <= p)) {
        lw.move_next();
    }

    /* Memory block belongs just before traverse */
    const uintptr_t p_int = reinterpret_cast<uintptr_t>(p);
    const uintptr_t curr_block_int = reinterpret_cast<uintptr_t>(lw.curr_block);
    if ((lw.curr_block != nullptr) && ((p_int + old_size) == curr_block_int)) {
        /* Following block is free and is adjacent to p, extend/shrink p */
        resize_allocated_block(lw, p, old_size, new_size);
        return p;
    } else {
        /* Not connected, will need to create a new free_entry */
        if (expanding) {
            /* Can't resize, caller will need to allocate new block,
             * copy data over, then free the old one.
             */
            return nullptr;
        } else {
            /* If the size difference is < min allocable size, just return p */
            const size_t size_diff = old_size - new_size;
            if (size_diff < MIN_ALLOC_SIZE) {
                return p;
            }
            /* Create a new block following p */
            const uintptr_t new_block_int = p_int + new_size;
            free_entry *new_block = reinterpret_cast<free_entry *>(new_block_int);

            insert_new_block(lw, *new_block, size_diff);

            return p;
        }
    }

    return nullptr;
}

/* Initializes structures required for allocator to work */
//TODO: this list will be used for heap allocations and will need memory to be allocated from mem_mgr before use
//TODO: move ALLOCABLE_MEM_SIZE to mem_mgr
void
alloc_init(void) {
    ALLOCABLE_MEM_SIZE = ((uintptr_t)&_DATA_RAM_START) + SRAM_SIZE - ((uintptr_t)&_ALLOCABLE_MEM);

    /* TODO: put this in mem_mgr
    free_entry *entry = reinterpret_cast<free_entry *>(ALLOCABLE_MEM_START);
    entry->size = ALLOCABLE_MEM_SIZE;

    for (unsigned i = 0; i < NUM_FREE_LISTS; i++) {
        free_list_start.heads[i] = entry;
        entry->next[i] = nullptr;
    }
    */
}

void *
_malloc(const size_t req_size) {
    if (req_size == 0) {
        return NULL;
    }

    const size_t size = ROUND_UP_TO_ALIGN(req_size) + MALLOC_HEADER_SIZE;

    size_t *p = static_cast<size_t *>(free_list_start.malloc(size));
    p[0] = size;
    const uintptr_t p_int = reinterpret_cast<uintptr_t>(p);
    return reinterpret_cast<void *>(p_int + MALLOC_HEADER_SIZE);
    /* The way I expect locking to work:
     * 1) lock
     * 2) ker_malloc()
     * 3) unlock
     * 4) If p == NULL:
     *     a) allocate more memory to heap
     *     b) lock
     *     c) ker_free() on newly allocated memory
     *     d) unlock
     *     e) lock
     *     f) ker_malloc()
     *     g) unlock
     *     h) if p == NULL return NULL else return p
     * 5) Else return p
     */
}

void *
_calloc(const size_t req_size) {
    if (req_size == 0) {
        return NULL;
    }

    const size_t size = ROUND_UP_TO_ALIGN(req_size) + MALLOC_HEADER_SIZE;

    size_t *p = static_cast<size_t *>(free_list_start.malloc(size));
    if (p == nullptr) {
        return nullptr;
    }

    /* p is guaranteed to be a multiple of size_t bytes */
    const size_t count = size / ALIGNMENT;
    for (unsigned i = 1; i < count; i++) {
        size_t *c = p;
        c[i] = 0;
    }
    p[0] = size;
    const uintptr_t p_int = reinterpret_cast<uintptr_t>(p);
    return reinterpret_cast<void *>(p_int + MALLOC_HEADER_SIZE);
}

void
_free(void *const p) {
    if (UNALIGNED(p) || (p == nullptr)) {
        /* Just do nothing... not sure what the "right" thing is */
        return;
    }
    const uintptr_t p_int = reinterpret_cast<uintptr_t>(p);
    size_t *const q = reinterpret_cast<size_t *>(p_int - MALLOC_HEADER_SIZE);
    const size_t size = q[0];

    free_list_start.free(size, static_cast<void *>(q));
}

void *
_realloc(const size_t req_size, void *const p) {
    if (!p) {
        return _malloc(req_size);
    } else if (req_size == 0) {
        /* p is a valid pointer and requested size is zero: free block of memory */
        _free(p);
        return nullptr;
    } else if (UNALIGNED(p)) {
        /* Just do nothing... not sure what the "right" thing is */
        return p;
    }

    const uintptr_t p_int = reinterpret_cast<uintptr_t>(p);
    size_t *const q = reinterpret_cast<size_t *>(p_int - MALLOC_HEADER_SIZE);
    const size_t old_size = q[0];
    const size_t new_size = ROUND_UP_TO_ALIGN(req_size) + MALLOC_HEADER_SIZE;
    if (new_size == old_size) {
        /* Same size requested, do nothing */
        return p;
    }
    /* Actual realloc */
    size_t *ret = static_cast<size_t *>(free_list_start.resize(old_size, new_size, static_cast<void *>(q)));
    if (ret == nullptr) {
        /* Need to allocate new block */
        ret = static_cast<size_t *>(free_list_start.malloc(new_size));
        if (ret == nullptr) {
            /* Couldn't allocate more mem */
            return nullptr;
        }
        /* First slot is for storing size of block allocated */
        ret[0] = new_size;

        /* Copy data over */
        size_t copy_size;
        if (new_size < old_size)  {
            copy_size = new_size;
        } else {
            copy_size = old_size;
        }

        size_t count = (copy_size - MALLOC_HEADER_SIZE) / sizeof(size_t);
        const uintptr_t ret_int = reinterpret_cast<uintptr_t>(ret);
        size_t *const r = reinterpret_cast<size_t *>(ret_int + MALLOC_HEADER_SIZE);
        size_t *const c = static_cast<size_t *>(p);
        for (size_t i = 0; i < count; i++) {
            r[i] = c[i];
        }

        /* Free old mem */
        free_list_start.free(old_size, static_cast<void *>(q));

        return static_cast<void *>(r);
    }

    return ret;
}

