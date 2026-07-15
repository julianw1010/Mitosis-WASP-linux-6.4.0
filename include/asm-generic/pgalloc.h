/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_GENERIC_PGALLOC_H
#define __ASM_GENERIC_PGALLOC_H

#ifdef CONFIG_MMU

#define GFP_PGTABLE_KERNEL	(GFP_KERNEL | __GFP_ZERO)
#define GFP_PGTABLE_USER	(GFP_PGTABLE_KERNEL | __GFP_ACCOUNT)

#include <asm/mitosis.h>

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
	return (pte_t *)__get_free_page(GFP_PGTABLE_KERNEL);
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
static inline pgtable_t __pte_alloc_one(struct mm_struct *mm, gfp_t gfp,
					pmd_t *pmd)
{
	int node = page_to_nid(virt_to_page(pmd));
	struct page *pte;

	pte = NULL;
	if (mm->repl_pgd_enabled)
		pte = mitosis_cache_pop(node, MITOSIS_CACHE_PTE, mm);
	if (!pte)
		pte = alloc_pages_node(node, gfp | __GFP_THISNODE, 0);
	BUG_ON(!pte);

	BUG_ON(!pgtable_pte_page_ctor(pte));

	pte->pt_owner_mm = mm;
	mitosis_pt_account_page(pte, MITOSIS_CACHE_PTE, 1);
	return pte;
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
static inline pgtable_t pte_alloc_one(struct mm_struct *mm, pmd_t *pmd)
{
	return __pte_alloc_one(mm, GFP_PGTABLE_USER, pmd);
}
#endif

static inline void mitosis_dtor_free_page(struct page *page, int level)
{
	int nid = page_to_nid(page);
	bool from_cache = PageMitosisFromCache(page);
	struct mm_struct *owner_mm = page->pt_owner_mm;

	mitosis_pt_account_page(page, level, -1);

	if (level == MITOSIS_CACHE_PTE)
		pgtable_pte_page_dtor(page);
	else if (level == MITOSIS_CACHE_PMD)
		pgtable_pmd_page_dtor(page);

	page->pt_owner_mm = NULL;
	ClearPageMitosisFromCache(page);

	if (from_cache) {
		page->pt_replica = NULL;
		if (mitosis_cache_push(page, nid, level, owner_mm))
			return;
	}

	__free_page(page);
}

/*
 * Should really implement gc for free page table pages. This could be
 * done with a reference count in struct page.
 */

/**
 * pte_free - free PTE-level user page table page
 * @mm: the mm_struct of the current context
 * @pte_page: the `struct page` representing the page table
 */
static inline void pte_free(struct mm_struct *mm, struct page *pte_page)
{
	mitosis_free_pte_replicas(mm, pte_page);
	mitosis_dtor_free_page(pte_page, MITOSIS_CACHE_PTE);
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
static inline pmd_t *pmd_alloc_one(struct mm_struct *mm, unsigned long addr,
				   pud_t *pud)
{
	gfp_t gfp = (mm == &init_mm) ? GFP_PGTABLE_KERNEL : GFP_PGTABLE_USER;
	struct page *page;
	int node;

	if (mm == &init_mm) {
		page = alloc_page(gfp);
		if (!page)
			return NULL;
		if (!pgtable_pmd_page_ctor(page)) {
			__free_page(page);
			return NULL;
		}
		return (pmd_t *)page_address(page);
	}

	node = page_to_nid(virt_to_page(pud));
	page = NULL;
	if (mm->repl_pgd_enabled)
		page = mitosis_cache_pop(node, MITOSIS_CACHE_PMD, mm);
	if (!page)
		page = alloc_pages_node(node, gfp | __GFP_THISNODE, 0);
	BUG_ON(!page);

	BUG_ON(!pgtable_pmd_page_ctor(page));

	page->pt_owner_mm = mm;
	mitosis_pt_account_page(page, MITOSIS_CACHE_PMD, 1);
	return (pmd_t *)page_address(page);
}
#endif

#ifndef __HAVE_ARCH_PMD_FREE
static inline void pmd_free(struct mm_struct *mm, pmd_t *pmd)
{
	BUG_ON((unsigned long)pmd & (PAGE_SIZE-1));
	mitosis_dtor_free_page(virt_to_page(pmd), MITOSIS_CACHE_PMD);
}
#endif

#endif /* CONFIG_PGTABLE_LEVELS > 2 */

#if CONFIG_PGTABLE_LEVELS > 3

static inline pud_t *__pud_alloc_one(struct mm_struct *mm, unsigned long addr,
				     p4d_t *p4d)
{
	gfp_t gfp = (mm == &init_mm) ? GFP_PGTABLE_KERNEL : GFP_PGTABLE_USER;
	struct page *page;
	int node;

	if (mm == &init_mm) {
		page = alloc_page(gfp);
		if (!page)
			return NULL;
		page->pt_replica = NULL;
		page->pt_owner_mm = NULL;
		return (pud_t *)page_address(page);
	}

	node = page_to_nid(virt_to_page(p4d));
	page = NULL;
	if (mm->repl_pgd_enabled)
		page = mitosis_cache_pop(node, MITOSIS_CACHE_PUD, mm);
	if (!page)
		page = alloc_pages_node(node, gfp | __GFP_ZERO | __GFP_THISNODE, 0);
	if (!page)
		return NULL;

	page->pt_replica = NULL;
	page->pt_owner_mm = mm;
	mitosis_pt_account_page(page, MITOSIS_CACHE_PUD, 1);
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
static inline pud_t *pud_alloc_one(struct mm_struct *mm, unsigned long addr,
				   p4d_t *p4d)
{
	return __pud_alloc_one(mm, addr, p4d);
}
#endif

static inline void __pud_free(struct mm_struct *mm, pud_t *pud)
{
	BUG_ON((unsigned long)pud & (PAGE_SIZE-1));
	mitosis_dtor_free_page(virt_to_page(pud), MITOSIS_CACHE_PUD);
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
