/**
 * @file desampler.c
 * @brief DE underflow sampler (tests only): samples the display engine and the DDR controller's bandwidth counters
 *        every few tens of microseconds after each LCD vblank, and keeps the frames in which the DE's FIFO underflowed
 *        (plus the frame before each, and one normal frame every `ref_every` frames as a reference).
 *
 * At every LCD pre-line interrupt (callback registered with the DE driver, so it runs at the start of vblank) the
 * four DMM performance counters are reset and restarted: PC0 = DE read, PC1 = GPU ("SI", id 7) read+write,
 * PC2 = CPU read+write, PC3 = all masters read+write (16-byte units). An hrtimer then samples, `dense` times every
 * `period_us`, and every `sparse_us` after that until the next vblank:
 *   DE_IRQSTATUS, DE_OUTPUT_STAT (sticky underflow bit, cleared by the kernel's de_path_fcr_set at each flip),
 *   DE_PATH_CTL(0) (bit 29 = flip "go" pending) and PC0..PC3.
 * Hardware writes: only the DMM performance-monitor control registers (PM_CTRL0/1). Nothing else is written.
 *
 * Readout: `cat /proc/desampler` (freezes the saved frames while it prints); `echo clear > /proc/desampler` empties
 * them and resumes. Decoded on the PC by desampler.py.
 */
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/kallsyms.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#define DE_PHYS 0xB02E0000
#define DE_SIZE 0x2000
#define DE_IRQSTATUS 0x0004
#define DE_PATH_CTL0 0x0100
#define DE_OUTPUT_STAT 0x100c

#define DMM_PHYS 0xB0290000
#define DMM_SIZE 0x100
#define DMM_PM_CTRL0 0x48
#define DMM_PM_CTRL1 0x4c
#define DMM_PC0 0x50

#define DE_IRQ_LCD_PRE 0x08 /* enum de_irq_type in drivers/video/owl/dss/de.h */
#define MAX_SAMPLES 200
#define RING 4
#define MAX_SAVED 48

static unsigned int period_us = 50;
module_param(period_us, uint, 0444);
MODULE_PARM_DESC(period_us, "dense sampling period after vblank (us)");
static unsigned int dense = 120;
module_param(dense, uint, 0444);
MODULE_PARM_DESC(dense, "number of dense samples per frame");
static unsigned int sparse_us = 500;
module_param(sparse_us, uint, 0444);
MODULE_PARM_DESC(sparse_us, "sampling period after the dense window (us)");
static unsigned int ref_every = 600;
module_param(ref_every, uint, 0444);
MODULE_PARM_DESC(ref_every, "save one normal frame every N frames as a reference (0 = never)");
/* DMM master ids (owl-ddr-debug.c numbering; on the S500, 7 carries the GPU) and modes (0 R, 1 W, 2 RW) */
static unsigned int m_id[4] = { 5, 7, 0, 10 };
static unsigned int m_mode[4] = { 0, 2, 2, 2 };
module_param_array(m_id, uint, NULL, 0444);
module_param_array(m_mode, uint, NULL, 0444);

/** @brief One sample: time since vblank and the registers. */
struct sample {
	u32 t_ns;
	u32 irqstat;
	u32 outstat;
	u32 pathctl;
	u32 pc[4];
};

/** @brief One frame: its vblank time and its samples. */
struct frame {
	u64 seq;
	u64 vb_ns;	/* local_clock() at the vblank callback, comparable with printk timestamps */
	u32 n;
	u32 kind;	/* 0 normal, 1 underflow seen, 2 frame before an underflow, 3 reference */
	struct sample s[MAX_SAMPLES];
};

typedef int (*reg_isr_t)(u32 irq, void (*isr)(u32, void *), void *arg);

static void __iomem *de_base;
static void __iomem *dmm_base;
static reg_isr_t reg_isr, unreg_isr;
static struct hrtimer timer;
static struct frame *ring;	/* RING frames being filled, by seq % RING */
static struct frame *saved;	/* MAX_SAVED kept frames */
static unsigned int nsaved;
static bool frozen;
static u32 seq;	/* frames since load (32-bit: no 64-bit division in the kernel) */
static DEFINE_SPINLOCK(lock);

/**
 * @brief Programs and restarts the four DMM counters (writing 0 clears them).
 */
static void dmm_restart(void)
{
	u32 c[2] = { 0, 0 };
	int i;

	for (i = 0; i < 4; i++) {
		u32 v = (m_mode[i] << 4) | (1 << 15);

		if (m_id[i] == 10)
			v |= 1 << 8;		/* all masters */
		else if (m_id[i] == 11)
			v |= 2 << 8;		/* idle */
		else
			v |= m_id[i];
		c[i / 2] |= v << ((i % 2) * 16);
	}
	writel(0, dmm_base + DMM_PM_CTRL0);
	writel(0, dmm_base + DMM_PM_CTRL1);
	writel(c[0], dmm_base + DMM_PM_CTRL0);
	writel(c[1], dmm_base + DMM_PM_CTRL1);
}

/**
 * @brief Saves a ring frame into the kept list (caller holds the lock).
 * @param f    The frame.
 * @param kind Why it is kept.
 */
static void keep(const struct frame *f, u32 kind)
{
	if (frozen || nsaved >= MAX_SAVED || !f->n)
		return;
	memcpy(&saved[nsaved], f, offsetof(struct frame, s) + f->n * sizeof(struct sample));
	saved[nsaved].kind = kind;
	nsaved++;
}

/**
 * @brief Whether a frame saw the DE underflow bit in any sample.
 */
static bool has_underflow(const struct frame *f)
{
	u32 i;

	for (i = 0; i < f->n; i++)
		if (f->s[i].outstat)
			return true;
	return false;
}

/**
 * @brief hrtimer callback: takes one sample of the current frame, re-arms until the frame is full.
 */
static enum hrtimer_restart sample_fn(struct hrtimer *t)
{
	struct frame *f = &ring[seq % RING];
	struct sample *s;
	u32 n = f->n;
	int i;

	if (n >= MAX_SAMPLES)
		return HRTIMER_NORESTART;
	s = &f->s[n];
	s->t_ns = (u32)(local_clock() - f->vb_ns);
	s->irqstat = readl_relaxed(de_base + DE_IRQSTATUS);
	s->outstat = readl_relaxed(de_base + DE_OUTPUT_STAT);
	s->pathctl = readl_relaxed(de_base + DE_PATH_CTL0);
	for (i = 0; i < 4; i++)
		s->pc[i] = readl_relaxed(dmm_base + DMM_PC0 + 4 * i);
	f->n = n + 1;
	hrtimer_forward_now(t, ns_to_ktime((n + 1 < dense ? period_us : sparse_us) * 1000ULL));
	return HRTIMER_RESTART;
}

/**
 * @brief DE LCD pre-line callback (hard IRQ, at the start of vblank): closes the previous frame, keeps it if it
 *        underflowed, starts a new one.
 */
static void vblank_isr(u32 irq, void *arg)
{
	struct frame *prev, *f;
	unsigned long flags;

	hrtimer_try_to_cancel(&timer);
	spin_lock_irqsave(&lock, flags);
	prev = &ring[seq % RING];
	if (seq && has_underflow(prev)) {
		struct frame *before = &ring[(seq - 1) % RING];

		if (before->kind != 1 && !has_underflow(before))
			keep(before, 2);
		keep(prev, 1);
		prev->kind = 1;
	} else if (seq && ref_every && seq % ref_every == 0) {
		keep(prev, 3);
	}
	seq++;
	f = &ring[seq % RING];
	f->seq = seq;
	f->vb_ns = local_clock();
	f->n = 0;
	f->kind = 0;
	spin_unlock_irqrestore(&lock, flags);
	dmm_restart();
	hrtimer_start(&timer, ns_to_ktime(period_us * 1000ULL), HRTIMER_MODE_REL_PINNED);
}

/**
 * @brief seq_file start: position 0 is the header, position i + 1 is kept frame i. Freezes the kept list.
 */
static void *seq_start_fn(struct seq_file *m, loff_t *pos)
{
	unsigned long flags;

	spin_lock_irqsave(&lock, flags);
	frozen = true;
	spin_unlock_irqrestore(&lock, flags);
	return *pos <= nsaved ? (void *)(uintptr_t)(*pos + 1) : NULL;
}

/**
 * @brief seq_file next: the following kept frame.
 */
static void *seq_next_fn(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return *pos <= nsaved ? (void *)(uintptr_t)(*pos + 1) : NULL;
}

/**
 * @brief seq_file stop: nothing to release (the list stays frozen until cleared).
 */
static void seq_stop_fn(struct seq_file *m, void *v)
{
}

/**
 * @brief Prints the header (first element) or one kept frame: "F seq vblank_ns kind n", then its samples
 *        "t_ns irqstat outstat pathctl pc0 pc1 pc2 pc3".
 */
static int seq_show_fn(struct seq_file *m, void *v)
{
	unsigned int idx = (unsigned int)(uintptr_t)v - 1, j;
	struct frame *f;

	if (idx == 0) {
		seq_printf(m, "# desampler period_us %u dense %u sparse_us %u ids %u/%u %u/%u %u/%u %u/%u frames %u kept %u\n",
			   period_us, dense, sparse_us, m_id[0], m_mode[0], m_id[1], m_mode[1], m_id[2], m_mode[2],
			   m_id[3], m_mode[3], seq, nsaved);
		return 0;
	}
	f = &saved[idx - 1];
	seq_printf(m, "F %llu %llu %u %u\n", f->seq, f->vb_ns, f->kind, f->n);
	for (j = 0; j < f->n; j++) {
		struct sample *s = &f->s[j];

		seq_printf(m, "%u %x %x %x %u %u %u %u\n", s->t_ns, s->irqstat, s->outstat, s->pathctl,
			   s->pc[0], s->pc[1], s->pc[2], s->pc[3]);
	}
	return 0;
}

static const struct seq_operations seq_ops = {
	.start = seq_start_fn,
	.next = seq_next_fn,
	.stop = seq_stop_fn,
	.show = seq_show_fn,
};

/**
 * @brief Opens /proc/desampler.
 */
static int open_fn(struct inode *inode, struct file *file)
{
	return seq_open(file, &seq_ops);
}

/**
 * @brief Any write empties the kept frames and resumes saving.
 */
static ssize_t write_fn(struct file *file, const char __user *buf, size_t len, loff_t *ppos)
{
	unsigned long flags;

	spin_lock_irqsave(&lock, flags);
	nsaved = 0;
	frozen = false;
	spin_unlock_irqrestore(&lock, flags);
	return len;
}

static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = open_fn,
	.read = seq_read,
	.write = write_fn,
	.llseek = seq_lseek,
	.release = seq_release,
};

/**
 * @brief Maps the registers, allocates the buffers and registers the vblank callback.
 */
static int __init desampler_init(void)
{
	if (dense > MAX_SAMPLES || !period_us || !sparse_us)
		return -EINVAL;
	reg_isr = (reg_isr_t)kallsyms_lookup_name("owl_de_register_isr");
	unreg_isr = (reg_isr_t)kallsyms_lookup_name("owl_de_unregister_isr");
	if (!reg_isr || !unreg_isr)
		return -ENOENT;
	ring = vzalloc(RING * sizeof(*ring));
	saved = vzalloc(MAX_SAVED * sizeof(*saved));
	de_base = ioremap(DE_PHYS, DE_SIZE);
	dmm_base = ioremap(DMM_PHYS, DMM_SIZE);
	if (!ring || !saved || !de_base || !dmm_base)
		goto fail;
	hrtimer_init(&timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
	timer.function = sample_fn;
	if (!proc_create("desampler", 0600, NULL, &fops))
		goto fail;
	if (reg_isr(DE_IRQ_LCD_PRE, vblank_isr, NULL)) {
		remove_proc_entry("desampler", NULL);
		goto fail;
	}
	pr_info("desampler: sampling every %u us x %u after vblank, then every %u us\n", period_us, dense, sparse_us);
	return 0;
fail:
	if (de_base)
		iounmap(de_base);
	if (dmm_base)
		iounmap(dmm_base);
	vfree(ring);
	vfree(saved);
	return -ENOMEM;
}

/**
 * @brief Unregisters the callback, stops the timer and frees everything.
 */
static void __exit desampler_exit(void)
{
	unreg_isr(DE_IRQ_LCD_PRE, vblank_isr, NULL);
	hrtimer_cancel(&timer);
	remove_proc_entry("desampler", NULL);
	writel(0, dmm_base + DMM_PM_CTRL0);
	writel(0, dmm_base + DMM_PM_CTRL1);
	iounmap(de_base);
	iounmap(dmm_base);
	vfree(ring);
	vfree(saved);
}

module_init(desampler_init);
module_exit(desampler_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RG35XX DE underflow sampler (tests only)");
