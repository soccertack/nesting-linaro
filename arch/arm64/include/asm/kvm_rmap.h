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

void kvm_rmap_add_pmd(struct kvm *kvm, struct kvm_s2_mmu *mmu,
		      gpa_t l2_ipa, gpa_t l1_ipa,
		      struct kvm_mmu_memory_cache *rmap_cache);

void kvm_rmap_add_pte(struct kvm *kvm, struct kvm_s2_mmu *mmu,
		      gpa_t l1_ipa, gpa_t l2_ipa,
		      struct kvm_mmu_memory_cache *rmap_cache);

void kvm_rmap_remove(struct kvm *kvm, struct kvm_rmap_head *rmap_curr,
		     gfn_t l1_gfn);

struct rmap_list_desc *kvm_mmu_alloc_rmap_list_desc(struct kvm_vcpu *vcpu);
void kvm_mmu_free_rmap_list_desc(struct rmap_list_desc *rmap_list_desc);

void cache_ipa(unsigned long table_addr, phys_addr_t fault_ipa, phys_addr_t ipa,
	       bool hugepage);
