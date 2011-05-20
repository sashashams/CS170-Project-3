
#define _SYSTEM 1

#include <minix/callnr.h>
#include <minix/com.h>
#include <minix/config.h>
#include <minix/const.h>
#include <minix/ds.h>
#include <minix/endpoint.h>
#include <minix/keymap.h>
#include <minix/minlib.h>
#include <minix/type.h>
#include <minix/ipc.h>
#include <minix/sysutil.h>
#include <minix/syslib.h>
#include <minix/safecopies.h>
#include <minix/bitmap.h>

#include <errno.h>
#include <string.h>
#include <env.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <assert.h>

#include <pagefaults.h>

#include "glo.h"
#include "proto.h"
#include "memory.h"
#include "util.h"
#include "region.h"

/*===========================================================================*
 *				pf_errstr	     		     	*
 *===========================================================================*/
char *pf_errstr(u32_t err)
{
	static char buf[100];

	sprintf(buf, "err 0x%lx ", err);
	if(PFERR_NOPAGE(err)) strcat(buf, "nopage ");
	if(PFERR_PROT(err)) strcat(buf, "protection ");
	if(PFERR_WRITE(err)) strcat(buf, "write");
	if(PFERR_READ(err)) strcat(buf, "read");

	return buf;
}

/*===========================================================================*
 *				do_pagefaults	     		     *
 *===========================================================================*/
PUBLIC void do_pagefaults(message *m)
{
	endpoint_t ep = m->m_source;
	u32_t addr = m->VPF_ADDR;
	u32_t err = m->VPF_FLAGS;
	struct vmproc *vmp;
	int r, s;

	struct vir_region *region;
	vir_bytes offset;
	int p, wr = PFERR_WRITE(err);

	if(vm_isokendpt(ep, &p) != OK)
		panic("do_pagefaults: endpoint wrong: %d", ep);

	vmp = &vmproc[p];
	assert(vmp->vm_flags & VMF_INUSE);

	/* See if address is valid at all. */
	if(!(region = map_lookup(vmp, addr))) {
		assert(PFERR_NOPAGE(err));
		printf("VM: pagefault: SIGSEGV %d bad addr %s; %s\n",
				ep, arch_map2str(vmp, addr), pf_errstr(err));
		if((s=sys_kill(vmp->vm_endpoint, SIGSEGV)) != OK)
			panic("sys_kill failed: %d", s);
		if((s=sys_vmctl(ep, VMCTL_CLEAR_PAGEFAULT, r)) != OK)
			panic("do_pagefaults: sys_vmctl failed: %d", ep);
		return;
	}

	/* Make sure this isn't a region that isn't supposed
	 * to cause pagefaults.
	 */
	assert(!(region->flags & VR_NOPF));

	/* We do not allow shared memory to cause pagefaults.
	 * These pages have to be pre-allocated.
	 */
	assert(!(region->flags & VR_SHARED));

	/* If process was writing, see if it's writable. */
	if(!(region->flags & VR_WRITABLE) && wr) {
		printf("VM: pagefault: SIGSEGV %d ro map 0x%lx %s\n",
				ep, arch_map2vir(vmp, addr), pf_errstr(err));
		if((s=sys_kill(vmp->vm_endpoint, SIGSEGV)) != OK)
			panic("sys_kill failed: %d", s);
		if((s=sys_vmctl(ep, VMCTL_CLEAR_PAGEFAULT, r)) != OK)
			panic("do_pagefaults: sys_vmctl failed: %d", ep);
		return;
	}

	assert(addr >= region->vaddr);
	offset = addr - region->vaddr;

	/* Access is allowed; handle it. */
	if((r=map_pf(vmp, region, offset, wr)) != OK) {
		printf("VM: pagefault: SIGSEGV %d pagefault not handled\n", ep);
		if((s=sys_kill(vmp->vm_endpoint, SIGSEGV)) != OK)
			panic("sys_kill failed: %d", s);
		if((s=sys_vmctl(ep, VMCTL_CLEAR_PAGEFAULT, r)) != OK)
			panic("do_pagefaults: sys_vmctl failed: %d", ep);
		return;
	}

	/* Pagefault is handled, so now reactivate the process. */
	if((s=sys_vmctl(ep, VMCTL_CLEAR_PAGEFAULT, r)) != OK)
		panic("do_pagefaults: sys_vmctl failed: %d", ep);
}

/*===========================================================================*
 *				   do_memory	     			     *
 *===========================================================================*/
PUBLIC void do_memory(void)
{
	endpoint_t who, who_s, requestor;
	vir_bytes mem, mem_s;
	vir_bytes len;
	int wrflag;

	while(1) {
		int p, r = OK;
		struct vmproc *vmp;

		r = sys_vmctl_get_memreq(&who, &mem, &len, &wrflag, &who_s,
			&mem_s, &requestor);

		switch(r) {
		case VMPTYPE_CHECK:
			if(vm_isokendpt(who, &p) != OK)
				panic("do_memory: bad endpoint: %d", who);
			vmp = &vmproc[p];

			r = handle_memory(vmp, mem, len, wrflag);
			break;
		case VMPTYPE_COWMAP:
			r = map_memory(who_s, who, mem_s, mem, len, -1);
			break;
		case VMPTYPE_SMAP:
			r = map_memory(who_s, who, mem_s, mem, len, wrflag);
			break;
		case VMPTYPE_SUNMAP:
			r = unmap_memory(who_s, who, mem_s, mem, len, wrflag);
			break;
		default:
			return;
		}

		if(sys_vmctl(requestor, VMCTL_MEMREQ_REPLY, r) != OK)
			panic("do_memory: sys_vmctl failed: %d", r);
	}
}

int handle_memory(struct vmproc *vmp, vir_bytes mem, vir_bytes len, int wrflag)
{
	struct vir_region *region;
	vir_bytes o;
	vir_bytes v;

	/* Page-align memory and length. */
	o = mem % VM_PAGE_SIZE;
	mem -= o;
	len += o;
	o = len % VM_PAGE_SIZE;
	if(o > 0) len += VM_PAGE_SIZE - o;

	while(len > 0) {
		int r;
		if(!(region = map_lookup(vmp, mem))) {
#if VERBOSE
			map_printmap(vmp);
			printf("VM: do_memory: memory doesn't exist\n");
#endif
			r = EFAULT;
		} else if(!(region->flags & VR_WRITABLE) && wrflag) {
#if VERBOSE
			printf("VM: do_memory: write to unwritable map\n");
#endif
			r = EFAULT;
		} else {
			vir_bytes offset, sublen;
			assert(region->vaddr <= mem);
			assert(!(region->flags & VR_NOPF));
			assert(!(region->vaddr % VM_PAGE_SIZE));
			offset = mem - region->vaddr;
			sublen = len;
			if(offset + sublen > region->length)
				sublen = region->length - offset;
	
			r = map_handle_memory(vmp, region, offset,
				sublen, wrflag);

			len -= sublen;
			mem += sublen;
		}
	
		if(r != OK) {
#if VERBOSE
			printf("VM: memory range 0x%lx-0x%lx not available in %d\n",
				arch_map2vir(vmp, mem), arch_map2vir(vmp, mem+len),
				vmp->vm_endpoint);
#endif
			return r;
		}
	}

	return OK;
}

