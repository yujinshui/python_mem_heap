#include "lmem_heap.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>



#ifndef LIKELY
#define LIKELY(x) __builtin_expect (!!(x), 1)
#endif /* !JERRY_LIKELY */

#ifndef UNLIKELY
#define UNLIKELY(x) __builtin_expect (!!(x), 0)
#endif /* !JERRY_UNLIKELY */

#undef uchar
#define uchar uint8_t /* 约8位 */

#undef uint
#define uint uint16_t /* 约大于等于16位 */

#undef ulong
#define ulong  uint32_t /* 约大于等于32位 */

#undef uptr
#define uptr uintptr_t

typedef uchar block;


typedef struct pool_header* poolp;


/*
 *   define macros
 */
#define ARENA_SIZE (256 << 10) /* 256K 字节 */
#define INITIAL_ARENA_OBJECTS 16
#define SYSTEM_PAGE_SIZE (4 * 1024)
#define POOL_SIZE SYSTEM_PAGE_SIZE /* must be 2^N */
#define SYSTEM_PAGE_SIZE_MASK (SYSTEM_PAGE_SIZE - 1)
#define POOL_SIZE_MASK SYSTEM_PAGE_SIZE_MASK
#define POOL_ADDR(P) ((poolp)((uptr)(P) & ~(uptr)POOL_SIZE_MASK))


#define ALIGNMENT 8 /* 有必要为2的N次方 */
#define ALIGNMENT_MASK  (ALIGNMENT - 1)
#define ALIGNMENT_SHIFT 3
#define SMALL_REQUEST_THRESHOLD 256
#define NB_SMALL_SIZE_CLASSES (SMALL_REQUEST_THRESHOLD / ALIGNMENT)
#define PT(x) PTA(x), PTA(x)
#define PTA(x) ((poolp )((uchar *)&(usedpools[2*(x)]) - 2*sizeof(block *)))
#define DUMMY_SIZE_IDX 0xffff
#define INDEX2SIZE(I) (((uint)(I) + 1) << ALIGNMENT_SHIFT)

#define ROUNDUP(x) (((x) + ALIGNMENT_MASK) & ~ALIGNMENT_MASK)   //向上取8的倍数
#define POOL_OVERHEAD ROUNDUP(sizeof(struct pool_header))


#define MIN(x, y) ((x) < (y) ? (x) : (y))
    



struct pool_header { 
    /* 分配到pool里的block的数量 */ 
    union { 
        block *_padding; 
        uint count; 
    } ref;
    
    /* block的空闲链表的开头 */ 
    block *freeblock; 
    
    /* 指向下一个pool的指针（双向链表） */ 
    struct pool_header *nextpool; 
    
    /* 指向前一个pool的指针（双向链表） */ 
    struct pool_header *prevpool; 
    
    /* 自己所属的arena的索引（对于arenas而言） */ 
    uint arenaindex; 
    
    /* 分配的block的大小 */
    uint szidx; 
    
    /* 到下一个block的偏移 */ 
    uint nextoffset; 
    
    /* 到能分配下一个block之前的偏移 */ 
    uint maxnextoffset;
};

struct arena_object { 
    /* malloc后的arena的地址 */
    uptr address; 
    
    /* 将arena的地址用于给pool使用而对齐的地址 */ 
    block* pool_address; 
    
    /* 此arena中空闲的pool数量 */ 
    uint nfreepools; 
    
    /* 此arena中pool的总数 */
    uint ntotalpools; 
    
    /* 连接空闲pool的单向链表 */ 
    struct pool_header* freepools; 
    
    /* 稍后说明 */ 
    struct arena_object* nextarena; 
    struct arena_object* prevarena;
};

/*
 *  define global variables
 */

/* 将arena_object作为元素的数组 */
static struct arena_object* arenas = NULL;
/* arenas的元素数量 */
static uint maxarenas = 0;
static struct arena_object* unused_arena_objects = NULL;
static struct arena_object* usable_arenas = NULL;


static poolp usedpools[2 * ((NB_SMALL_SIZE_CLASSES + 7) / 8) * 8] = { 
    PT(0), PT(1), PT(2), PT(3), PT(4), PT(5), PT(6), PT(7),
    PT(8), PT(9), PT(10), PT(11), PT(12), PT(13), PT(14), PT(15),
    PT(16), PT(17), PT(18), PT(19), PT(20), PT(21), PT(22), PT(23),
    PT(24), PT(25), PT(26), PT(27), PT(28), PT(29), PT(30), PT(31),
};


/*
 *  declare static functions
 */
static struct arena_object* new_arena(void);
static int address_in_range(void *p, poolp pool);


/**
 * @brief 自定义内存分配函数
 *
 * 该函数用于分配指定大小的内存块。如果请求的内存大小小于等于256字节，它将尝试从预先分配的内存池中分配。
 * 如果内存池中没有可用的内存块，它将尝试从arena中获取新的内存池。如果请求的内存大小大于256字节，它将调用标准库的malloc函数。
 *
 * @param nbytes 需要分配的内存大小（以字节为单位）
 * @return void* 分配的内存块的指针，如果分配失败则返回NULL
 */
void *lmem_malloc(size_t nbytes) {
    block *bp;
    poolp next;
    /* 是否小于等于256字节？ */ 
    if (LIKELY((nbytes - 1) < SMALL_REQUEST_THRESHOLD)) {  
        // LOCK(); /* 线程锁 */ 
        /* 变换成索引 */ 
        size_t size = (uint)(nbytes - 1) >> ALIGNMENT_SHIFT; 
        poolp pool = usedpools[size + size]; 
     
        /* 取出pool */ 
        if (LIKELY(pool != pool->nextpool)) { 
            /* 返回pool内的block */ 
            /* pool内分配的block的数量 */ 
            ++pool->ref.count;   
            bp = pool->freeblock; 
            /* 通过空闲链表取出block（使用完毕的block） */ 
            if (LIKELY((pool->freeblock = *(block **)bp) != NULL )) { 
                // UNLOCK(); /* 解除线程锁 */  
                return (void *)bp; 
            }
            /* 通过偏移量取出block(未使用的block) */  
            if (LIKELY(pool->nextoffset <= pool->maxnextoffset)) { 
                pool->freeblock = (block*)pool + pool->nextoffset;  
                /* 设定到下一个空block的偏移量 */  
                pool->nextoffset += INDEX2SIZE(size); 
                *(block **)(pool->freeblock) = NULL; 
                // UNLOCK();  
                return (void *)bp; 
            } 
            /* 没有能分配到pool内的block了 */ 
            next = pool->nextpool; 
            pool = pool->prevpool; 
            next->prevpool = pool;     
            pool->nextpool = next;    
            // UNLOCK(); 
            return (void *)bp;
        } 
        /* 是否存在可以使用的arena？ */ 
        if (UNLIKELY(usable_arenas == NULL))  { 
            /* 分配新的arena_object */ 
            usable_arenas = new_arena(); 
            if (UNLIKELY(usable_arenas == NULL)) { 
                // UNLOCK(); 
                goto redirect; 
            } 
            usable_arenas->nextarena = usable_arenas->prevarena = NULL; 
        }
        /* 从arena取出使用完毕的pool */ 
        pool = usable_arenas->freepools; 
        /* 是否存在使用完毕的pool？ */ 
        if (LIKELY(pool != NULL)) { 
            /* 初始化使用完毕的pool */ 
            /* 把使用完毕的pool从链表中取出 */  
            usable_arenas->freepools = pool->nextpool;
            /* 从arena内可用的pool数中减去一个 */  
            --usable_arenas->nfreepools; 
            if (UNLIKELY(usable_arenas->nfreepools == 0)) { 
                /* 设定下一个arena */ 
                usable_arenas = usable_arenas->nextarena; 
                if (usable_arenas != NULL) { 
                    usable_arenas->prevarena = NULL; 
                } 
            }
            init_pool:
            /* (E)初始化pool并返回block */ 
            next = usedpools[size + size];  /* == prev */ 
            pool->nextpool = next; 
            pool->prevpool = next;  
            next->nextpool = pool; 
            next->prevpool = pool; 
            pool->ref.count = 1;  
            if (UNLIKELY(pool->szidx == size)) { 
                /* 比较申请的大小和pool中固定的block大小， 
                 * 如果大小一样，那么不初始化也无所谓 
                */ 
                bp = pool->freeblock; 
                /* 设定下一个空block的地址 */ 
                pool->freeblock = *(block **)bp; 
                // UNLOCK(); 
                return (void *)bp; 
            } 
            pool->szidx = size;  
            size = INDEX2SIZE(size);  //相当于向8字节取整了
            bp = (block *)pool + POOL_OVERHEAD; 
            pool->nextoffset  = POOL_OVERHEAD + (size << 1); 
            pool->maxnextoffset = POOL_SIZE - size; 
            pool->freeblock = bp + size;   
            *(block **)(pool->freeblock) = NULL; 
            // UNLOCK();   
            return (void *)bp;
        } 


        /* 初始化空pool */ 
        pool = (poolp)usable_arenas->pool_address; 
        /* 设定arena_object的位置 */ 
        pool->arenaindex = usable_arenas - arenas; 
        /* 输入一个虚拟的大值 */ 
        pool->szidx = DUMMY_SIZE_IDX; 
        usable_arenas->pool_address += POOL_SIZE; 
        --usable_arenas->nfreepools;   
        if (UNLIKELY(usable_arenas->nfreepools == 0)) { 
            /* 如果没有可用的pool，就设定下一个arena */ 
            usable_arenas = usable_arenas->nextarena; 
            if (LIKELY(usable_arenas != NULL)) {  
                usable_arenas->prevarena = NULL; 
            } 
        } 
        goto init_pool; 
    }



redirect: /* 当大于等于256字节时，按一般情况调用malloc */ 
        return (void *)malloc(nbytes);
}


/**
 * @brief 释放内存块
 *
 * 该函数用于释放之前通过lmem_alloc分配的内存块。如果内存块属于某个内存池，则将其重新连接到该内存池的空闲块链表中。
 * 如果内存池中的所有块都已释放，则将该内存池返回给arena对象以便后续重用。
 * 如果内存块不属于任何内存池，则直接使用标准库函数free释放。
 *
 * @param p 要释放的内存块的指针。如果为NULL，则不执行任何操作。
 */
void lmem_free(void *p) {
    block *lastfree;
    poolp next, prev; 
    struct arena_object* ao; 
    uint nf; 
    uint size; /* 为NULL时不执行任何操作 */ 
    if (UNLIKELY(p == NULL)) 
        return; 
    poolp pool = POOL_ADDR(p);
    if (LIKELY(address_in_range(p, pool))) { 
        // LOCK(); 
        /* 把作为释放对象的block连接到freeblock */ 
        *(block **)p = lastfree = pool->freeblock; 
        /* 将释放的block连接到freeblock的开头 */ 
        pool->freeblock = (block *)p; 
        /* 这个pool中最后free的block是否为NULL？ */ 
        if (LIKELY(lastfree)) {   
            /* pool中有已经分配的block */ 
            if (UNLIKELY(--pool->ref.count != 0)) { 
                /* 不执行任何操作 */ 
                // UNLOCK(); 
                return; 
            } 
            /* 将pool返回arena */ 
            next = pool->nextpool; 
            prev = pool->prevpool; 
            next->prevpool = prev; 
            prev->nextpool = next; 
            
            /* 将pool返回arena */ 
            ao = &arenas[pool->arenaindex]; 
            pool->nextpool = ao->freepools; 
            ao->freepools = pool;  
            nf = ++ao->nfreepools;
            /* 当arena内所有pool为空时 */ 
            if (UNLIKELY(nf == ao->ntotalpools)) { 
                /* 从usable_arenas取出arena_object */ 
                if (LIKELY(ao->prevarena == NULL)) { 
                    usable_arenas = ao->nextarena; 
                } else { 
                    ao->prevarena->nextarena = ao->nextarena; 
                }

                /* 为了再利用arena_object 
                * 连接到unused_arena_objects 
                */ 
                ao->nextarena = unused_arena_objects; 
                unused_arena_objects = ao; 
                
                /* 释放arena */ 
                free((void *)ao->address); 
                
                /* “arena尚未被分配”的标记 */ 
                ao->address = 0; 
                // --narenas_currently_allocated; 
                // UNLOCK(); 
                return; 
            } 


            /* arena只有一个空pool */ 
            if (UNLIKELY(nf == 1)) {  
                /* 连接到usable_arenas的开头 */ 
                ao->nextarena = usable_arenas; 
                ao->prevarena = NULL; 
                if (usable_arenas) 
                    usable_arenas->prevarena = ao; 
                usable_arenas = ao; 
                
                // UNLOCK(); 
                return; 
            }   

            if (LIKELY(ao->nextarena == NULL || nf <= ao->nextarena->nfreepools)) { 
                /* 不执行任何操作 */ 
                // UNLOCK(); 
                return; 
            } 

            if (UNLIKELY(ao->prevarena != NULL)) { 
                /* ao isn't at the head of the list */ 
                ao->prevarena->nextarena = ao->nextarena; 
            } else { 
                /* ao is at the head of the list */ 
                usable_arenas = ao->nextarena; 
            } 

            ao->nextarena->prevarena = ao->prevarena; 
            while (ao->nextarena != NULL && nf > ao->nextarena->nfreepools) { 
                ao->prevarena = ao->nextarena; 
                ao->nextarena = ao->nextarena->nextarena; 
            } 
            
            ao->prevarena->nextarena = ao; 
            if (LIKELY(ao->nextarena != NULL)) 
                ao->nextarena->prevarena = ao; 
            // UNLOCK(); 
            return; 
        }

        --pool->ref.count;  
        
        size = pool->szidx; 
        next = usedpools[size + size]; 
        prev = next->prevpool; 
        /* 在usedpools的开头插入: prev <-> pool <-> next */ 
        pool->nextpool = next; 
        pool->prevpool = prev; 
        next->prevpool = pool; 
        prev->nextpool = pool; 
        // UNLOCK(); 
        return; 

    }

    /* (G)释放其他空间 */ 
    free(p);
}

/**
 * @brief 重新定位内存块
 *
 * 该函数尝试将给定的内存块 p 重新定位到新的大小 size。如果 p 在内存池范围内，
 * 则会分配一个新的内存块，并将 p 的内容复制到新的内存块中，然后释放 p。
 * 如果 p 不在内存池范围内，则直接使用 realloc 函数重新分配内存。
 *
 * @param p 需要重新定位的内存块指针
 * @param size 新的内存块大小
 * @return void* 重新定位后的内存块指针，如果失败则返回 NULL
 */
void *lmem_relocate(void *p, size_t size) {
    poolp pool = POOL_ADDR(p);
    if (LIKELY(address_in_range(p, pool)))
    {
        void * newp =  lmem_malloc(size);   
        if (UNLIKELY(p == NULL)) {
            return newp;
        }

        size_t nbytes = INDEX2SIZE(pool->szidx);
        memcpy(newp, p, MIN(nbytes, size));
        lmem_free(p);
        return newp;
    }
    return realloc(p, size);
}

/*
 *    define static functions
 */

/**
 * @brief 创建一个新的arena对象
 *
 * 该函数用于创建一个新的arena对象，并为其分配内存。如果当前未使用的arena对象列表为空，
 * 则会扩展arenas数组以容纳更多的arena对象。接着，从数组中取出一个未使用的arena对象，
 * 并为其分配一个指定大小的arena内存区域。最后，将arena内部分割成多个pool，并返回新创建的arena对象。
 *
 * @return 返回新创建的arena对象指针，如果创建失败则返回NULL。
 */
static struct arena_object *new_arena(void) {
    struct arena_object *arenaobj;
    uint excess;
    if (UNLIKELY(unused_arena_objects == NULL)) { 
        uint i; 
        /* 生成arena_object */ 
        uint numarenas = maxarenas ? maxarenas << 1 :INITIAL_ARENA_OBJECTS;
        if (UNLIKELY(numarenas <= maxarenas)) { 
            return NULL; /* 溢出 */
        }
        size_t nbytes = sizeof(struct arena_object) * numarenas;
        arenaobj = (struct arena_object*)realloc(arenas, nbytes);
        if (UNLIKELY(arenaobj == NULL)) { 
            return NULL; /* 内存不足 */
        }
        /* 把生成的arena_object 补充到arenas和unused_arena_objects里 */ 
        arenas = arenaobj;
        /* unused_arena_objects 生成列表 */ 
        for (i = maxarenas; i < numarenas; i++) { 
            /* 标记尚未分配arena */ 
            arenas[i].address = 0; 
            /* 只在末尾存入NULL，除此之外都指向下一个指针 */ 
            arenas[i].nextarena = i < numarenas - 1 ? &arenas[i+1] : NULL; 
        }
        /* 反映到全局变量中 */ 
        unused_arena_objects = &arenas[maxarenas]; 
        maxarenas = numarenas; 
    } 
    /* 把arena分配给未使用的arena_object */ 
    arenaobj = unused_arena_objects;
    unused_arena_objects = arenaobj->nextarena;
    arenaobj->address = (uptr)malloc(ARENA_SIZE);
    if (UNLIKELY(arenaobj->address == 0)) { 
        /* 分配失败 */ 
        arenaobj->nextarena = unused_arena_objects; 
        unused_arena_objects = arenaobj; 
        return NULL; 
    }
    /* 把arena内部分割成pool */ 
    arenaobj->freepools = NULL; 

    /* pool_address <- 对齐后开头pool的地址 nfreepools <- 对齐后arena中pool的数量 */
    arenaobj->pool_address = (block*)arenaobj->address; 
    arenaobj->nfreepools = ARENA_SIZE / POOL_SIZE; 

    excess = (uint)(arenaobj->address & POOL_SIZE_MASK); 
    if (UNLIKELY(excess != 0)) { 
        --arenaobj->nfreepools; 
        arenaobj->pool_address += POOL_SIZE - excess; 
    } 

    arenaobj->ntotalpools = arenaobj->nfreepools; 
    return arenaobj; 
    /* 返回新的arena_object */
}

/**
 * @brief 检查给定指针是否在内存池范围内
 *
 * 该函数遍历所有内存区域，检查给定指针是否位于任一内存区域的地址范围内。
 * 如果指针位于某个内存区域的地址范围内，则返回1，否则返回0。
 *
 * @param p 要检查的指针
 * @param pool 内存池结构体指针
 * @return int 如果指针在内存池范围内则返回1，否则返回0
 */
static int address_in_range(void *p, poolp pool) {
    for (size_t i = 0; i < maxarenas; i++)
    {
        if (arenas[i].address != 0 && arenas[i].address <= (uptr)p && (uptr)p < (uptr)arenas[i].address + ARENA_SIZE)
        {
            return 1;
        }
    }
    return 0;
}


