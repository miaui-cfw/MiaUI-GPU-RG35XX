/**
 * @file apmlat.c
 * @brief D18 test module (tests only): changes the PowerVR SGX active power management latency without rebuilding
 *        pvrsrvkm. SysGetSGXTimingInformation() (services4/system/omap/sysutils_linux.c) fills the
 *        SGX_TIMING_INFORMATION that SGXUpdateTimingInfo() hands to the SGX microkernel at every power-up; its
 *        ui32ActivePowManLatencyms is SYS_SGX_ACTIVE_POWER_LATENCY_MS = 2, so the GPU saves its state and powers off
 *        2 ms after every frame (~86 power cycles a second at 60 fps). A kretprobe rewrites the latency after the
 *        function returns.
 *
 *   latency_ms (rw)  0: leave the driver's value (2 ms); N: use N ms from the next GPU power-up on
 *   powerups (ro)    calls seen = GPU power-ups (timing info is regenerated at each)
 *
 * The struct offsets are those of the console's pvrsrvkm.ko (md5 8d33b1ee): +8 bEnableActivePM, +12 latency; the
 * module checks bEnableActivePM == 1 and latency == 2 before writing, and leaves the struct alone otherwise.
 */
#include <linux/kprobes.h>
#include <linux/module.h>

static int latency_ms;
module_param(latency_ms, int, 0644);
MODULE_PARM_DESC(latency_ms, "0: driver default (2 ms); N: SGX active power latency in ms");

static unsigned long powerups, rewritten;
module_param(powerups, ulong, 0444);
module_param(rewritten, ulong, 0444);

/** @brief What the entry handler keeps for the return handler: the struct pointer (first argument). */
struct call { u32* info; };

/** @brief Entry: saves the SGX_TIMING_INFORMATION pointer. @return 0. */
static int on_entry(struct kretprobe_instance* ri, struct pt_regs* regs)
{
	((struct call*)ri->data)->info = (u32*)regs->ARM_r0;
	return 0;
}

/** @brief Return: counts the power-up and, if asked, rewrites the latency. @return 0. */
static int on_return(struct kretprobe_instance* ri, struct pt_regs* regs)
{
	u32* info = ((struct call*)ri->data)->info;
	int lat = ACCESS_ONCE(latency_ms);

	powerups++;
	if (lat > 0 && info && info[2] == 1 && info[3] == 2) {
		info[3] = lat;
		rewritten++;
	}
	return 0;
}

static struct kretprobe krp = {
	.kp.symbol_name = "SysGetSGXTimingInformation",
	.entry_handler = on_entry,
	.handler = on_return,
	.data_size = sizeof(struct call),
	.maxactive = 4,
};

/** @brief Arms the kretprobe. @return 0, or the register error (pvrsrvkm not loaded). */
static int __init apmlat_init(void)
{
	int r = register_kretprobe(&krp);

	pr_info("apmlat: SysGetSGXTimingInformation %s (%d), latency_ms %d\n", r ? "NOT probed" : "probed", r,
		latency_ms);
	return r;
}

/** @brief Disarms it; the driver's own 2 ms applies again from the next GPU power-up. */
static void __exit apmlat_exit(void)
{
	unregister_kretprobe(&krp);
	pr_info("apmlat: removed (%lu power-ups, %lu rewritten)\n", powerups, rewritten);
}

module_init(apmlat_init);
module_exit(apmlat_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Test only: PowerVR SGX active power management latency");
