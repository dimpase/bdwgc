/*
 * Copyright (c) 2011 by Hewlett-Packard Company.  All rights reserved.
 *
 * THIS MATERIAL IS PROVIDED AS IS, WITH ABSOLUTELY NO WARRANTY EXPRESSED
 * OR IMPLIED.  ANY USE IS AT YOUR OWN RISK.
 *
 * Permission is hereby granted to use or copy this program
 * for any purpose,  provided the above notices are retained on all copies.
 * Permission to modify the code and to distribute modified code is granted,
 * provided the above notices are retained, and a notice that the code was
 * modified is included with the above copyright notice.
 *
 */

#include "private/gc_priv.h"

#ifdef ENABLE_DISCLAIM

#include "gc_disclaim.h"

#ifdef THREAD_LOCAL_ALLOC
# include "private/thread_local_alloc.h"
#else
  STATIC ptr_t * GC_finalized_objfreelist = NULL;
#endif /* !THREAD_LOCAL_ALLOC */

STATIC int GC_finalized_kind = 0;

STATIC int GC_CALLBACK GC_finalized_disclaim(void *obj)
{
    GC_word fc_word;
    fc_word = *(GC_word *)obj;
    if ((fc_word & 1) != 0) {
       /* [1] The disclaim function may be passed fragments from the    */
       /* free-list, on which it should not run finalization.           */
       /* To recognize this case, we use the fact that the first word   */
       /* on such fragments are always even (a link to the next         */
       /* fragment, or NULL).  If it is desirable to have a finalizer   */
       /* which does not use the first word for storing finalization    */
       /* info, GC_reclaim_with_finalization must be extended to clear  */
       /* fragments so that the assumption holds for the selected word. */
        const struct GC_finalizer_closure *fc = (void *)(fc_word & ~(GC_word)1);
        (*fc->proc)((GC_word *)obj + 1, fc->cd);
    }
    return 0;
}

static GC_bool done_init = FALSE;

GC_API void GC_CALL GC_init_finalized_malloc(void)
{
    DCL_LOCK_STATE;

    GC_init();  /* In case it's not already done.       */
    LOCK();
    if (done_init) {
        UNLOCK();
        return;
    }
    done_init = TRUE;

    GC_register_displacement_inner(sizeof(GC_word));

    GC_finalized_objfreelist = (ptr_t *)GC_new_free_list_inner();
    GC_finalized_kind = GC_new_kind_inner((void **)GC_finalized_objfreelist,
                                          GC_DS_LENGTH, TRUE, TRUE);
    GC_register_disclaim_proc(GC_finalized_kind, GC_finalized_disclaim, TRUE);
    UNLOCK();
}

GC_API void GC_CALL GC_register_disclaim_proc(int kind, GC_disclaim_proc proc,
                                              int mark_unconditionally)
{
    GC_ASSERT((unsigned)kind < MAXOBJKINDS);
    GC_obj_kinds[kind].ok_disclaim_proc = proc;
    GC_obj_kinds[kind].ok_mark_unconditionally = (GC_bool)mark_unconditionally;
}

#ifdef THREAD_LOCAL_ALLOC
  STATIC void * GC_core_finalized_malloc(size_t lb,
                                const struct GC_finalizer_closure *fclos)
#else
  GC_API GC_ATTR_MALLOC void * GC_CALL GC_finalized_malloc(size_t lb,
                                const struct GC_finalizer_closure *fclos)
#endif
{
    ptr_t op;
    word lg;
    DCL_LOCK_STATE;

    lb += sizeof(GC_word);
    GC_ASSERT(done_init);
    if (SMALL_OBJ(lb)) {
        GC_DBG_COLLECT_AT_MALLOC(lb);
        lg = GC_size_map[lb];
        LOCK();
        op = GC_finalized_objfreelist[lg];
        if (EXPECT(0 == op, FALSE)) {
            UNLOCK();
            op = GC_generic_malloc(lb, GC_finalized_kind);
            if (NULL == op)
                return NULL;
            /* GC_generic_malloc has extended the size map for us.      */
            lg = GC_size_map[lb];
        } else {
            GC_finalized_objfreelist[lg] = obj_link(op);
            obj_link(op) = 0;
            GC_bytes_allocd += GRANULES_TO_BYTES(lg);
            UNLOCK();
        }
        GC_ASSERT(lg > 0);
    } else {
        size_t op_sz;

        op = GC_generic_malloc(lb, GC_finalized_kind);
        if (NULL == op)
            return NULL;
        op_sz = GC_size(op);
        GC_ASSERT(op_sz >= lb);
    }
    *(GC_word *)op = (GC_word)fclos | 1;
    return GC_clear_stack((GC_word *)op + 1);
}

#ifdef THREAD_LOCAL_ALLOC
  GC_API GC_ATTR_MALLOC void * GC_CALL GC_finalized_malloc(size_t client_lb,
                                const struct GC_finalizer_closure *fclos)
  {
    size_t lb = client_lb + sizeof(GC_word);
    size_t lg = ROUNDED_UP_GRANULES(lb);
    GC_tlfs tsd;
    void *result;
    void **tiny_fl, **my_fl, *my_entry;
    void *next;

    if (EXPECT(lg >= GC_TINY_FREELISTS, FALSE))
        return GC_core_finalized_malloc(client_lb, fclos);

    tsd = GC_getspecific(GC_thread_key);
    tiny_fl = tsd->finalized_freelists;
    my_fl = tiny_fl + lg;
    my_entry = *my_fl;
    while (EXPECT((word)my_entry
                  <= DIRECT_GRANULES + GC_TINY_FREELISTS + 1, FALSE)) {
        if ((word)my_entry - 1 < DIRECT_GRANULES) {
            *my_fl = (ptr_t)my_entry + lg + 1;
            return GC_core_finalized_malloc(client_lb, fclos);
        } else {
            GC_generic_malloc_many(GC_RAW_BYTES_FROM_INDEX(lg),
                                   GC_finalized_kind, my_fl);
            my_entry = *my_fl;
            if (my_entry == 0) {
                return (*GC_get_oom_fn())(lb);
            }
        }
    }

    next = obj_link(my_entry);
    result = (void *)my_entry;
    *my_fl = next;
    obj_link(result) = 0;
    *(GC_word *)result = (GC_word)fclos | 1;
    PREFETCH_FOR_WRITE(next);
    return (GC_word *)result + 1;
  }
#endif /* THREAD_LOCAL_ALLOC */

#endif /* ENABLE_DISCLAIM */
