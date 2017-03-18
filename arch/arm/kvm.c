#include "kvm/kvm.h"
#include "kvm/term.h"
#include "kvm/util.h"
#include "kvm/8250-serial.h"
#include "kvm/virtio-console.h"
#include "kvm/fdt.h"

#include "arm-common/gic.h"

#include <linux/kernel.h>
#include <linux/kvm.h>
#include <linux/sizes.h>

struct kvm_ext kvm_req_ext[] = {
	{ DEFINE_KVM_EXT(KVM_CAP_IRQCHIP) },
	{ DEFINE_KVM_EXT(KVM_CAP_ONE_REG) },
	{ DEFINE_KVM_EXT(KVM_CAP_ARM_PSCI) },
	{ 0, 0 },
};

bool kvm__arch_cpu_supports_vm(void)
{
	/* The KVM capability check is enough. */
	return true;
}

void kvm__init_ram(struct kvm *kvm)
{
	int err;
	u64 phys_start, phys_size;
	void *host_mem;

	phys_start	= ARM_MEMORY_AREA;
	phys_size	= kvm->ram_size;
	host_mem	= kvm->ram_start;

	err = kvm__register_mem(kvm, phys_start, phys_size, host_mem);
	if (err)
		die("Failed to register %lld bytes of memory at physical "
		    "address 0x%llx [err %d]", phys_size, phys_start, err);

	kvm->arch.memory_guest_start = phys_start;
}

void kvm__arch_delete_ram(struct kvm *kvm)
{
	munmap(kvm->arch.ram_alloc_start, kvm->arch.ram_alloc_size);
}

void kvm__arch_read_term(struct kvm *kvm)
{
	if (term_readable(0)) {
		serial8250__update_consoles(kvm);
		virtio_console__inject_interrupt(kvm);
	}
}

void kvm__arch_set_cmdline(char *cmdline, bool video)
{
}

void kvm__arch_init(struct kvm *kvm, const char *hugetlbfs_path, u64 ram_size)
{
	/*
	 * Allocate guest memory. We must align our buffer to 64K to
	 * correlate with the maximum guest page size for virtio-mmio.
	 * If using THP, then our minimal alignment becomes 2M.
	 * 2M trumps 64K, so let's go with that.
	 */
	kvm->ram_size = min(ram_size, (u64)ARM_MAX_MEMORY(kvm));
	kvm->arch.ram_alloc_size = kvm->ram_size + SZ_2M;
	kvm->arch.ram_alloc_start = mmap_anon_or_hugetlbfs(kvm, hugetlbfs_path,
						kvm->arch.ram_alloc_size);

	if (kvm->arch.ram_alloc_start == MAP_FAILED)
		die("Failed to map %lld bytes for guest memory (%d)",
		    kvm->arch.ram_alloc_size, errno);

	kvm->ram_start = (void *)ALIGN((unsigned long)kvm->arch.ram_alloc_start,
					SZ_2M);

	madvise(kvm->arch.ram_alloc_start, kvm->arch.ram_alloc_size,
		MADV_MERGEABLE);

	madvise(kvm->arch.ram_alloc_start, kvm->arch.ram_alloc_size,
		MADV_HUGEPAGE);

	/* Create the virtual GIC. */
	if (gic__create(kvm, kvm->cfg.arch.irqchip))
		die("Failed to create virtual GIC");
}

#define FDT_ALIGN	SZ_2M
#define INITRD_ALIGN	4
bool kvm__arch_load_kernel_image(struct kvm *kvm, int fd_kernel, int fd_initrd,
				 const char *kernel_cmdline)
{
	void *pos, *kernel_end, *limit;
	unsigned long guest_addr;
	ssize_t file_size;

	/*
	 * Linux requires the initrd and dtb to be mapped inside lowmem,
	 * so we can't just place them at the top of memory.
	 */
	limit = kvm->ram_start + min(kvm->ram_size, (u64)SZ_256M) - 1;

	pos = kvm->ram_start + ARM_KERN_OFFSET(kvm);
	kvm->arch.kern_guest_start = host_to_guest_flat(kvm, pos);
	file_size = read_file(fd_kernel, pos, limit - pos);
	if (file_size < 0) {
		if (errno == ENOMEM)
			die("kernel image too big to contain in guest memory.");

		die_perror("kernel read");
	}
	kernel_end = pos + file_size;
	pr_info("Loaded kernel to 0x%llx (%zd bytes)",
		kvm->arch.kern_guest_start, file_size);

	/*
	 * Now load backwards from the end of memory so the kernel
	 * decompressor has plenty of space to work with. First up is
	 * the device tree blob...
	 */
	pos = limit;
	pos -= (FDT_MAX_SIZE + FDT_ALIGN);
	guest_addr = ALIGN(host_to_guest_flat(kvm, pos), FDT_ALIGN);
	pos = guest_flat_to_host(kvm, guest_addr);
	if (pos < kernel_end)
		die("fdt overlaps with kernel image.");

	kvm->arch.dtb_guest_start = guest_addr;
	pr_info("Placing fdt at 0x%llx - 0x%llx",
		kvm->arch.dtb_guest_start,
		host_to_guest_flat(kvm, limit));
	limit = pos;

	/* ... and finally the initrd, if we have one. */
	if (fd_initrd != -1) {
		struct stat sb;
		unsigned long initrd_start;

		if (fstat(fd_initrd, &sb))
			die_perror("fstat");

		pos -= (sb.st_size + INITRD_ALIGN);
		guest_addr = ALIGN(host_to_guest_flat(kvm, pos), INITRD_ALIGN);
		pos = guest_flat_to_host(kvm, guest_addr);
		if (pos < kernel_end)
			die("initrd overlaps with kernel image.");

		initrd_start = guest_addr;
		file_size = read_file(fd_initrd, pos, limit - pos);
		if (file_size == -1) {
			if (errno == ENOMEM)
				die("initrd too big to contain in guest memory.");

			die_perror("initrd read");
		}

		kvm->arch.initrd_guest_start = initrd_start;
		kvm->arch.initrd_size = file_size;
		pr_info("Loaded initrd to 0x%llx (%llu bytes)",
			kvm->arch.initrd_guest_start,
			kvm->arch.initrd_size);
	} else {
		kvm->arch.initrd_size = 0;
	}

	return true;
}
