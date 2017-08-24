#include <asm/kvm_rmap.h>
#include <asm/kvm_mmu.h>

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

