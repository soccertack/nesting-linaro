#ifndef __ARM64_KVM_NESTED_PV_H__
#define __ARM64_KVM_NESTED_PV_H__

#include <asm/kvm_nested_pv_encoding.h>
#include "sys_regs.h"

int emulate_tlbi(struct kvm_vcpu *vcpu, struct sys_reg_params *params);
int kvm_handle_eret(struct kvm_vcpu *vcpu, struct kvm_run *run);

#ifdef __ASSEMBLY__
#ifndef CONFIG_PV_EL2
	.macro mrs_pv rt, sreg
	mrs	\rt, \sreg
	.endm

	.macro msr_pv sreg, rt
	msr	\sreg, \rt
	.endm

	.macro eret_pv
	eret
	.endm

	.macro tlbi_pv0 instr
	tlbi	\instr
	.endm

	.macro tlbi_pv1 instr, rt
	tlbi	\instr, \rt
	.endm

	.macro hvc_pv imm
	hvc	\imm
	.endm
#else
	.macro mrs_pv rt, sreg
	/* hvc|instr|sysreg|gpreg */
	.inst	0xd4000002|(.L__mrs)|(.L__\sreg)|(.L__gpreg_num_\rt)
	.endm

	.macro msr_pv sreg, rt
	/* hvc|instr|sysreg|gpreg */
	.inst	0xd4000002|(.L__msr)|(.L__\sreg)|(.L__gpreg_num_\rt)
	.endm

	.macro tlbi_pv0 instr
	/* hvc|instr */
	.inst	0xd4000002|(.L__tlbi)|(.L__\instr)
	.endm

	.macro tlbi_pv1 instr, rt
	/* hvc|instr|gpreg */
	.inst	0xd4000002|(.L__tlbi)|(.L__\instr)|(.L__gpreg_num_\rt)
	.endm

	.macro eret_pv
	/* hvc|instr */
	.inst	0xd4000002|(.L__eret)
	.endm

	.macro hvc_pv imm
	/* hvc|instr|imm */
	.inst	0xd4000002|(.L__hvc)
	.endm
#endif /* CONFIG_PV_EL2 */
#else /* __ASSEMBLY__ */

#ifndef CONFIG_PV_EL2
asm(
"	.macro mrs_pv rt, sreg\n"
"	mrs	\\rt, \\sreg\n"
"	.endm\n"
"\n"
"	.macro msr_pv sreg, rt\n"
"	msr	\\sreg, \\rt\n"
"	.endm\n"
"\n"
"	.macro eret_pv\n"
"	eret\n"
"	.endm\n"
"\n"
"	.macro tlbi_pv0 instr\n"
"	tlbi	\\instr\n"
"	.endm\n"
"\n"
"	.macro tlbi_pv1 instr, rt\n"
"	tlbi	\\instr, \\rt	\n"
"	.endm\n"
"\n"
"	.macro hvc_pv imm\n"
"	hvc	\\imm\n"
"	.endm\n"
);

#define kvm_read_sysreg(r)	read_sysreg(r)
#define kvm_read_sysreg_el1(r)	read_sysreg_el1(r)
#define kvm_read_sysreg_el2(r)	read_sysreg_el2(r)
#define kvm_write_sysreg(v, r)		write_sysreg(v, r)
#define kvm_write_sysreg_el1(v, r)	write_sysreg_el1(v, r)
#define kvm_write_sysreg_el2(v, r)	write_sysreg_el2(v, r)

#else /* CONFIG_PV_EL2 */

#include <linux/types.h>

asm(
PV_CONST
"	.macro mrs_pv rt, sreg\n"
"	.inst	0xd4000002|(.L__mrs)|(.L__\\sreg)|(.L__gpreg_num_\\rt)\n"
"	.endm\n"
"\n"
"	.macro msr_pv sreg, rt\n"
"	.inst	0xd4000002|(.L__msr)|(.L__\\sreg)|(.L__gpreg_num_\\rt)\n"
"	.endm\n"
"\n"
"	.macro tlbi_pv0 instr\n"
"	.inst	0xd4000002|(.L__tlbi)|(.L__\\instr)\n"
"	.endm\n"
"\n"
"	.macro tlbi_pv1 instr, rt\n"
"	.inst	0xd4000002|(.L__tlbi)|(.L__\\instr)|(.L__gpreg_num_\\rt)\n"
"	.endm\n"
"\n"
"	.macro eret_pv\n"
"	.inst	0xd4000002|(.L__eret)\n"
"	.endm\n"
"\n"
"	.macro hvc_pv imm\n"
"	.inst	0xd4000002|(.L__hvc)\n"
"	.endm\n"
);

#define read_s_sysreg(sreg) ({                          \
        u64 __val = 0;                                  \
	asm volatile( "mrs_pv x1, " __stringify(sreg)"\n\t"	\
                "mov %0, x1\n\t" : "=r" (__val)::"x1"); \
        __val;                                          \
})

#define write_s_sysreg(v, sreg) do {                    \
        u64 __val = (u64)v;                             \
        asm volatile("mov       x1, %0\n\t"             \
                "msr_pv " __stringify(sreg)", x1\n\t"        \
                     : : "r" (__val):"x1");             \
} while (0)

#define kvm_read_sysreg(r) 	read_s_sysreg(r)
#define kvm_write_sysreg(v, r)		write_s_sysreg(v, r)
/* TODO: Following four lines will break VHE enabled system */
#define kvm_read_sysreg_el1(r) 	read_s_sysreg(r##_el1)
#define kvm_read_sysreg_el2(r) 	read_s_sysreg(r##_el2)
#define kvm_write_sysreg_el1(v, r)	write_s_sysreg(v, r##_el1)
#define kvm_write_sysreg_el2(v, r)	write_s_sysreg(v, r##_el2)
#endif /* CONFIG_PV_EL2 */
#endif /* __ASSEMBLY__ */

#endif
