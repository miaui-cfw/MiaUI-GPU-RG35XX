/**
 * @file bifctl.c
 * @brief D20 test module (tests only): sets bits of the SGX544 memory interface control register EUR_CR_BIF_MMU_CTRL
 *        (SGX registers at 0xB0300000, register 0x0CD0; sgx544defs.h):
 *          bit 3 ENABLE_WRITE_BURST_COLLATE, bit 5 DISABLE_BURST_EXP (no other bit is ever touched).
 *        On a single-core SGX544 the PowerVR driver never writes this register: it keeps its reset value, which comes
 *        back at every GPU power-up (GPU reset). So the bits are applied from a kretprobe at the return of SGXReset(),
 *        called by SGXInitialise() at each power-up: the SGX is powered and clocked there (the driver has just reset
 *        and programmed it) and its microkernel has not been started yet. Nothing is accessed at insmod/rmmod.
 *
 *   set_mask (rw)   bits to set   (only 0x08 and 0x20 are honoured)
 *   clr_mask (rw)   bits to clear (idem)
 *   resets, applied, before, after (ro)   SGXReset calls seen, writes made, last value read before/after
 * A change of the masks takes effect at the next GPU power-up (the GPU powers off 2 ms after it goes idle).
 */
#include <linux/io.h>
#include <linux/kprobes.h>
#include <linux/module.h>

#define SGX_REGS_PHYS 0xB0300000u
#define EUR_CR_BIF_MMU_CTRL 0x0CD0u
#define ALLOWED_BITS 0x28u	/* ENABLE_WRITE_BURST_COLLATE | DISABLE_BURST_EXP */

static unsigned int set_mask, clr_mask;
module_param(set_mask, uint, 0644);
module_param(clr_mask, uint, 0644);
static unsigned long resets, applied;
static unsigned int before, after;
module_param(resets, ulong, 0444);
module_param(applied, ulong, 0444);
module_param(before, uint, 0444);
module_param(after, uint, 0444);

static void __iomem *sgx;

/**
 * @brief Return of SGXReset(): reads EUR_CR_BIF_MMU_CTRL and applies the masks (allowed bits only).
 * @param ri Unused. @param regs Unused.
 * @return 0.
 */
static int on_reset_return(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	unsigned int set = ACCESS_ONCE(set_mask) & ALLOWED_BITS, clr = ACCESS_ONCE(clr_mask) & ALLOWED_BITS;
	u32 v = readl(sgx + EUR_CR_BIF_MMU_CTRL);

	resets++;
	before = v;
	if (set || clr) {
		writel((v & ~clr) | set, sgx + EUR_CR_BIF_MMU_CTRL);
		applied++;
	}
	after = readl(sgx + EUR_CR_BIF_MMU_CTRL);
	return 0;
}

static struct kretprobe krp = {
	.kp.symbol_name = "SGXReset",
	.handler = on_reset_return,
	.maxactive = 2,
};

/** @brief Maps the SGX register page and arms the kretprobe (no SGX access here). @return 0 or an error. */
static int __init bifctl_init(void)
{
	int r;

	sgx = ioremap(SGX_REGS_PHYS, 0x1000);
	if (!sgx)
		return -ENOMEM;
	r = register_kretprobe(&krp);
	if (r) {
		iounmap(sgx);
		return r;
	}
	pr_info("bifctl: SGXReset probed, set 0x%x clr 0x%x from the next GPU power-up\n", set_mask, clr_mask);
	return 0;
}

/** @brief Disarms it; the register returns to its reset value at the next GPU power-up. */
static void __exit bifctl_exit(void)
{
	unregister_kretprobe(&krp);
	iounmap(sgx);
	pr_info("bifctl: removed (%lu resets, %lu writes, last 0x%x -> 0x%x)\n", resets, applied, before, after);
}

module_init(bifctl_init);
module_exit(bifctl_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Test only: SGX544 EUR_CR_BIF_MMU_CTRL burst bits");
