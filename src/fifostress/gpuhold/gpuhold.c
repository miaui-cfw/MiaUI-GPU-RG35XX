/**
 * @file gpuhold.c
 * @brief D13 test module (tests only): holds one extra reference on the GPU3D power domain, so the PowerVR
 *        driver's active power management (SGX off after 2 ms idle, ~86 times a second) no longer removes the
 *        domain's power. Everything else of the GPU driver's per-frame cycle stays as it is: on each power-on
 *        owl_powergate_set() still asserts and releases the GPU reset (GPU3D has no power ack), and the GPU module
 *        clock is still gated by owl_gpu_clk_disable(), which this module cannot hold (module_clk_enable is not
 *        reference-counted).
 *
 * Load it only with no GL client running (MinUI parked): taking the reference powers the domain on with a GPU reset.
 * Unload it the same way. /sys/kernel/debug/powergate shows the count.
 */
#include <linux/module.h>
#include <mach/powergate.h>

/**
 * @brief Takes the extra power reference on GPU3D.
 * @return The result of owl_powergate_power_on().
 */
static int __init gpuhold_init(void)
{
	int r = owl_powergate_power_on(OWL_POWERGATE_GPU3D);

	pr_info("gpuhold: GPU3D power domain held (%d)\n", r);
	return r;
}

/** @brief Drops the reference (the domain powers off if the GPU driver holds none). */
static void __exit gpuhold_exit(void)
{
	owl_powergate_power_off(OWL_POWERGATE_GPU3D);
	pr_info("gpuhold: GPU3D power domain released\n");
}

module_init(gpuhold_init);
module_exit(gpuhold_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Test only: keep the GPU3D power domain on");
