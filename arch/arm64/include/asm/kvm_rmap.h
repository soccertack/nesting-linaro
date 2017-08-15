#include <linux/kvm_host.h>

struct kvm_rmap_head {
	/*
	 * An L2 IPA or a pointer to a descriptor, which contains multiple
	 * kvm_rmap_head structures.
	 */
	unsigned long val;

	/*
	 * a shadow stage-2 mmu that we use to locate a pte/pmd with an L2 IPA.
	 * mmu->vmid is also used to invalidate TLB entries.
	 */
	struct kvm_s2_mmu *mmu;

	/* used to differentiate between a page mapping and a block mapping */
	int last_level;
};

/* Max number of struct kvm_map_head objects fits in a page */
#define RMAP_LIST_MAX (PAGE_SIZE/sizeof(struct kvm_rmap_head) - 1)

struct rmap_list_desc {
	struct kvm_rmap_head rmap_entries[RMAP_LIST_MAX];
	struct rmap_list_desc *more;
};
