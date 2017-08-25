#include <asm/kvm_rmap.h>
#include <asm/kvm_mmu.h>

static struct kvm_rmap_head *gfn_to_rmap(struct kvm *kvm, gfn_t gfn)
{
	struct kvm_memory_slot *slot = gfn_to_memslot(kvm, gfn);
	unsigned long idx;

	if (!slot) {
		WARN(1, "Can't access the reverse mapping table.\n");
		return NULL;
	}

	idx = gfn - slot->base_gfn;
	return &slot->arch.rmap[idx];
}

/* Return a pointer to an entry, where we cache L1 IPA, for a given L2 IPA */
static unsigned long *get_l1_ipa_entry(unsigned long table_addr,
				       phys_addr_t fault_addr, bool hugepage)
{
	struct page *last_level_table;
	int idx;
	unsigned long *l1_ipas;

	if (hugepage)
		idx = pmd_index(fault_addr);
	else
		idx = pte_index(fault_addr);

	last_level_table = pfn_to_page(__pa(table_addr) >> PAGE_SHIFT);
	l1_ipas = (unsigned long *)page_private(last_level_table);

	return &l1_ipas[idx];
}

/* Cache L2 IPA to L1 IPA mapping */
void cache_ipa(unsigned long table_addr, phys_addr_t fault_ipa, phys_addr_t ipa,
	       bool hugepage)
{
	unsigned long *l1_ipa_entry;

	l1_ipa_entry = get_l1_ipa_entry(table_addr, fault_ipa, hugepage);
	*l1_ipa_entry = ipa;
}

static struct kvm_rmap_head *search_rmap_entry(struct kvm *kvm,
					       unsigned long l1_ipa,
					       unsigned long l2_ipa)
{
	struct kvm_rmap_head *rmap_head, *rmap_curr;
	struct rmap_iterator iter;

	rmap_head = gfn_to_rmap(kvm, gpa_to_gfn(l1_ipa));

	for_each_rmap_head(rmap_head, &iter, rmap_curr) {
		if (rmap_curr->val == l2_ipa)
			return rmap_curr;
	}

	return NULL;
}

/**
 * clear_rmap -- Remove rmap entry and clear a cached 'L2 IPA->L1 IPA' mapping
 *
 * When unmapping a page/block from a shadow stage-2 page table, we also
 * need to update rmap information. First we remove the rmap entry
 * and then clear the cached L1 IPA.
 */
static void clear_rmap(struct kvm *kvm, struct kvm_s2_mmu *mmu,
		       unsigned long table_addr, phys_addr_t addr,
		       bool hugepage)
{
	unsigned long *l1_ipa_entry;
	unsigned long l1_ipa, l2_ipa;
	struct kvm_rmap_head *rmap_curr;

	/* Do nothing if this is not a nested-mmu */
	if (mmu == &kvm->arch.mmu)
		return;

	l1_ipa_entry = get_l1_ipa_entry(table_addr, addr, hugepage);
	l1_ipa = *l1_ipa_entry;
	l2_ipa = addr;

	/* remove rmap entry */
	rmap_curr = search_rmap_entry(kvm, l1_ipa, l2_ipa);
	if (rmap_curr)
		kvm_rmap_remove(kvm, rmap_curr, gpa_to_gfn(l1_ipa));

	/* clear cached L1 IPA */
	*l1_ipa_entry = 0;
}


void clear_rmap_pte(struct kvm *kvm, struct kvm_s2_mmu *mmu,
		    unsigned long table_addr, phys_addr_t addr)
{
	clear_rmap(kvm, mmu, table_addr, addr, false);
}

void clear_rmap_pmd(struct kvm *kvm, struct kvm_s2_mmu *mmu,
		    unsigned long table_addr, phys_addr_t addr)
{
	clear_rmap(kvm, mmu, table_addr, addr, true);
}

static void set_rmap_entry(struct kvm_rmap_head *rmap_entry,
			struct kvm_s2_mmu *mmu, gpa_t gpa, int last_level)
{
	rmap_entry->val = (unsigned long)gpa;
	rmap_entry->mmu = mmu;
	rmap_entry->last_level = last_level;
}

static void clear_rmap_entry(struct kvm_rmap_head *rmap_head)
{
	set_rmap_entry(rmap_head, NULL, 0, 0);
}

/*
 * About rmap_head->val encoding:
 *
 * If the bit zero of rmap_head->val is clear, then it has the only L1 IPA
 * in this rmap chain. Otherwise, (rmap_head->val & ~1) points to a struct
 * rmap_list_desc containing more mappings.
 */

static void rmap_list_add(struct kvm_rmap_head *rmap_head,
			struct kvm_s2_mmu *mmu, gpa_t l2_ipa, int last_level,
			struct kvm_mmu_memory_cache *rmap_cache)

{
	struct rmap_list_desc *desc;
	int i;

	l2_ipa = l2_ipa & PAGE_MASK;

	if (!rmap_head->val) {
		/* Set a mapping to an empty rmap entry. */
		set_rmap_entry(rmap_head, mmu, l2_ipa, last_level);
	} else if (!(rmap_head->val & 1)) {
		/*
		 * Move an existing mapping from the rmap head to the
		 * descriptor and add a new mapping there.
		 */

		desc = kvm_mmu_memory_cache_alloc(rmap_cache);
		desc->rmap_entries[0] = *rmap_head;
		set_rmap_entry(&desc->rmap_entries[1], mmu, l2_ipa, last_level);

		/* Denote that rmap head is pointing to a descriptor */
		rmap_head->val = (unsigned long)desc | 1;
	} else {
		/* Search for the last descriptor starting from the first one */
		desc = (struct rmap_list_desc *)(rmap_head->val & ~1ul);
		while (desc->rmap_entries[RMAP_LIST_MAX-1].val && desc->more)
			desc = desc->more;

		/* If the last descriptor is full, create a new one */
		if (desc->rmap_entries[RMAP_LIST_MAX-1].val) {
			desc->more = kvm_mmu_memory_cache_alloc(rmap_cache);
			desc = desc->more;
		}

		for (i = 0; desc->rmap_entries[i].val; ++i)
			;
		set_rmap_entry(&desc->rmap_entries[i], mmu, l2_ipa, last_level);
	}
}

static void rmap_list_desc_remove_entry(struct kvm_rmap_head *rmap_head,
					struct rmap_list_desc *desc, int i,
					struct rmap_list_desc *prev_desc)
{
	int j;

	for (j = RMAP_LIST_MAX - 1; !desc->rmap_entries[j].val && j > i; --j)
		;
	desc->rmap_entries[i] = desc->rmap_entries[j];
	clear_rmap_entry(&desc->rmap_entries[j]);

	if (j != 0)
		return;

	if (!prev_desc && !desc->more) {
		rmap_head->val = (unsigned long)desc->rmap_entries[0].val;
	} else {
		if (prev_desc) {
			prev_desc->more = desc->more;
		} else {
			/* Set rmap_head-val to the next descriptor */
			rmap_head->val = (unsigned long)desc->more | 1;
		}
	}
	kvm_mmu_free_rmap_list_desc(desc);
}

static bool find_rmap_entry_desc(struct kvm_rmap_head *rmap_curr,
				 struct rmap_list_desc *desc, int *idx)
{
	int i;

	for (i = 0; i < RMAP_LIST_MAX && desc->rmap_entries[i].val; ++i) {
		if (&desc->rmap_entries[i] == rmap_curr) {
			*idx = i;
			return true;
		}
	}
	return false;
}

static void rmap_list_remove(struct kvm_rmap_head *rmap_curr,
			     struct kvm_rmap_head *rmap_head)
{
	struct rmap_list_desc *desc;
	struct rmap_list_desc *prev_desc;
	int idx;

	if (!rmap_head->val) {
		pr_err("%s: No entry to remove\n", __func__);
		return;
	} else if (!(rmap_head->val & 1)) {
		if (rmap_head != rmap_curr) {
			pr_err("%s: trying to remove a wrong rmap entry\n",
								      __func__);
			return;
		}

		clear_rmap_entry(rmap_head);
	} else {
		desc = (struct rmap_list_desc *)(rmap_head->val & ~1ul);
		prev_desc = NULL;
		while (desc) {
			if (find_rmap_entry_desc(rmap_curr, desc, &idx)) {
				rmap_list_desc_remove_entry(rmap_head, desc,
							    idx, prev_desc);
				return;
			}
			prev_desc = desc;
			desc = desc->more;
		}
		pr_err("%s: can't find a rmap entry to remove\n", __func__);
	}
}

/**
 * rmap_add - Add a l2_ipa to l1_ipa mapping into the rmap table
 * @vcpu:	the VCPU pointer
 * @mmu:	the mmu that has l2_ipa to pa mapping
 * @l1_ipa:	pa from the VM's perspective.
 * @l2_ipa:	pa from the nested VM's perspective.
 * @last_level:	the level that the translation ends. If last_level!= 3, it's a
 * 		hugepage mapping.
 */
void rmap_add(struct kvm *kvm, struct kvm_s2_mmu *mmu,
		  gpa_t l2_ipa, gpa_t l1_ipa, int last_level,
		struct kvm_mmu_memory_cache *rmap_cache)
{
	struct kvm_rmap_head *rmap_head;
	bool first_entry;

	/* Look up the rmap entry using L1 IPA as a key */
	rmap_head = gfn_to_rmap(kvm, gpa_to_gfn(l1_ipa));
	if (!rmap_head)
		return;

	first_entry = !rmap_head->val;
	rmap_list_add(rmap_head, mmu, l2_ipa, last_level, rmap_cache);
}

void kvm_rmap_add_pmd(struct kvm *kvm, struct kvm_s2_mmu *mmu,
		      gpa_t l2_ipa, gpa_t l1_ipa,
		      struct kvm_mmu_memory_cache *rmap_cache)
{
	rmap_add(kvm, mmu, l2_ipa, l1_ipa, 2, rmap_cache);
}

void kvm_rmap_add_pte(struct kvm *kvm, struct kvm_s2_mmu *mmu,
		      gpa_t l1_ipa, gpa_t l2_ipa,
		      struct kvm_mmu_memory_cache *rmap_cache)
{
	rmap_add(kvm, mmu, l2_ipa, l1_ipa, 3, rmap_cache);
}

/**
 * rmap_remove - Remove a rmap entry from the rmap table.
 * @kvm:	pointer to kvm structure.
 * @rmap_curr:	pointer to an rmap entry to be removed
 * @l1_gfn:	frame number of L1 IPA
 */
void kvm_rmap_remove(struct kvm *kvm, struct kvm_rmap_head *rmap_curr,
		     gfn_t l1_gfn)
{
	struct kvm_rmap_head *rmap_head;

	rmap_head = gfn_to_rmap(kvm, l1_gfn);
	if (!rmap_head)
		return;

	rmap_list_remove(rmap_curr, rmap_head);
}

/*
 * Iteration must be started by this function. This should also be used after
 * removing rmap entries from the rmap link because in such cases the
 * information in the itererator may not be valid.
 *
 * Returns kvm_rmap_head pointer if found, NULL otherwise.
 */
struct kvm_rmap_head *rmap_get_first(struct kvm_rmap_head *rmap_head,
			   struct rmap_iterator *iter)
{
	struct kvm_rmap_head *head;

	if (!rmap_head->val)
		return NULL;

	if (!(rmap_head->val & 1)) {
		iter->desc = NULL;
		return rmap_head;
	}

	iter->desc = (struct rmap_list_desc *)(rmap_head->val & ~1ul);
	iter->pos = 0;
	head = &iter->desc->rmap_entries[iter->pos];

	return head;
}

/*
 * Must be used with a valid iterator: e.g. after rmap_get_first().
 *
 * Returns kvm_rmap_head pointer if found, NULL otherwise.
 */
struct kvm_rmap_head *rmap_get_next(struct rmap_iterator *iter)
{
	struct kvm_rmap_head *rmap_head;

	if (iter->desc) {
		if (iter->pos < RMAP_LIST_MAX - 1) {
			++iter->pos;
			rmap_head = &iter->desc->rmap_entries[iter->pos];
			if (rmap_head->val)
				goto out;
		}

		iter->desc = iter->desc->more;

		if (iter->desc) {
			iter->pos = 0;
			/* desc->rmap_entries[0] cannot be NULL */
			rmap_head = &iter->desc->rmap_entries[iter->pos];
			goto out;
		}
	}

	return NULL;
out:
	return rmap_head;
}
