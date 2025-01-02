/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM mm

#define TRACE_INCLUDE_PATH trace/hooks

#if !defined(_TRACE_HOOK_MM_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_MM_H

#include <trace/hooks/vendor_hooks.h>

struct shmem_inode_info;
struct folio;

DECLARE_RESTRICTED_HOOK(android_rvh_shmem_get_folio,
			TP_PROTO(struct shmem_inode_info *info, struct folio **folio),
			TP_ARGS(info, folio), 2);

/*

DECLARE_RESTRICTED_HOOK(android_rvh_set_skip_swapcache_flags,
			TP_PROTO(gfp_t *flags),
			TP_ARGS(flags), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_set_gfp_zone_flags,
			TP_PROTO(gfp_t *flags),
			TP_ARGS(flags), 1);
DECLARE_RESTRICTED_HOOK(android_rvh_set_readahead_gfp_mask,
			TP_PROTO(gfp_t *flags),
			TP_ARGS(flags), 1);
*/
struct mem_cgroup;
DECLARE_HOOK(android_vh_mem_cgroup_alloc,
	TP_PROTO(struct mem_cgroup *memcg),
	TP_ARGS(memcg));
DECLARE_HOOK(android_vh_mem_cgroup_free,
	TP_PROTO(struct mem_cgroup *memcg),
	TP_ARGS(memcg));

struct cma;
DECLARE_HOOK(android_vh_cma_alloc_bypass,
	TP_PROTO(struct cma *cma, unsigned long count, unsigned int align,
		gfp_t gfp_mask, struct page **page, bool *bypass),
	TP_ARGS(cma, count, align, gfp_mask, page, bypass));

struct compact_control;
DECLARE_HOOK(android_vh_isolate_freepages,
	TP_PROTO(struct compact_control *cc, struct page *page, bool *bypass),
	TP_ARGS(cc, page, bypass));

struct oom_control;
DECLARE_HOOK(android_vh_oom_check_panic,
	TP_PROTO(struct oom_control *oc, int *ret),
	TP_ARGS(oc, ret));

struct page_vma_mapped_walk;
DECLARE_HOOK(android_vh_test_clear_look_around_ref,
	TP_PROTO(struct page *page),
	TP_ARGS(page));
DECLARE_HOOK(android_vh_look_around_migrate_folio,
	TP_PROTO(struct folio *old_folio, struct folio *new_folio),
	TP_ARGS(old_folio, new_folio));
DECLARE_HOOK(android_vh_look_around,
	TP_PROTO(struct page_vma_mapped_walk *pvmw, struct folio *folio,
		struct vm_area_struct *vma, int *referenced),
	TP_ARGS(pvmw, folio, vma, referenced));

DECLARE_HOOK(android_vh_calc_alloc_flags,
	TP_PROTO(gfp_t gfp_mask, unsigned int *alloc_flags,
		bool *bypass),
	TP_ARGS(gfp_mask, alloc_flags, bypass));

DECLARE_HOOK(android_vh_slab_folio_alloced,
	TP_PROTO(unsigned int order, gfp_t flags),
	TP_ARGS(order, flags));
DECLARE_HOOK(android_vh_kmalloc_large_alloced,
	TP_PROTO(struct folio *folio, unsigned int order, gfp_t flags),
	TP_ARGS(folio, order, flags));
DECLARE_RESTRICTED_HOOK(android_rvh_ctl_dirty_rate,
	TP_PROTO(struct inode *inode),
	TP_ARGS(inode), 1);
#endif /* _TRACE_HOOK_MM_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
