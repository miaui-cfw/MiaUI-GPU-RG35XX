/**
 * @file noflip.c
 * @brief D14 test module (tests only): turns the PowerVR display class's flip (OMAPLFBFlip in pvrsrvkm, which calls
 *        fb_set_var) into a no-op, with a kprobe at its entry that returns to the caller at once. The rest of the GPU
 *        driver's frame is untouched: rendering, eglSwapBuffers, the flip command, its work queue, the (stub) vsync
 *        wait and CmdComplete all run as usual, so a GL client renders and swaps exactly as normal while the display
 *        engine is never reprogrammed by it (the picture stays static, or another tool such as panflip flips it).
 *
 * Load after pvrsrvkm, with or without a GL client running. Parameter skipped (read only) counts the flips skipped.
 */
#include <linux/kprobes.h>
#include <linux/module.h>

static unsigned long skipped;
module_param(skipped, ulong, 0444);
MODULE_PARM_DESC(skipped, "GPU driver flips skipped so far");

/* OMAPLFBFlip's first instruction in the console's pvrsrvkm.ko (md5 8d33b1ee...): push {r4, r5, r6, lr} */
#define FLIP_ENTRY_OPCODE 0xe92d4070u
#define FLIP_ENTRY_PUSHED 16

/**
 * @brief kprobe post-handler, after OMAPLFBFlip's first instruction (the push) ran: undoes the push and returns to the
 *        caller, so the rest of the function (lock_fb_info, console_lock, fb_set_var) never runs.
 *        (A pre-handler that changes pc would have to clear current_kprobe, which is not exported on 3.10 ARM.)
 * @param p The kprobe. @param regs Registers after the push: lr still holds the return address.
 * @param flags Unused.
 */
static void skip_flip(struct kprobe* p, struct pt_regs* regs, unsigned long flags)
{
	skipped++;
	regs->ARM_sp += FLIP_ENTRY_PUSHED;
	regs->ARM_pc = regs->ARM_lr;
}

static struct kprobe kp = {
	.symbol_name = "OMAPLFBFlip",
	.post_handler = skip_flip,
};

/** @brief Arms the probe. @return 0, or the register_kprobe() error (e.g. pvrsrvkm not loaded). */
static int __init noflip_init(void)
{
	int r = register_kprobe(&kp);

	if (!r && kp.opcode != FLIP_ENTRY_OPCODE) {
		/* a different pvrsrvkm build: the stack fix-up above would be wrong */
		pr_err("noflip: OMAPLFBFlip starts with %08lx, not %08x: refusing\n", (unsigned long)kp.opcode,
		       FLIP_ENTRY_OPCODE);
		unregister_kprobe(&kp);
		return -EINVAL;
	}
	pr_info("noflip: OMAPLFBFlip %s (%d)\n", r ? "NOT probed" : "skipped from now on", r);
	return r;
}

/** @brief Disarms the probe; GPU flips reach the display again. */
static void __exit noflip_exit(void)
{
	unregister_kprobe(&kp);
	pr_info("noflip: removed after %lu skipped flips\n", skipped);
}

module_init(noflip_init);
module_exit(noflip_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Test only: make the PowerVR display class flip a no-op");
