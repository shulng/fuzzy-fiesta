/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_PAGEISOLATION_H
#define __LINUX_PAGEISOLATION_H

#ifdef CONFIG_MEMORY_ISOLATION
static inline bool has_isolate_pageblock(struct zone *zone)
{
	return zone->nr_isolate_pageblock;
}
static inline bool is_migrate_isolate_page(struct page *page)
{
	return get_pageblock_migratetype(page) == MIGRATE_ISOLATE;
}
static inline bool is_migrate_isolate(int migratetype)
{
	return migratetype == MIGRATE_ISOLATE;
}
#else
static inline bool has_isolate_pageblock(struct zone *zone)
{
	return false;
}
static inline bool is_migrate_isolate_page(struct page *page)
{
	return false;
}
static inline bool is_migrate_isolate(int migratetype)
{
	return false;
}
#endif

#define SKIP_HWPOISON	0x1
#define REPORT_FAILURE	0x2

bool has_unmovable_pages(struct zone *zone, struct page *page, int count,
			 int migratetype, int flags);
void set_pageblock_migratetype(struct page *page, int migratetype);
#ifdef CONFIG_BIGGER_ORDER_UNMOV
int move_freepages_block(struct zone *zone, struct page *page,
				int migratetype, int *num_movable, int old_mt);
#else
int move_freepages_block(struct zone *zone, struct page *page,
				int migratetype, int *num_movable);
#endif
/*
 * Changes migrate type in [start_pfn, end_pfn) to be MIGRATE_ISOLATE.
 */
int
start_isolate_page_range(unsigned long start_pfn, unsigned long end_pfn,
			 unsigned migratetype, int flags);

/*
 * Changes MIGRATE_ISOLATE to MIGRATE_MOVABLE.
 * target range is [start_pfn, end_pfn)
 */
void
undo_isolate_page_range(unsigned long start_pfn, unsigned long end_pfn,
			unsigned migratetype);

/*
 * Test all pages in [start_pfn, end_pfn) are isolated or not.
 */
int test_pages_isolated(unsigned long start_pfn, unsigned long end_pfn,
			bool skip_hwpoisoned_pages);

struct page *alloc_migrate_target(struct page *page, unsigned long private);

#endif
