/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_GENERIC_PGALLOC_H
#define __ASM_GENERIC_PGALLOC_H

#ifdef CONFIG_MMU

#define GFP_PGTABLE_KERNEL	(GFP_KERNEL | __GFP_ZERO)
#define GFP_PGTABLE_USER	(GFP_PGTABLE_KERNEL | __GFP_ACCOUNT)

#include <asm/pgtable_repl.h>

static inline bool mitosis_active(struct mm_struct *mm)
{
	return mm && mm != &init_mm &&
	       (smp_load_acquire(&mm->repl_pgd_enabled) ||
		READ_ONCE(mm->cache_only_mode) ||
		sysctl_mitosis_mode == 0);
}

static inline struct page *mitosis_alloc_primary(struct mm_struct *mm,
						  gfp_t gfp, int order,
						  int cache_level)
{
	int interleave;
	int node;
	struct page *page;

	if (sysctl_mitosis_mode == 0) {
		node = 0;
		interleave = -1;
	} else {
		interleave = mitosis_interleave_node(mm);
		node = (interleave >= 0) ? interleave : numa_node_id();
	}

	if (order == 0) {
		page = mitosis_cache_pop(node, cache_level);
		if (page)
			return page;
	}

	if (sysctl_mitosis_mode == 0 || interleave >= 0)
		return alloc_pages_node(node, gfp | __GFP_THISNODE, order);
	return alloc_pages(gfp, order);
}

static inline void mitosis_ctor_fail(struct page *page, int cache_level)
{
	if (PageMitosisFromCache(page)) {
		page->pt_replica = NULL;
		mitosis_cache_push(page, page_to_nid(page), cache_level);
	} else {
		__free_page(page);
	}
}

/**
 * __pte_alloc_one_kernel - allocate a page for PTE-level kernel page table
 * @mm: the mm_struct of the current context
 *
 * This function is intended for architectures that need
 * anything beyond simple page allocation.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
static inline pte_t *__pte_alloc_one_kernel(struct mm_struct *mm)
{
    gfp_t gfp = GFP_PGTABLE_KERNEL;
    struct page *page;

    if (mitosis_active(mm)) {
        page = mitosis_alloc_primary(mm, gfp, 0, MITOSIS_CACHE_PTE);
        if (!page)
            return NULL;
        page->pt_owner_mm = mm;
        return (pte_t *)page_address(page);
    }

    page = alloc_pages(gfp, 0);
    if (!page)
        return NULL;
    page->pt_owner_mm = mm;
    return (pte_t *)page_address(page);
}

#ifndef __HAVE_ARCH_PTE_ALLOC_ONE_KERNEL
/**
 * pte_alloc_one_kernel - allocate a page for PTE-level kernel page table
 * @mm: the mm_struct of the current context
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
static inline pte_t *pte_alloc_one_kernel(struct mm_struct *mm)
{
	return __pte_alloc_one_kernel(mm);
}
#endif

/**
 * pte_free_kernel - free PTE-level kernel page table page
 * @mm: the mm_struct of the current context
 * @pte: pointer to the memory containing the page table
 */
static inline void pte_free_kernel(struct mm_struct *mm, pte_t *pte)
{
    struct page *page;
    int nid;
    bool from_cache;

    page = virt_to_page(pte);
    nid = page_to_nid(page);
    from_cache = PageMitosisFromCache(page);

    page->pt_owner_mm = NULL;

    if (from_cache) {
        ClearPageMitosisFromCache(page);
        page->pt_replica = NULL;
        if (mitosis_cache_push(page, nid, MITOSIS_CACHE_PTE))
            return;
    }
    ClearPageMitosisFromCache(page);
    free_page((unsigned long)pte);
}

/**
 * __pte_alloc_one - allocate a page for PTE-level user page table
 * @mm: the mm_struct of the current context
 * @gfp: GFP flags to use for the allocation
 *
 * Allocates a page and runs the pgtable_pte_page_ctor().
 *
 * This function is intended for architectures that need
 * anything beyond simple page allocation or must have custom GFP flags.
 *
 * Return: `struct page` initialized as page table or %NULL on error
 */
static inline pgtable_t __pte_alloc_one(struct mm_struct *mm, gfp_t gfp)
{
    struct page *page;

    if (mitosis_active(mm)) {
        page = mitosis_alloc_primary(mm, gfp, 0, MITOSIS_CACHE_PTE);
        if (!page)
            return NULL;
        if (!pgtable_pte_page_ctor(page)) {
            mitosis_ctor_fail(page, MITOSIS_CACHE_PTE);
            return NULL;
        }
        page->pt_owner_mm = mm;
        return page;
    }

    page = alloc_pages(gfp, 0);
    if (!page)
        return NULL;
    if (!pgtable_pte_page_ctor(page)) {
        __free_page(page);
        return NULL;
    }
    page->pt_owner_mm = mm;
    return page;
}

#ifndef __HAVE_ARCH_PTE_ALLOC_ONE
/**
 * pte_alloc_one - allocate a page for PTE-level user page table
 * @mm: the mm_struct of the current context
 *
 * Allocates a page and runs the pgtable_pte_page_ctor().
 *
 * Return: `struct page` initialized as page table or %NULL on error
 */
static inline pgtable_t pte_alloc_one(struct mm_struct *mm)
{
	return __pte_alloc_one(mm, GFP_PGTABLE_USER);
}
#endif

/*
 * Should really implement gc for free page table pages. This could be
 * done with a reference count in struct page.
 */

/**
 * pte_free - free PTE-level user page table page
 * @mm: the mm_struct of the current context
 * @pte_page: the `struct page` representing the page table
 */
static inline void pte_free(struct mm_struct *mm, pgtable_t pte)
{
    int nid;
    bool from_cache;

    nid = page_to_nid(pte);
    from_cache = PageMitosisFromCache(pte);

    pgtable_repl_free_pte_replicas(mm, pte);
    pgtable_pte_page_dtor(pte);

    pte->pt_owner_mm = NULL;

    if (from_cache) {
        ClearPageMitosisFromCache(pte);
        pte->pt_replica = NULL;
        if (mitosis_cache_push(pte, nid, MITOSIS_CACHE_PTE))
            return;
    }
    ClearPageMitosisFromCache(pte);
    __free_page(pte);
}


#if CONFIG_PGTABLE_LEVELS > 2

#ifndef __HAVE_ARCH_PMD_ALLOC_ONE
/**
 * pmd_alloc_one - allocate a page for PMD-level page table
 * @mm: the mm_struct of the current context
 *
 * Allocates a page and runs the pgtable_pmd_page_ctor().
 * Allocations use %GFP_PGTABLE_USER in user context and
 * %GFP_PGTABLE_KERNEL in kernel context.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
static inline pmd_t *pmd_alloc_one(struct mm_struct *mm, unsigned long addr)
{
    gfp_t gfp = (mm == &init_mm) ? GFP_PGTABLE_KERNEL : GFP_PGTABLE_USER;
    struct page *page;

    if (mitosis_active(mm)) {
        page = mitosis_alloc_primary(mm, gfp, 0, MITOSIS_CACHE_PMD);
        if (!page)
            return NULL;
        if (!pgtable_pmd_page_ctor(page)) {
            mitosis_ctor_fail(page, MITOSIS_CACHE_PMD);
            return NULL;
        }
        page->pt_owner_mm = mm;
        return (pmd_t *)page_address(page);
    }

    page = alloc_pages(gfp, 0);
    if (!page)
        return NULL;
    if (!pgtable_pmd_page_ctor(page)) {
        __free_page(page);
        return NULL;
    }
    page->pt_owner_mm = mm;
    return (pmd_t *)page_address(page);
}
#endif

#ifndef __HAVE_ARCH_PMD_FREE
static inline void pmd_free(struct mm_struct *mm, pmd_t *pmd)
{
    struct page *page;
    int nid;
    bool from_cache;

    page = virt_to_page(pmd);
    nid = page_to_nid(page);
    from_cache = PageMitosisFromCache(page);

    BUG_ON((unsigned long)pmd & (PAGE_SIZE-1));
    pgtable_pmd_page_dtor(page);

    page->pt_owner_mm = NULL;

    if (from_cache) {
        ClearPageMitosisFromCache(page);
        page->pt_replica = NULL;
        if (mitosis_cache_push(page, nid, MITOSIS_CACHE_PMD))
            return;
    }
    ClearPageMitosisFromCache(page);
    __free_page(page);
}
#endif

#endif /* CONFIG_PGTABLE_LEVELS > 2 */

#if CONFIG_PGTABLE_LEVELS > 3

static inline pud_t *__pud_alloc_one(struct mm_struct *mm, unsigned long addr)
{
    gfp_t gfp = (mm == &init_mm) ? GFP_PGTABLE_KERNEL : GFP_PGTABLE_USER;
    struct page *page;

    if (mitosis_active(mm)) {
        page = mitosis_alloc_primary(mm, gfp | __GFP_ZERO, 0, MITOSIS_CACHE_PUD);
        if (!page)
            return NULL;
        page->pt_owner_mm = mm;
        return (pud_t *)page_address(page);
    }

    page = alloc_pages(gfp | __GFP_ZERO, 0);
    if (!page)
        return NULL;
    page->pt_owner_mm = mm;
    return (pud_t *)page_address(page);
}

#ifndef __HAVE_ARCH_PUD_ALLOC_ONE
/**
 * pud_alloc_one - allocate a page for PUD-level page table
 * @mm: the mm_struct of the current context
 *
 * Allocates a page using %GFP_PGTABLE_USER for user context and
 * %GFP_PGTABLE_KERNEL for kernel context.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
static inline pud_t *pud_alloc_one(struct mm_struct *mm, unsigned long addr)
{
	return __pud_alloc_one(mm, addr);
}
#endif

static inline void __pud_free(struct mm_struct *mm, pud_t *pud)
{
    struct page *page;
    int nid;
    bool from_cache;

    page = virt_to_page(pud);
    nid = page_to_nid(page);
    from_cache = PageMitosisFromCache(page);

    BUG_ON((unsigned long)pud & (PAGE_SIZE-1));

    page->pt_owner_mm = NULL;

    if (from_cache) {
        ClearPageMitosisFromCache(page);
        page->pt_replica = NULL;
        if (mitosis_cache_push(page, nid, MITOSIS_CACHE_PUD))
            return;
    }
    ClearPageMitosisFromCache(page);
    free_page((unsigned long)pud);
}

#ifndef __HAVE_ARCH_PUD_FREE
static inline void pud_free(struct mm_struct *mm, pud_t *pud)
{
	__pud_free(mm, pud);
}
#endif

#endif /* CONFIG_PGTABLE_LEVELS > 3 */

#ifndef __HAVE_ARCH_PGD_FREE
static inline void pgd_free(struct mm_struct *mm, pgd_t *pgd)
{
	free_page((unsigned long)pgd);
}
#endif

#endif /* CONFIG_MMU */

#endif /* __ASM_GENERIC_PGALLOC_H */
