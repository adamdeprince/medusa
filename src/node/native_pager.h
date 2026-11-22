/* SPDX-License-Identifier: GPL-2.0-only */
// Optional master-kernel ABI from kernel/patches/0007. Keep building with
// stock UAPI headers, including on agents that never enable these operations.
#pragma once

#include <linux/userfaultfd.h>
#include <sys/ioctl.h>

#ifndef DNUMA_UFFD_ATTACH
struct dnuma_uffd_target {
    __s32 pidfd;
    __u32 flags;
};
#define DNUMA_UFFD_ATTACH _IOW(0xd7, 0x01, struct dnuma_uffd_target)
#define DNUMA_UFFD_EVICT  _IOW(0xd7, 0x02, struct uffdio_range)
#endif
