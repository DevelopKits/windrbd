﻿/*
	Copyright(C) 2017-2018, Johannes Thoma <johannes@johannesthoma.com>
	Copyright(C) 2017-2018, LINBIT HA-Solutions GmbH  <office@linbit.com>
	Copyright(C) 2007-2016, ManTechnology Co., LTD.
	Copyright(C) 2007-2016, wdrbd@mantech.co.kr

	Windows DRBD is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2, or (at your option)
	any later version.

	Windows DRBD is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Windows DRBD; see the file COPYING. If not, write to
	the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "drbd_windows.h"
#include "windrbd_device.h"
#include <wdmsec.h>
#include <ntdddisk.h>
#include <wdm.h>

/* TODO: either fix the mountmanager code or throw it away */
#if 0
#include <initguid.h>
DEFINE_GUID(MOUNTDEV_MOUNTED_DEVICE_GUID, 0x53F5630D, 0xB6BF, 0x11D0, 0x94, 0xF2, 0x00, 0xA0, 0xC9, 0x1E, 0xFB, 0x8B);

/* TODO: randomize */
DEFINE_GUID(DRBD_DEVICE_CLASS_GUID, 0x53F5630A, 0xB6BF, 0x11D0, 0x94, 0xF2, 0x00, 0xA0, 0xC9, 0x1E, 0xFB, 0x8B);
#endif

/* Does not work: unresolved external symbol MOUNTDEV_MOUNTED_DEVICE_GUID */
/* #include <mountmgr.h> */
	/* Define this if you want a built in test for backing device
	   I/O. Attention this destroys data on the back device.
	   Note that submit_bio may fail on some systems, therefore
	   leave this commented out for now.
	 */
// #define _HACK
// #define _HACK_WRITE

#undef _NTDDK_
/* Can't include that without getting redefinition errors.
 *   10.0.14393.0\km\ntddk.h(69): error C2371: 'PEPROCESS': redefinition; different basic types
 *   10.0.14393.0\km\wdm.h(84): note: see declaration of 'PEPROCESS'
#include <ntifs.h>
 *   */
NTSTATUS
NTAPI
ZwCreateEvent (
	_Out_ PHANDLE EventHandle,
	_In_ ACCESS_MASK DesiredAccess,
	_In_opt_ POBJECT_ATTRIBUTES ObjectAttributes,
	_In_ EVENT_TYPE EventType,
	_In_ BOOLEAN InitialState
	);
NTSTATUS
NTAPI
ZwWaitForSingleObject(
	_In_ HANDLE Handle,
	_In_ BOOLEAN Alertable,
	_In_opt_ PLARGE_INTEGER Timeout
	);


ULONG RtlRandomEx(
  _Inout_ PULONG Seed
);


#include <ntddk.h>
#include <stdint.h>
#include <stdarg.h>
#include <intrin.h>
#include "wsk2.h"
#include "drbd_wingenl.h"
#include "linux/idr.h"
#include "drbd_wrappers.h"
#include "disp.h"

	/* TODO: these should go away. */
long		gLogCnt = 0;
LONGLONG 	gTotalLogCnt = 0;
char		gLogBuf[LOGBUF_MAXCNT][MAX_DRBDLOG_BUF] = {0,};

int g_netlink_tcp_port;
int g_daemon_tcp_port;

// minimum levels of logging, below indicates default values. it can be changed when WDRBD receives IOCTL_MVOL_SET_LOGLV_MIN.
atomic_t g_eventlog_lv_min = LOG_LV_DEFAULT_EVENTLOG;
atomic_t g_dbglog_lv_min = LOG_LV_DEFAULT_DBG;
#ifdef _WIN32_DEBUG_OOS
atomic_t g_oos_trace = 0;
#endif

int g_handler_use;
int g_handler_timeout;
int g_handler_retry;

#define MAX_IDR_SHIFT		(sizeof(int) * 8 - 1)
#define MAX_IDR_BIT		(1U << MAX_IDR_SHIFT)

/* Leave the possibility of an incomplete final layer */
#define MAX_IDR_LEVEL ((MAX_IDR_SHIFT + IDR_BITS - 1) / IDR_BITS)

/* Number of id_layer structs to leave in free list */
#define MAX_IDR_FREE (MAX_IDR_LEVEL * 2)


extern SIMULATION_DISK_IO_ERROR gSimulDiskIoError = {0,};

// DW-1105: monitoring mount change thread state (FALSE : not working, TRUE : working)
atomic_t g_monitor_mnt_working = FALSE;

static LIST_HEAD(backing_devices);
static struct mutex read_bootsector_mutex;

void msleep(int ms)
{
	LARGE_INTEGER d;

	if (KeGetCurrentIrql() >= DISPATCH_LEVEL) {
		/* Maybe busy loop? */
		printk("msleep called with irqlevel > DISPATCH_LEVEL (%d)\n", KeGetCurrentIrql());
	} else {
		d.QuadPart = -10000LL * ms;
		KeDelayExecutionThread(KernelMode, FALSE, &d);
	}
}

void init_windrbd(void)
{
	mutex_init(&read_bootsector_mutex);
}

//__ffs - find first bit in word.
ULONG_PTR __ffs(ULONG_PTR word) 
{
	int num = 0;

#if BITS_PER_LONG == 64
	if ((word & 0xffffffff) == 0) {
		num += 32;
		word >>= 32;
	}
#endif
	if ((word & 0xffff) == 0) {
		num += 16;
		word >>= 16;
	}
	if ((word & 0xff) == 0) {
		num += 8;
		word >>= 8;
	}
	if ((word & 0xf) == 0) {
		num += 4;
		word >>= 4;
	}
	if ((word & 0x3) == 0) {
		num += 2;
		word >>= 2;
	}
	if ((word & 0x1) == 0)
		num += 1;
	return num;
}

#define ffz(x)  __ffs(~(x))

int fls(int x)
{
	int r = 32;

	if (!x)
		return 0;
	if (!(x & 0xffff0000u)) {
		x <<= 16;
		r -= 16;
	}
	if (!(x & 0xff000000u)) {
		x <<= 8;
		r -= 8;
	}
	if (!(x & 0xf0000000u)) {
		x <<= 4;
		r -= 4;
	}
	if (!(x & 0xc0000000u)) {
		x <<= 2;
		r -= 2;
	}
	if (!(x & 0x80000000u)) {
		x <<= 1;
		r -= 1;
	}
	return r;
}

#define BITOP_WORD(nr)          ((nr) / BITS_PER_LONG)

ULONG_PTR find_first_bit(const ULONG_PTR* addr, ULONG_PTR size)
{
	const ULONG_PTR* p = addr;
	ULONG_PTR result = 0;
	ULONG_PTR tmp;

	while (size & ~(BITS_PER_LONG - 1)) {
		if ((tmp = *(p++)))
			goto found;
		result += BITS_PER_LONG;
		size -= BITS_PER_LONG;
	}
	if (!size)
		return result;
#ifdef _WIN64
	tmp = (*p) & (~0ULL >> (BITS_PER_LONG - size));
	if (tmp == 0ULL)	{	/* Are any bits set? */
#else
	tmp = (*p) & (~0UL >> (BITS_PER_LONG - size));
	if (tmp == 0UL)	{	/* Are any bits set? */
#endif
		return result + size;	/* Nope. */
	}
found:
	return result + __ffs(tmp);
}

ULONG_PTR find_next_bit(const ULONG_PTR *addr, ULONG_PTR size, ULONG_PTR offset)
{
	const ULONG_PTR *p = addr + BITOP_WORD(offset);
	ULONG_PTR result = offset & ~(BITS_PER_LONG - 1);
	ULONG_PTR tmp;

	if (offset >= size)
		return size;
	size -= result;
	offset %= BITS_PER_LONG;
	if (offset) {
		tmp = *(p++);
#ifdef _WIN64
		tmp &= (~0ULL << offset);
#else
		tmp &= (~0UL << offset);
#endif
		if (size < BITS_PER_LONG)
			goto found_first;
		if (tmp)
			goto found_middle;
		size -= BITS_PER_LONG;
		result += BITS_PER_LONG;
	}
	while (size & ~(BITS_PER_LONG - 1)) {
		if ((tmp = *(p++)))
			goto found_middle;
		result += BITS_PER_LONG;
		size -= BITS_PER_LONG;
	}
	if (!size)
		return result;
	tmp = *p;

found_first:
#ifdef _WIN64
	tmp &= (~0ULL >> (BITS_PER_LONG - size));
	if (tmp == 0ULL)	/* Are any bits set? */
#else
	tmp &= (~0UL >> (BITS_PER_LONG - size));
	if (tmp == 0UL)		/* Are any bits set? */
#endif
		return result + size;	/* Nope. */
found_middle:
	return result + __ffs(tmp);
}

const char _zb_findmap [] = {
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 5,
    0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,
    0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,6,
    0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,
    0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,5,
    0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,4,
    0,1,0,2,0,1,0,3,0,1,0,2,0,1,0,7,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 5,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 6,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 5,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 8 };

static inline ULONG_PTR __ffz_word(ULONG_PTR nr, ULONG_PTR word)
 {
 #ifdef _WIN64
    if ((word & 0xffffffff) == 0xffffffff) {
            word >>= 32;
            nr += 32;
    }
 #endif
    if ((word & 0xffff) == 0xffff) {
            word >>= 16;
            nr += 16;
    }
    if ((word & 0xff) == 0xff) {
            word >>= 8;
            nr += 8;
    }
	return nr + _zb_findmap[(unsigned char) word];
 }
 /*
 * Find the first cleared bit in a memory region.
 */
ULONG_PTR find_first_zero_bit(const ULONG_PTR *addr, ULONG_PTR size)
 {
	const ULONG_PTR *p = addr;
	ULONG_PTR result = 0;
	ULONG_PTR tmp;

	 while (size & ~(BITS_PER_LONG - 1)) {
		 if (~(tmp = *(p++)))
			 goto found;
		 result += BITS_PER_LONG;
		 size -= BITS_PER_LONG;
	 }
	 if (!size)
		 return result;

#ifdef _WIN64
	 tmp = (*p) | (~0ULL << size);
	 if (tmp == ~0ULL)        /* Are any bits zero? */
#else
	 tmp = (*p) | (~0UL << size);
	 if (tmp == ~0UL)        /* Are any bits zero? */
#endif
		 return result + size;        /* Nope. */
 found:
	 return result + ffz(tmp);
 }

int find_next_zero_bit(const ULONG_PTR * addr, ULONG_PTR size, ULONG_PTR offset)
{
	const ULONG_PTR *p;
	ULONG_PTR bit, set;
 
    if (offset >= size)
            return size;
    bit = offset & (BITS_PER_LONG - 1);
    offset -= bit;
    size -= offset;
    p = addr + offset / BITS_PER_LONG;
    if (bit) {
        /*
        * __ffz_word returns BITS_PER_LONG
        * if no zero bit is present in the word.
        */
        set = __ffz_word(bit, *p >> bit);
        if (set >= size)
                return size + offset;
        if (set < BITS_PER_LONG)
                return set + offset;
        offset += BITS_PER_LONG;
        size -= BITS_PER_LONG;
        p++;
    }

    return offset + find_first_zero_bit(p, size);
 }

static int g_test_and_change_bit_flag = 0;
static spinlock_t g_test_and_change_bit_lock;

int test_and_change_bit(int nr, const ULONG_PTR *addr)
{
	ULONG_PTR mask = BIT_MASK(nr);
	ULONG_PTR *p = ((ULONG_PTR *) addr);
	ULONG_PTR old;
	ULONG_PTR flags;

	/* TODO: racy. Someone might call spin_lock_irq and somebody
	 * else later spin_lock_init. We should have a function that
	 * initializes all the spin locks before anything else happens.
	 */

	if (!g_test_and_change_bit_flag)
	{
		spin_lock_init(&g_test_and_change_bit_lock);
		g_test_and_change_bit_flag = 1;
	}

	spin_lock_irq(&g_test_and_change_bit_lock);
	old = *p;
	*p = old ^ mask;
	spin_unlock_irq(&g_test_and_change_bit_lock);

    return (old & mask) != 0;
}

LONG_PTR xchg(LONG_PTR *target, LONG_PTR value)
{
#ifdef _WIN64
	return (InterlockedExchange64(target, value));
#else
	return (InterlockedExchange(target, value));
#endif
}


void atomic_set(atomic_t *v, int i)
{
	InterlockedExchange((long *)v, i);
}

void atomic_add(int i, atomic_t *v)
{
	InterlockedExchangeAdd((long *)v, i);
}

void atomic_add64(LONGLONG a, atomic_t64 *v)
{
	InterlockedExchangeAdd64((LONGLONG*)v, a);
}

void atomic_sub(int i, atomic_t *v)
{
	atomic_sub_return(i, v);
}

void atomic_sub64(LONGLONG a, atomic_t64 *v)
{
	atomic_sub_return64(a, v);
}

	/* TODO: atomic? Results may be non-monotonic decreasing, not
	 * sure if double values can occur.
	 */
int atomic_sub_return(int i, atomic_t *v)
{
	int retval;
	retval = InterlockedExchangeAdd((LONG*)v, -i);
	retval -= i;
	return retval;
}

LONGLONG atomic_sub_return64(LONGLONG a, atomic_t64 *v)
{
	LONGLONG retval;
	retval = InterlockedExchangeAdd64((LONGLONG*)v, -a);
	retval -= a;
	return retval;
}

	/* TODO: this is really atomic? */

int atomic_dec_and_test(atomic_t *v)
{
	return (0 == InterlockedDecrement((LONG*)v));
}

int atomic_sub_and_test(int i, atomic_t *v)
{
	LONG_PTR retval;
	retval = InterlockedExchangeAdd((LONG*)v, -i);
	retval -= i;
	return (retval == 0);
}

int atomic_cmpxchg(atomic_t *v, int old, int new)
{
	return InterlockedCompareExchange((long *)v, new, old);
}

int atomic_xchg(atomic_t *v, int n)
{
	return InterlockedExchange((LONG*)v, n);
}

int atomic_read(const atomic_t *v)
{
	return InterlockedAnd((LONG*)v, 0xffffffff);
}

LONGLONG atomic_read64(const atomic_t64 *v)
{
	return InterlockedAnd64((LONGLONG*)v, 0xffffffffffffffff);
}

	/* TODO: we don"t need to initialize memory here, it should
	   be just the other way round (kzalloc calling kmalloc) 
	 */

	/* TODO: we would save patches to DRBD if we skip the tag
	   here .. aren't using Windows Degugger anyway at the moment..
	 */

void * kmalloc(int size, int flag, ULONG Tag)
{
	return kcalloc(size, 1, flag, Tag); // => adjust size, count parameter mismatch
}

void * kcalloc(int size, int count, int flag, ULONG Tag)
{
	return kzalloc(size * count, 0, Tag);
}

void * kzalloc(int size, int flag, ULONG Tag)
{
	void *mem;
    static int fail_count = 0;

	mem = ExAllocatePoolWithTag(NonPagedPool, size, Tag);
	if (!mem)
	{
		return NULL;
	}

	RtlZeroMemory(mem, size);
	return mem;
}

char *kstrdup(const char *s, int gfp)
{
	size_t len;
	char *buf;

	if (!s)
		return NULL;

	len = strlen(s) + 1;
	buf = kzalloc(len, gfp, 'C3DW');
	if (buf)
		memcpy(buf, s, len);
	return buf;
}

void *page_address(const struct page *page)
{
	return page->addr;
}

struct page *alloc_page(int flag)
{
	struct page *p = kmalloc(sizeof(struct page),0, 'D3DW'); 
	if (!p)	{
		WDRBD_INFO("alloc_page struct page failed\n");
		return NULL;
	}	
	RtlZeroMemory(p, sizeof(struct page));
	
		/* TODO: is this page aligned? */
	p->addr = kzalloc(PAGE_SIZE, 0, 'E3DW');
	if (!p->addr)	{
		kfree(p); 
		WDRBD_INFO("alloc_page PAGE_SIZE failed\n");
		return NULL;
	}
	RtlZeroMemory(p->addr, PAGE_SIZE);

	return p;
}

void __free_page(struct page *page)
{
	kfree(page->addr);
	kfree(page); 
}

#ifdef _HACK

static void hack_endio BIO_ENDIO_ARGS(struct bio *bio)
{
	printk("1\n");
	struct completion *c = (struct completion*) bio->bi_private;
	printk("2\n");
	complete(c);
	printk("3\n");
}

void hack_alloc_page(struct block_device *dev)
{
	printk("1\n");
	struct page *p = alloc_page(0);
	struct page *p2 = alloc_page(0);
	printk("2\n");
	struct bio *b = bio_alloc(0, 2, 'XXXX');
	int i;
	struct completion c;
	printk("3\n");
	bio_add_page(b, p, 4096, 0);
	bio_add_page(b, p2, 4096, 0);
	printk("4\n");
#ifdef _HACK_WRITE
	bio_set_op_attrs(b, REQ_OP_WRITE, 0);
#else
	bio_set_op_attrs(b, REQ_OP_READ, 0);
#endif
	printk("5\n");
	for (i=0;i<4096;i++) {
		((char*)p->addr)[i] = i;
		((char*)p2->addr)[i] = i;
	}
	printk("5a\n");
	b->bi_end_io = hack_endio;
	DRBD_BIO_BI_SECTOR(b) = 1;
	printk("6\n");
	init_completion(&c);
	printk("7\n");
	b->bi_private = &c;
	printk("7a\n");
	bio_set_dev(b, dev);
	printk("7b\n");
	submit_bio(b);
	//     hack_endio(b, 0);
	printk("8\n");
	wait_for_completion(&c);
	printk("9\n");
	bio_put(b);
	printk("9a\n");
	__free_page(p);
	printk("9b\n");
	__free_page(p2);
	printk("a\n");
	printk("karin 2\n");
}
#endif

void drbd_bp(char *msg)
{
    WDRBD_ERROR("breakpoint: msg(%s)\n", msg);
}

__inline void kfree(const void * x)
{
	if (x)
	{
		ExFreePool((void*)x);
	}
}

__inline void kvfree(const void * x)
{
	if (x)
	{
		ExFreePool((void*)x);
	}
}

// from  linux 2.6.32
int kref_put(struct kref *kref, void (*release)(struct kref *kref))
{
	WARN_ON(release == NULL);
	WARN_ON(release == (void (*)(struct kref *))kfree);

	if (atomic_dec_and_test(&kref->refcount))
	{
		release(kref);
		return 1;
	}
	return 0;
}

/* TODO: originally this is void. */
int kref_get(struct kref *kref)
{
	return atomic_inc_return(&kref->refcount) < 2;
}

void kref_init(struct kref *kref)
{
	atomic_set(&kref->refcount, 1);
}

struct request_queue *bdev_get_queue(struct block_device *bdev)
{
	if (bdev && bdev->bd_disk) {
		return bdev->bd_disk->queue;
	}

	return NULL;
}

struct bio *bio_alloc_bioset(gfp_t gfp_mask, int nr_iovecs, struct bio_set *bs)
{
	return NULL;
}

	/* TODO: Tag needed? Would save patches .. */
struct bio *bio_alloc(gfp_t gfp_mask, int nr_iovecs, ULONG Tag)
{
	struct bio *bio;

	bio = kzalloc(sizeof(struct bio) + nr_iovecs * sizeof(struct bio_vec), gfp_mask, Tag);
	if (!bio)
	{
		return 0;
	}
	bio->bi_max_vecs = nr_iovecs;
	bio->bi_cnt = 1;
	bio->bi_vcnt = 0;

	return bio;
}

static void free_mdls_and_irp(struct bio *bio)
{
	struct _MDL *mdl, *next_mdl;
	int r;

		/* This happens quite frequently when DRBD allocates a
	         * bio without ever calling generic_make_request on it.
		 */

	if (bio->bi_irps == NULL)
		return;

	for (r=0;r<bio->bi_num_requests;r++) {
		/* This has to be done before freeing the buffers with
		 * __free_page(). Else we get a PFN list corrupted (or
		 * so) BSOD.
		 */
		if (bio->bi_irps[r] == NULL)
			continue;

		for (mdl = bio->bi_irps[r]->MdlAddress;
		     mdl != NULL;
		     mdl = next_mdl) {
			next_mdl = mdl->Next;
			if (bio->bi_paged_memory && mdl->MdlFlags & MDL_PAGES_LOCKED) {
				MmUnlockPages(mdl); /* Must not do this when MmBuildMdlForNonPagedPool() is used */
			}
			IoFreeMdl(mdl); // This function will also unmap pages.
		}
		bio->bi_irps[r]->MdlAddress = NULL;
		ObDereferenceObject(bio->bi_irps[r]->Tail.Overlay.Thread);

		IoFreeIrp(bio->bi_irps[r]);
	}

	kfree(bio->bi_irps);
}

void bio_put(struct bio *bio)
{
	int cnt;
	cnt = atomic_dec(&bio->bi_cnt);
	if (cnt == 0)
		bio_free(bio);
}

void bio_free(struct bio *bio)
{
	free_mdls_and_irp(bio);
	kfree(bio);
}

struct bio *bio_clone(struct bio * bio_src, int flag)
{
    struct bio *bio = bio_alloc(flag, bio_src->bi_max_vecs, '24DW');

    if (!bio)
    {
        return NULL;
    }

	memcpy(bio->bi_io_vec, bio_src->bi_io_vec, bio_src->bi_max_vecs * sizeof(struct bio_vec));
	bio->bi_sector = bio_src->bi_sector;
	bio->bi_bdev = bio_src->bi_bdev;
	//bio->bi_flags |= 1 << BIO_CLONED;
	bio->bi_rw = bio_src->bi_rw;
	bio->bi_vcnt = bio_src->bi_vcnt;
	bio->bi_size = bio_src->bi_size;
	bio->bi_idx = bio_src->bi_idx;

	return bio;
}

int bio_add_page(struct bio *bio, struct page *page, unsigned int len,unsigned int offset)
{
	struct bio_vec *bvec = &bio->bi_io_vec[bio->bi_vcnt++];
		
	bvec->bv_page = page;
	bvec->bv_len = len;
	bvec->bv_offset = offset;
	bio->bi_size += len;
/* printk("bio: %p bv_page: %p bv_page->addr: %p bv_offset: %d bv_len: %d bi_size: %d\n", bio, bvec->bv_page, bvec->bv_page->addr, bvec->bv_offset, bvec->bv_len, bio->bi_size); */

	return len;
}

#include "drbd_int.h"

long IS_ERR_OR_NULL(const void *ptr)
{
	return !ptr || IS_ERR_VALUE((unsigned long) ptr); 
}

void *ERR_PTR(long error)
{
	return (void *) error;
}

long PTR_ERR(const void *ptr)
{
	return (long)ptr;
}

int IS_ERR(void *ptr)
{
	return IS_ERR_VALUE((unsigned long) ptr);
}

void wake_up_process(struct drbd_thread *thi)
{
    KeSetEvent(&thi->wait_event, 0, FALSE);
}

void _wake_up(wait_queue_head_t *q, char *__func, int __line)
{		
    KeSetEvent(&q->wqh_event, 0, FALSE);
}

void wake_up_all(wait_queue_head_t *q)
{
	/* Should cause all threads to wake up and check the condition again */
	/* TODO: phil check whether the single-wake-up is wrong? */
    KeSetEvent(&q->wqh_event, 0, FALSE);
}

void init_completion(struct completion *completion)
{
	memset(completion->wait.eventName, 0, Q_NAME_SZ);
	strcpy(completion->wait.eventName, "completion");
	init_waitqueue_head(&completion->wait);
}

long wait_for_completion(struct completion *completion)
{
	return schedule(&completion->wait, MAX_SCHEDULE_TIMEOUT, __FUNCTION__, __LINE__);
}

long wait_for_completion_timeout(struct completion *completion, long timeout)
{
    return schedule(&completion->wait, timeout, __FUNCTION__, __LINE__);
}

void complete(struct completion *c)
{
    KeSetEvent(&c->wait.wqh_event, 0, FALSE);
}

void complete_all(struct completion *c)
{
    KeSetEvent(&c->wait.wqh_event, 0, FALSE);
}

static  void __add_wait_queue(wait_queue_head_t *head, wait_queue_t *new)
{
	list_add(&new->task_list, &head->task_list);
}

long schedule(wait_queue_head_t *q, long timeout, char *func, int line) 
{
	LARGE_INTEGER nWaitTime;
	LARGE_INTEGER *pTime;
	unsigned long expire;

	expire = timeout + jiffies;
	nWaitTime.QuadPart = 0;

	if(timeout != MAX_SCHEDULE_TIMEOUT)
	{
		nWaitTime = RtlConvertLongToLargeInteger((timeout) * (-1 * 1000 * 10));
	}
	else
	{
		nWaitTime = RtlConvertLongToLargeInteger((60) * (-1 * 10000000));
	}
	pTime = &nWaitTime;
	if ((q == NULL) || (q == (wait_queue_head_t *)SCHED_Q_INTERRUPTIBLE))
	{
		KTIMER ktimer;
		KeInitializeTimer(&ktimer);
		KeSetTimer(&ktimer, nWaitTime, 0);
		KeWaitForSingleObject(&ktimer, Executive, KernelMode, FALSE, NULL);
	}
	else
	{
		NTSTATUS status;
		PVOID waitObjects[2] = {0};
		struct task_struct *thread = current;

		int wObjCount = 1;

		waitObjects[0] = (PVOID) &q->wqh_event;
		if (thread->has_sig_event)
		{
			waitObjects[1] = (PVOID) &thread->sig_event;
			wObjCount = 2;
		}

		while (1)
		{
			//HERf("about to wait; count %d, timeout %llu, obj 0x%p 0x%p", wObjCount, (unsigned long long)nWaitTime.QuadPart, waitObjects[0], waitObjects[1]); // too noisy
			status = KeWaitForMultipleObjects(wObjCount, &waitObjects[0], WaitAny, Executive, KernelMode, FALSE, pTime, NULL);

			switch (status) {
			case STATUS_WAIT_0:
				KeResetEvent(&q->wqh_event); // DW-105: use event and polling both.
				break;

			case STATUS_WAIT_1:
				if (thread->sig == DRBD_SIGKILL)
				{
					return -DRBD_SIGKILL;
				}
				break;

			case STATUS_TIMEOUT:
				if (timeout == MAX_SCHEDULE_TIMEOUT)
				{
					continue;
				}
				break;

			default:
				WDRBD_ERROR("DRBD_PANIC: KeWaitForMultipleObjects done! default status=0x%x\n", status);
				BUG();
				break;
			}
			break;
		}
	}

	timeout = expire - jiffies;
	return timeout < 0 ? 0 : timeout;
}

struct workqueue_struct *system_wq;

#ifdef _WIN32
int queue_work(struct workqueue_struct* queue, struct work_struct* work)
#else
void queue_work(struct workqueue_struct* queue, struct work_struct* work)
#endif
{
    struct work_struct_wrapper * wr = kmalloc(sizeof(struct work_struct_wrapper), 0, '68DW');
#ifdef _WIN32 // DW-1051	
	if(!wr) {
		return FALSE;
	}
#endif	
    wr->w = work;
    ExInterlockedInsertTailList(&queue->list_head, &wr->element, &queue->list_lock);
    KeSetEvent(&queue->wakeupEvent, 0, FALSE); // signal to run_singlethread_workqueue
#ifdef _WIN32
    return TRUE;
#endif
}
#ifdef _WIN32
void run_singlethread_workqueue(struct workqueue_struct * wq)
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    PVOID waitObjects[2] = { &wq->wakeupEvent, &wq->killEvent };
    int maxObj = 2;

    while (wq->run)
    {
        status = KeWaitForMultipleObjects(maxObj, &waitObjects[0], WaitAny, Executive, KernelMode, FALSE, NULL, NULL);
        switch (status)
        {
            case STATUS_WAIT_0:
            {
                PLIST_ENTRY entry;
                while ((entry = ExInterlockedRemoveHeadList(&wq->list_head, &wq->list_lock)) != 0)
                {
                    struct work_struct_wrapper * wr = CONTAINING_RECORD(entry, struct work_struct_wrapper, element);
                    wr->w->func(wr->w);
                    kfree(wr);
                }
                break;
            }

            case (STATUS_WAIT_1) :
                wq->run = FALSE;
                break;

            default:
                continue;
        }
    }
}

struct workqueue_struct *alloc_ordered_workqueue(const char * fmt, int flags, ...)
{
    struct workqueue_struct * wq = kzalloc(sizeof(struct workqueue_struct), 0, '31DW');
    va_list args;
    va_start(args, flags);
    NTSTATUS status;


    if (!wq)
    {
        return NULL;
    }

    KeInitializeEvent(&wq->wakeupEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&wq->killEvent, SynchronizationEvent, FALSE);
    InitializeListHead(&wq->list_head);
    KeInitializeSpinLock(&wq->list_lock);

    status = RtlStringCbVPrintfA(wq->name, sizeof(wq->name)-1, fmt, args);
    if (status != STATUS_SUCCESS) {
	WDRBD_ERROR("Can't RtlStringCbVPrintfA");
        kfree(wq);
        return NULL;
    }

    wq->run = TRUE;

    HANDLE hThread = NULL;
    status = PsCreateSystemThread(&hThread, THREAD_ALL_ACCESS, NULL, NULL, NULL, run_singlethread_workqueue, wq);
    if (!NT_SUCCESS(status))
    {
        WDRBD_ERROR("PsCreateSystemThread failed with status 0x%08X\n", status);
        kfree(wq);
        return NULL;
    }

    status = ObReferenceObjectByHandle(hThread, THREAD_ALL_ACCESS, NULL, KernelMode, &wq->pThread, NULL);
    ZwClose(hThread);
    if (!NT_SUCCESS(status))
    {
        WDRBD_ERROR("ObReferenceObjectByHandle failed with status 0x%08X\n", status);
        kfree(wq);
        return NULL;
    }

    return wq;
}
#endif

/* TODO: implement */
void flush_workqueue(struct workqueue_struct *wq)
{
	printk(KERN_INFO "flush_workqueue not implemented.\n");
}

#ifdef _WIN32_TMP_DEBUG_MUTEX
void mutex_init(struct mutex *m, char *name)
#else
void mutex_init(struct mutex *m)
#endif
{
	KeInitializeMutex(&m->mtx, 0);
#ifdef _WIN32_TMP_DEBUG_MUTEX
	memset(m->name, 0, 32);
	strcpy(m->name, name); 
#endif
}

NTSTATUS mutex_lock_timeout(struct mutex *m, ULONG msTimeout)
{
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	LARGE_INTEGER nWaitTime = { 0, };

	if (NULL == m)
	{
		return STATUS_INVALID_PARAMETER;
	}

	nWaitTime.QuadPart = (-1 * 10000);
	nWaitTime.QuadPart *= msTimeout;		// multiply timeout value separately to avoid overflow.
	status = KeWaitForMutexObject(&m->mtx, Executive, KernelMode, FALSE, &nWaitTime);

	return status;
}

__inline
NTSTATUS mutex_lock(struct mutex *m)
{
    return KeWaitForMutexObject(&m->mtx, Executive, KernelMode, FALSE, NULL);
}

__inline
int mutex_lock_interruptible(struct mutex *m)
{
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	int err = -EIO;
	struct task_struct *thread = current;
	PVOID waitObjects[2];
	int wObjCount = 1;

	waitObjects[0] = (PVOID)&m->mtx;
	if (thread->has_sig_event)
	{
		waitObjects[1] = (PVOID)&thread->sig_event;
		wObjCount++;
	}
	
	status = KeWaitForMultipleObjects(wObjCount, &waitObjects[0], WaitAny, Executive, KernelMode, FALSE, NULL, NULL);

	switch (status)
	{
	case STATUS_WAIT_0:		// mutex acquired.
		err = 0;
		break;
	case STATUS_WAIT_1:		// thread got signal by the func 'force_sig'
		err = thread->sig != 0 ? -thread->sig : -EIO;
		break;
	default:
		err = -EIO;
		WDRBD_ERROR("KeWaitForMultipleObjects returned unexpected status(0x%x)", status);
		break;
	}

	return err;
}

// Returns 1 if the mutex is locked, 0 if unlocked.
int mutex_is_locked(struct mutex *m)
{
	return (KeReadStateMutex(&m->mtx) == 0) ? 1 : 0;
}

// Try to acquire the mutex atomically. 
// Returns 1 if the mutex has been acquired successfully, and 0 on contention.
int mutex_trylock(struct mutex *m)
{
	LARGE_INTEGER Timeout;
	Timeout.QuadPart = 0; 

	if (KeWaitForMutexObject(&m->mtx, Executive, KernelMode, FALSE, &Timeout) == STATUS_SUCCESS)
	{
		return 1;
	}
	else
	{
		return 0;
	}
}

void mutex_unlock(struct mutex *m)
{
	KeReleaseMutex(&m->mtx, FALSE);
}

void sema_init(struct semaphore *s, int limit)
{
    KeInitializeSemaphore(&s->sem, limit, limit);    
    WDRBD_TRACE_SEM("KeInitializeSemaphore!  KeReadStateSemaphore (%d)\n", KeReadStateSemaphore(&s->sem));
}

void down(struct semaphore *s)
{
    WDRBD_TRACE_SEM("KeWaitForSingleObject before! KeReadStateSemaphore (%d)\n", KeReadStateSemaphore(&s->sem));
    KeWaitForSingleObject(&s->sem, Executive, KernelMode, FALSE, NULL);
    WDRBD_TRACE_SEM("KeWaitForSingleObject after! KeReadStateSemaphore (%d)\n", KeReadStateSemaphore(&s->sem));
}

/**
  * down_trylock - try to acquire the semaphore, without waiting
  * @sem: the semaphore to be acquired
  *
  * Try to acquire the semaphore atomically.  Returns 0 if the semaphore has
  * been acquired successfully or 1 if it it cannot be acquired.
  */

int down_trylock(struct semaphore *s)
{
    LARGE_INTEGER Timeout;
    Timeout.QuadPart = 0;
    
    if (KeWaitForSingleObject(&s->sem, Executive, KernelMode, FALSE, &Timeout) == STATUS_SUCCESS)
    {
        WDRBD_TRACE_SEM("success! KeReadStateSemaphore (%d)\n", KeReadStateSemaphore(&s->sem));
        return 0;
    }
    else
    {
        WDRBD_TRACE_SEM("fail! KeReadStateSemaphore (%d)\n", KeReadStateSemaphore(&s->sem));
        return 1;
    }
}

void up(struct semaphore *s)
{
    if (KeReadStateSemaphore(&s->sem) < s->sem.Limit)
    {
        WDRBD_TRACE_SEM("KeReleaseSemaphore before! KeReadStateSemaphore (%d)\n", KeReadStateSemaphore(&s->sem));
        KeReleaseSemaphore(&s->sem, IO_NO_INCREMENT, 1, FALSE);
        WDRBD_TRACE_SEM("KeReleaseSemaphore after! KeReadStateSemaphore (%d)\n", KeReadStateSemaphore(&s->sem));
    }
}

KIRQL du_OldIrql;

void downup_rwlock_init(KSPIN_LOCK* lock)
{
	KeInitializeSpinLock(lock);
}

KIRQL down_write(KSPIN_LOCK* lock)
{
	return KeAcquireSpinLock(lock, &du_OldIrql);
}

void up_write(KSPIN_LOCK* lock)
{
	KeReleaseSpinLock(lock, du_OldIrql);
	return;
}

KIRQL down_read(KSPIN_LOCK* lock)
{
	return KeAcquireSpinLock(lock, &du_OldIrql);
}

void up_read(KSPIN_LOCK* lock)
{
	KeReleaseSpinLock(lock, du_OldIrql);
	return;
}

void spin_lock_init(spinlock_t *lock)
{
	KeInitializeSpinLock(&lock->spinLock);
	lock->Refcnt = 0;
	lock->OwnerThread = 0;
}

void acquireSpinLock(KSPIN_LOCK *lock, KIRQL *flags)
{
	KeAcquireSpinLock(lock, flags);
}

void releaseSpinLock(KSPIN_LOCK *lock, KIRQL flags)
{
	KeReleaseSpinLock(lock, flags);
}

// DW-903 protect lock recursion
// if current thread equal lock owner thread, just increase refcnt

long _spin_lock_irqsave(spinlock_t *lock)
{
	KIRQL	oldIrql = 0;
	PKTHREAD curthread = KeGetCurrentThread();
	if( curthread == lock->OwnerThread) { 
		WDRBD_WARN("thread:%p spinlock recursion is happened! function:%s line:%d\n", curthread, __FUNCTION__, __LINE__);
	} else {
		acquireSpinLock(&lock->spinLock, &oldIrql);
		lock->OwnerThread = curthread;
	}
	InterlockedIncrement(&lock->Refcnt);
	return (long)oldIrql;
}

void spin_lock(spinlock_t *lock)
{
	spin_lock_irq(lock);
}

void spin_unlock(spinlock_t *lock)
{
	spin_unlock_irq(lock);
}

// DW-903 protect lock recursion
// if current thread equal lock owner thread, just increase refcnt

void spin_lock_irq(spinlock_t *lock)
{
	PKTHREAD curthread = KeGetCurrentThread();
	if( curthread == lock->OwnerThread) {//DW-903 protect lock recursion
		WDRBD_WARN("thread:%p spinlock recursion is happened! function:%s line:%d\n", curthread, __FUNCTION__, __LINE__);
	} else {
		acquireSpinLock(&lock->spinLock, &lock->saved_oldIrql);
		lock->OwnerThread = curthread;
	}
	InterlockedIncrement(&lock->Refcnt);
}

// fisrt, decrease refcnt
// If refcnt is 0, clear OwnerThread and release lock

void spin_unlock_irq(spinlock_t *lock)
{
	InterlockedDecrement(&lock->Refcnt);
	if(lock->Refcnt == 0) {
		lock->OwnerThread = 0;
		releaseSpinLock(&lock->spinLock, lock->saved_oldIrql);
	}
}
// fisrt, decrease refcnt
// If refcnt is 0, clear OwnerThread and release lock

void spin_unlock_irqrestore(spinlock_t *lock, long flags)
{
	InterlockedDecrement(&lock->Refcnt);
	if(lock->Refcnt == 0) {
		lock->OwnerThread = 0;
		releaseSpinLock(&lock->spinLock, (KIRQL) flags);
	}
}

// DW-903 protect lock recursion
// if current thread equal lock owner thread, just increase refcnt
void spin_lock_bh(spinlock_t *lock)
{
	PKTHREAD curthread = KeGetCurrentThread();
	if( curthread == lock->OwnerThread) {
		WDRBD_WARN("thread:%p spinlock recursion is happened! function:%s line:%d\n", curthread, __FUNCTION__, __LINE__);
	} else {
		KeAcquireSpinLock(&lock->spinLock, &lock->saved_oldIrql);
		lock->OwnerThread = curthread;
	}
	InterlockedIncrement(&lock->Refcnt);
}
// fisrt, decrease refcnt
// If refcnt is 0, clear OwnerThread and release lock
void spin_unlock_bh(spinlock_t *lock)
{
	InterlockedDecrement(&lock->Refcnt);
	if(lock->Refcnt == 0) {
		lock->OwnerThread = 0;
		KeReleaseSpinLock(&lock->spinLock, lock->saved_oldIrql);
	}
}

spinlock_t g_irqLock;
void local_irq_disable()
{	
	spin_lock_irq(&g_irqLock);
}

void local_irq_enable()
{
	spin_unlock_irq(&g_irqLock);
}

BOOLEAN spin_trylock(spinlock_t *lock)
{
	if (FALSE == KeTestSpinLock(&lock->spinLock))
		return FALSE;
	
	spin_lock(lock);
	return TRUE;
}

void get_random_bytes(char *buf, int nbytes)
{
    static ULONG_PTR lcg_2_64_div_pi = 0;
    static ULONG_PTR mmix_knuth = 0;
    LARGE_INTEGER li;
    ULONG_PTR rnt;
    ULONG rn;
    int length;

    if (mmix_knuth == 0) {
	    li = KeQueryPerformanceCounter(NULL);
	    mmix_knuth = li.QuadPart;
    }
    if (lcg_2_64_div_pi == 0) {
	    lcg_2_64_div_pi = mmix_knuth;
	    get_random_bytes((char*)&lcg_2_64_div_pi, sizeof(lcg_2_64_div_pi));
    }

    /* https://www.winvistatips.com/threads/verifier-exe-is-not-compatible-with-rtlrandom-ex.193117/ */
    if (KeGetCurrentIrql() < DISPATCH_LEVEL) {
	    /* RtlRandomEx only returns [0, 2^31-1], so not even 31bit. */
	    RtlRandomEx(&rn);
	    mmix_knuth += rn;
    }

    while (nbytes > 0)
    {
	    /* https://en.wikipedia.org/wiki/Linear_congruential_generator */
	    mmix_knuth = mmix_knuth * 6364136223846793005 + 1442695040888963407;
	    /* Nearest prime to 2^64/π */
	    lcg_2_64_div_pi = lcg_2_64_div_pi * 5871781006564002809 + 1;

	    /* Hide lowest bits */
	    rn = (lcg_2_64_div_pi ^ mmix_knuth) >> 11;

	    length = sizeof(rn);
	    if (length > nbytes)
		    length = nbytes;

	    memcpy(buf, (UCHAR *)&rn, length);

	    nbytes -= length;
	    buf += length;
    }
}

unsigned int crypto_tfm_alg_digestsize(struct crypto_tfm *tfm)
{
	return 4; // 4byte in constant
}

int page_count(struct page *page)
{
	return 1;
}

static void timer_callback(PKDPC dpc, struct timer_list* timer, PVOID arg1, PVOID arg2)
{
	(void)arg1;
	(void)arg2;
	(void)dpc;
	timer->function(timer->data);
}

void setup_timer(struct timer_list * timer, void(*function)(ULONG_PTR data), ULONG_PTR data)
{
	timer->function = function;
	timer->data = data;
	KeInitializeTimer(&timer->ktimer);
	KeInitializeDpc(&timer->dpc, (PKDEFERRED_ROUTINE)timer_callback, timer);
}

void add_timer(struct timer_list *t)
{
	mod_timer(t, t->expires);
}

void del_timer(struct timer_list *t)
{
	KeCancelTimer(&t->ktimer);
    t->expires = 0;
}

#ifdef _WIN32
/**
 * timer_pending - is a timer pending?
 * @timer: the timer in question
 *
 * timer_pending will tell whether a given timer is currently pending,
 * or not. Callers must ensure serialization wrt. other operations done
 * to this timer, eg. interrupt contexts, or other CPUs on SMP.
 *
 * return value: 1 if the timer is pending, 0 if not.
 */
static __inline int timer_pending(const struct timer_list * timer)
{
    return timer->ktimer.Header.Inserted;
}

int del_timer_sync(struct timer_list *t)
{
#ifdef _WIN32		
	bool pending = 0;
	pending = timer_pending(t);
	
	del_timer(t);

	return pending;
#else
	// from linux kernel 2.6.24
	for (;;) {
		int ret = try_to_del_timer_sync(timer);
		if (ret >= 0)
			return ret;
		cpu_relax();
	}
#endif
}


static int
__mod_timer(struct timer_list *timer, ULONG_PTR expires, bool pending_only)
{
    if (!timer_pending(timer) && pending_only)
    {
		return 0;
    }

    LARGE_INTEGER nWaitTime = { .QuadPart = 0 };
    ULONG_PTR current_milisec = jiffies;

    timer->expires = expires;

    if (current_milisec >= expires)
    {
		nWaitTime.QuadPart = -1;
    }
	else
	{
		expires -= current_milisec;
		nWaitTime = RtlConvertLongToLargeInteger(RELATIVE(MILLISECONDS(expires)));
	}

#ifdef DBG
    WDRBD_TRACE_TM("%s timer(0x%p) current(%d) expires(%d) gap(%d)\n",
        timer->name, timer, current_milisec, timer->expires, timer->expires - current_milisec);
#endif
    KeSetTimer(&timer->ktimer, nWaitTime, &timer->dpc);
    return 1;
}

/**
 * mod_timer_pending - modify a pending timer's timeout
 * @timer: the pending timer to be modified
 * @expires: new timeout in jiffies
 *
 * mod_timer_pending() is the same for pending timers as mod_timer(),
 * but will not re-activate and modify already deleted timers.
 *
 * It is useful for unserialized use of timers.
 */
int mod_timer_pending(struct timer_list *timer, ULONG_PTR expires)
{
    return __mod_timer(timer, expires, true);
}
#endif

int mod_timer(struct timer_list *timer, ULONG_PTR expires)
{
#ifdef _WIN32

	// DW-519 timer logic temporary patch. required fix DW-824. 
    //if (timer_pending(timer) && timer->expires == expires)
    //	return 1;

    return __mod_timer(timer, expires, false);
#else
	LARGE_INTEGER nWaitTime;

	unsigned long current_milisec = jiffies;
	nWaitTime.QuadPart = 0;

	if (current_milisec >= expires_ms)
	{
		nWaitTime.LowPart = 1;
		KeSetTimer(&t->ktimer, nWaitTime, &t->dpc);
		return 0;
	}
	expires_ms -= current_milisec;
	nWaitTime = RtlConvertLongToLargeInteger(-1 * (expires_ms) * 1000 * 10);

	KeSetTimer(&t->ktimer, nWaitTime, &t->dpc);
	return 1;
#endif
}

void kobject_put(struct kobject *kobj)
{
    if (kobj) 
    {
        if (kobj->name == NULL)
        {
            //WDRBD_WARN("%p name is null.\n", kobj);
            return;
        }

		if (atomic_sub_and_test(1, &kobj->kref.refcount))
		{
			void(*release)(struct kobject *kobj);
			release = kobj->ktype->release;
			if (release == 0)
			{
				return;
			}
			release(kobj);
		}
    }
    else
    {
        //WDRBD_WARN("kobj is null.\n");
        return;
    }
}

void kobject_del(struct kobject *kobj)
{
    if (!kobj)
    {
        WDRBD_WARN("kobj is null.\n");
        return;
    }
    kobject_put(kobj->parent); 
}

void kobject_get(struct kobject *kobj)
{
    if (kobj)
    {
        kref_get(&kobj->kref);
    }
    else
    {
        WDRBD_INFO("kobj is null.\n");
        return;
    }
}

void del_gendisk(struct gendisk *disk)
{
	// free disk
}

 void destroy_workqueue(struct workqueue_struct *wq)
{
	 KeSetEvent(&wq->killEvent, 0, FALSE);
	 KeWaitForSingleObject(wq->pThread, Executive, KernelMode, FALSE, NULL);
	 ObDereferenceObject(wq->pThread);
     kfree(wq);
}

 void sock_release(struct socket *sock)
{
	NTSTATUS status;
	
	if (!sock)
	{
		WDRBD_WARN("socket is null.\n");
		return;
	}

#ifndef _WIN32_SEND_BUFFING
	
	status = CloseSocket(sock->sk); 
	if (!NT_SUCCESS(status)) 
	{
		WDRBD_ERROR("error=0x%x\n", status);
		return;
	}
#endif

#ifdef _WIN32_SEND_BUFFING
	struct _buffering_attr *buffering_attr = &sock->buffering_attr;
	struct ring_buffer *bab = buffering_attr->bab;

	if (bab)
	{
		if (bab->static_big_buf)
		{
			kfree(bab->static_big_buf);
		}
		kfree(bab);
	}
	
	status = CloseSocket(sock->sk);
	if (!NT_SUCCESS(status)) {
		return;
	}
#endif

	kfree(sock);
}

//Linux/block/genhd.c
void set_disk_ro(struct gendisk *disk, int flag)
{

}

/* TODO: why? (was hardcoded to 40) */
/* TODO: why the ct_thread_list? */
#define CT_MAX_THREAD_LIST          1000
static LIST_HEAD(ct_thread_list);
static int ct_thread_num = 0;
static KSPIN_LOCK ct_thread_list_lock;
static KIRQL ct_oldIrql;

void ct_init_thread_list()
{
    KeInitializeSpinLock(&ct_thread_list_lock);
}

static struct task_struct *__find_thread(PKTHREAD id)
{
    struct task_struct *t;

    list_for_each_entry(struct task_struct, t, &ct_thread_list, list)
    {
        if (t->pid == id) {
            return t;
        }
    }
    return NULL;
}

static void __delete_thread(struct task_struct *t)
{
    list_del(&t->list);
    kfree(t);
    ct_thread_num--;

    // logic check
    if (ct_thread_num < 0)
    {
        WDRBD_ERROR("DRBD_PANIC:unexpected ct_thread_num(%d)\n", ct_thread_num);
        BUG();
    }
}

struct task_struct * ct_add_thread(PKTHREAD id, const char *name, BOOLEAN event, ULONG Tag)
{
    struct task_struct *t;

    if ((t = kzalloc(sizeof(*t), GFP_KERNEL, Tag)) == NULL)
    {
        return NULL;
    }

    t->pid = id;
    if (event)
    {
        KeInitializeEvent(&t->sig_event, SynchronizationEvent, FALSE);
        t->has_sig_event = TRUE;
    }
    strcpy(t->comm, name);
    KeAcquireSpinLock(&ct_thread_list_lock, &ct_oldIrql);
	list_add(&t->list, &ct_thread_list);
	if (++ct_thread_num > CT_MAX_THREAD_LIST) {
        WDRBD_WARN("ct_thread too big(%d)\n", ct_thread_num);
    }
    KeReleaseSpinLock(&ct_thread_list_lock, ct_oldIrql);
    return t;
}

void ct_delete_thread(PKTHREAD id)
{
    KeAcquireSpinLock(&ct_thread_list_lock, &ct_oldIrql);
    __delete_thread(__find_thread(id));
    KeReleaseSpinLock(&ct_thread_list_lock, ct_oldIrql);
}

struct task_struct* ct_find_thread(PKTHREAD id)
{
    struct task_struct *t;
    KeAcquireSpinLock(&ct_thread_list_lock, &ct_oldIrql);
    t = __find_thread(id);
    if (!t)
    {
        static struct task_struct g_dummy_current;
        t = &g_dummy_current;
        t->pid = 0;
        t->has_sig_event = FALSE;
        strcpy(t->comm, "not_drbd_thread");
    }
    KeReleaseSpinLock(&ct_thread_list_lock, ct_oldIrql);
    return t;
}

int signal_pending(struct task_struct *task)
{
    if (task->has_sig_event)
	{
		if (task->sig || KeReadStateEvent(&task->sig_event))
		{
			return 1;
		}
	}
	return 0;
}

void force_sig(int sig, struct task_struct  *task)
{
    if (task->has_sig_event)
	{
		task->sig = sig;
		KeSetEvent(&task->sig_event, 0, FALSE);
	}
}

void flush_signals(struct task_struct *task)
{
    if (task->has_sig_event)
	{
		KeClearEvent(&task->sig_event); 
		task->sig = 0;
	}
}

/* https://msdn.microsoft.com/de-de/library/ff548354(v=vs.85).aspx */
IO_COMPLETION_ROUTINE DrbdIoCompletion;

static inline blk_status_t win_status_to_blk_status(NTSTATUS status)
{
	return (status == STATUS_SUCCESS) ? 0 : BLK_STS_IOERR; 
}

	/* Patch boot sector contained in buffer.
	 * When to_fs is nonzero, patch from DRBD to filesystem (such as
	 * NTFS) done typically in a read request, else the other way
	 * around. 
	 * When test_mode is non-zero do not touch the buffer. Just
	 * return non-zero when signature is found. To check for
	 * Windows filesystems use patch_boot_sector(buf, 0, 1).
	 *
	 * Returns non-zero if signature is found, else zero.
	 */

static int patch_boot_sector(char *buffer, int to_fs, int test_mode)
{
	static const char *fs_signatures[][2] = {
		{ "NTFS", "DRBD" },
		{ "MSDOS5.0", "FATDRBD" },
		{ "EXFAT", "EDRBD" },
		{ NULL, NULL }};
	int fs;
	int i;

	for (fs=0; fs_signatures[fs][0] != NULL; fs++) {
		for (i=0;fs_signatures[fs][to_fs][i] != '\0';i++) {
			if (buffer[3+i] != fs_signatures[fs][to_fs][i])
				break;
		}
		if (fs_signatures[fs][to_fs][i] == '\0') {
			if (!test_mode) {
				printk("Patching boot sector from %s to %s\n", fs_signatures[fs][to_fs], fs_signatures[fs][!to_fs]);
				for (i=0;fs_signatures[fs][to_fs][i] != '\0';i++) {
					buffer[3+i] = fs_signatures[fs][!to_fs][i];
				}
			} else {
				printk("File system signature %s found in boot sector\n", fs_signatures[fs][to_fs]);
			}
			return 1;
		}
	}
	return 0;
}

static int is_filesystem(char *buf)
{
	return patch_boot_sector(buf, 0, 1);
}

NTSTATUS DrbdIoCompletion(
  _In_     PDEVICE_OBJECT DeviceObject,
  _In_     PIRP           Irp,
  _In_opt_ PVOID          Context
)
{
/* TODO: Device object is NULL here. Fix that in case we need it one day. */
/* printk(KERN_INFO "DrbdIoCompletion: DeviceObject: %p, Irp: %p, Context: %p\n", DeviceObject, Irp, Context); */
	struct bio *bio = Context;
	PMDL mdl, nextMdl;
	struct _IO_STACK_LOCATION *stack_location = IoGetNextIrpStackLocation (Irp);
	int i;
	NTSTATUS status = Irp->IoStatus.Status;

	if (status != STATUS_SUCCESS) {
		if (status == STATUS_INVALID_DEVICE_REQUEST && stack_location->MajorFunction == IRP_MJ_FLUSH_BUFFERS)
			status = STATUS_SUCCESS;
	}

	if (status != STATUS_SUCCESS) {
		printk(KERN_WARNING "DrbdIoCompletion: I/O failed with error %x\n", Irp->IoStatus.Status);
	}
	if (stack_location->MajorFunction == IRP_MJ_READ && bio->bi_sector == 0 && bio->bi_size >= 512 && bio->bi_first_element == 0 && !bio->dont_patch_boot_sector) {
		void *buffer = bio->bi_io_vec[0].bv_page->addr; 
		patch_boot_sector(buffer, 1, 0);
	}
/*
	if (stack_location->MajorFunction == IRP_MJ_READ) {
		for (i=0;i<bio->bi_vcnt;i++) {
			printk("i: %d bv_len: %d data: %x\n", i, bio->bi_io_vec[i].bv_len, *((int*)bio->bi_io_vec[i].bv_page->addr));
		}
	}
*/

/* TODO: if failed call drbd_bio_endio always */
	int num_completed = atomic_inc_return(&bio->bi_requests_completed);
	if (num_completed == bio->bi_num_requests)
		drbd_bio_endio(bio, win_status_to_blk_status(status));

	bio_put(bio);

		/* Tell IO manager that it should not touch the
		 * irp. It has yet to be freed together with the
		 * bio.
		 */

	return STATUS_MORE_PROCESSING_REQUIRED;
}

static LONGLONG windrbd_get_volsize(struct block_device *dev)
{
	NTSTATUS status;
	KEVENT event;
	struct _IRP *newIrp;
	struct _IO_STACK_LOCATION *s;
	ULONGLONG ret;

	mutex_lock(&dev->vol_size_mutex);

	memset(&dev->vol_size_length_information, 0, sizeof(dev->vol_size_length_information));

	if (KeGetCurrentIrql() > APC_LEVEL) {
		WDRBD_ERROR("cannot run IoBuildDeviceIoControlRequest becauseof IRP(%d)\n", KeGetCurrentIrql());
		mutex_unlock(&dev->vol_size_mutex);

		return -1;
	}

	KeInitializeEvent(&event, NotificationEvent, FALSE);
	newIrp = IoBuildDeviceIoControlRequest(IOCTL_DISK_GET_LENGTH_INFO,
       		dev->windows_device, NULL, 0,
		&dev->vol_size_length_information, sizeof(dev->vol_size_length_information), 
		FALSE, &event, &dev->vol_size_io_status);

	if (!newIrp) {
		WDRBD_ERROR("cannot alloc new IRP\n");
		mutex_unlock(&dev->vol_size_mutex);

		return -1;
	}	
	s = IoGetNextIrpStackLocation(newIrp);

	s->DeviceObject = dev->windows_device;
	s->FileObject = dev->file_object;

	status = IoCallDriver(dev->windows_device, newIrp);
	if (status == STATUS_PENDING) {
		KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, (PLARGE_INTEGER)NULL);
		status = dev->vol_size_io_status.Status;
	}
	if (!NT_SUCCESS(status)) {
	        WDRBD_ERROR("cannot get volume information, err=0x%x\n", status);
		mutex_unlock(&dev->vol_size_mutex);
		return -1;
	}

	ret = dev->vol_size_length_information.Length.QuadPart;
	mutex_unlock(&dev->vol_size_mutex);

	return ret;
}

static int make_flush_request(struct bio *bio)
{
	NTSTATUS status;
	PIO_STACK_LOCATION next_stack_location;

	bio->bi_irps[bio->bi_this_request] = IoBuildAsynchronousFsdRequest(
				IRP_MJ_FLUSH_BUFFERS,
				bio->bi_bdev->windows_device,
				NULL,
				0,
				NULL,
				&bio->io_stat
				);

	if (bio->bi_irps[bio->bi_this_request] == NULL) {
		printk(KERN_ERR "Cannot build IRP.\n");
		return -EIO;
	}

	IoSetCompletionRoutine(bio->bi_irps[bio->bi_this_request], DrbdIoCompletion, bio, TRUE, TRUE, TRUE);

	status = ObReferenceObjectByPointer(bio->bi_irps[bio->bi_this_request]->Tail.Overlay.Thread, THREAD_ALL_ACCESS, NULL, KernelMode);
	if (!NT_SUCCESS(status)) {
		WDRBD_WARN("ObReferenceObjectByPointer failed with status %x\n", status);
		return -EIO;
	}

	next_stack_location = IoGetNextIrpStackLocation (bio->bi_irps[bio->bi_this_request]);

	next_stack_location->DeviceObject = bio->bi_bdev->windows_device;
	next_stack_location->FileObject = bio->bi_bdev->file_object;

// printk("next_stack_location->MajorFunction: %x next_stack_location->MinorFunction: %x\n", next_stack_location->MajorFunction, next_stack_location->MinorFunction);

// printk("bio: %p bio->bi_bdev: %p bio->bi_bdev->windows_device: %p\n", bio, bio->bi_bdev, bio->bi_bdev->windows_device);

	bio_get(bio);
	status = IoCallDriver(bio->bi_bdev->windows_device, bio->bi_irps[bio->bi_this_request]);

	if (status != STATUS_SUCCESS && status != STATUS_PENDING) {
		if (status == STATUS_INVALID_DEVICE_REQUEST) {
			printk(KERN_INFO "Flush not supported by windows device, ignored\n");
			return 0;
		}
		printk(KERN_WARNING "flush request failed with status %x\n", status);
		return EIO;	/* Positive value means do not call endio function */
	}

	return 0;
}

static int windrbd_generic_make_request(struct bio *bio)
{
	NTSTATUS status;

	void *buffer;
	ULONG io = 0;
	PIO_STACK_LOCATION next_stack_location;
	struct _MDL *mdl, *nextMdl;
	int i;
	int err = -EIO;
	unsigned int first_size;
	
	if (bio->bi_vcnt == 0) {
		printk(KERN_ERR "Warning: bio->bi_vcnt == 0\n");
		return -EIO;
	}
	if (bio->bi_rw & WRITE) {
		io = IRP_MJ_WRITE;
	} else {
		io = IRP_MJ_READ;
	}

	bio->offset.QuadPart = bio->bi_sector << 9;
	buffer = (void*) (((char*) bio->bi_io_vec[bio->bi_first_element].bv_page->addr) + bio->bi_io_vec[bio->bi_first_element].bv_offset); 
	first_size = bio->bi_io_vec[bio->bi_first_element].bv_len;

// if (bio->bi_io_vec[0].bv_offset != 0) {
// printk("karin (%s)Local I/O(%s): offset=0x%llx sect=0x%llx total sz=%d IRQL=%d buf=0x%p bi_vcnt: %d bv_offset=%d first_size=%d\n", current->comm, (io == IRP_MJ_READ) ? "READ" : "WRITE", bio->offset.QuadPart, bio->offset.QuadPart / 512, bio->bi_size, KeGetCurrentIrql(), buffer, bio->bi_vcnt, bio->bi_io_vec[0].bv_offset, first_size);
// }


	if (io == IRP_MJ_WRITE && bio->bi_sector == 0 && bio->bi_size >= 512 && bio->bi_first_element == 0 && !bio->dont_patch_boot_sector) {
		patch_boot_sector(buffer, 0, 0);
	}

	bio->bi_irps[bio->bi_this_request] = IoBuildAsynchronousFsdRequest(
				io,
				bio->bi_bdev->windows_device,
				buffer,
				first_size,
				&bio->offset,
				&bio->io_stat
				);

	if (!bio->bi_irps[bio->bi_this_request]) {
		WDRBD_ERROR("IoBuildAsynchronousFsdRequest: cannot alloc new IRP\n");
		return -ENOMEM;
	}

		/* Unlock the MDLs pages locked by
		 * IoBuildAsynchronousFsdRequest, we must not have
		 * pages locked while using MmBuildMdlForNonPagedPool()
		 * (which is used for pages from NONPAGED pool (which
		 * is what we have).
		 * Update: if there is an NTFS on the backing device,
		 * MmBuildMdlForNonPagedPool() blue screens.
		 */

	if (!bio->bi_paged_memory) {
		struct _MDL *first_mdl;
		first_mdl = bio->bi_irps[bio->bi_this_request]->MdlAddress;
		if (first_mdl != NULL) {
			if (first_mdl->MdlFlags & MDL_PAGES_LOCKED) {
				MmUnlockPages(first_mdl);
			}
			if (!bio->bi_might_access_filesystem)
				MmBuildMdlForNonPagedPool(first_mdl);

			/* Else do nothing. Memory cannot be freed, so
			 * use static memory for the file system test.
			 */

		}
	}
		/* Else leave it locked */

	/* Windows tries to split up MDLs and crashes when
	 * there are more than 32*4K MDLs.
	 */

#define MAX_MDL_ELEMENTS 32

		/* TODO: use bio->bi_size it should be correct now. */

	int total_size = first_size;

	for (i=bio->bi_first_element+1;i<bio->bi_last_element;i++) {
		struct bio_vec *entry = &bio->bi_io_vec[i];
		struct _MDL *mdl = IoAllocateMdl(((char*)entry->bv_page->addr)+entry->bv_offset, entry->bv_len, TRUE, FALSE, bio->bi_irps[bio->bi_this_request]);

		if (mdl == NULL) {
			printk("Could not allocate mdl, giving up.\n");
			err = -ENOMEM;
				/* TODO: will also dereference thread */
			goto out_free_irp;
		}
		total_size += entry->bv_len;

		if (bio->bi_paged_memory)
			MmProbeAndLockPages(mdl, KernelMode, IoWriteAccess);
		else
			if (!bio->bi_might_access_filesystem)
				MmBuildMdlForNonPagedPool(mdl);
	}

	IoSetCompletionRoutine(bio->bi_irps[bio->bi_this_request], DrbdIoCompletion, bio, TRUE, TRUE, TRUE);

	next_stack_location = IoGetNextIrpStackLocation (bio->bi_irps[bio->bi_this_request]);

	next_stack_location->DeviceObject = bio->bi_bdev->windows_device;
	next_stack_location->FileObject = bio->bi_bdev->file_object;

	if (io == IRP_MJ_WRITE) {
		next_stack_location->Parameters.Write.Length = total_size;
	}
	if (io == IRP_MJ_READ) {
		next_stack_location->Parameters.Read.Length = total_size;
	}

		/* Take a reference to this thread, it is referenced
		 * in the IRP.
		 */

	status = ObReferenceObjectByPointer(bio->bi_irps[bio->bi_this_request]->Tail.Overlay.Thread, THREAD_ALL_ACCESS, NULL, KernelMode);
	if (!NT_SUCCESS(status)) {
		WDRBD_WARN("ObReferenceObjectByPointer failed with status %x\n", status);
		goto out_free_irp;
	}
	bio_get(bio);	/* To be put in completion routine */
	status = IoCallDriver(bio->bi_bdev->windows_device, bio->bi_irps[bio->bi_this_request]);

		/* either STATUS_SUCCESS or STATUS_PENDING */
		/* Update: may also return STATUS_ACCESS_DENIED */

	if (status != STATUS_SUCCESS && status != STATUS_PENDING) {
		printk("IoCallDriver status %x, I/O on backing device failed, bio: %p\n", status, bio);
		return EIO; /* positive value indicating that irp is already
			     * completed and bio is already freed (bio_endio
			     * must not be called).
			     */
	}
	return 0;

out_free_irp:
	free_mdls_and_irp(bio);

	return err;
}

	/* This just ensures that DRBD gets I/O errors in case something
	 * in processing the request before submitting it to the lower
	 * level driver goes wrong. It also splits the I/O requests
	 * into smaller pieces of maximum 32 vector elements. Windows
	 * block drivers cannot handle more than that.
	 */

int generic_make_request(struct bio *bio)
{
	int ret;
	int sector;
	int orig_sector;
	int total_size;
	int orig_size;
	int e;
	int flush_request;

	bio_get(bio);

// printk(KERN_INFO "bio: %p bio->bi_rw: %x bio->bi_size: %d bio->bi_vcnt: %d\n", bio, bio->bi_rw, bio->bi_size, bio->bi_vcnt);

	flush_request = (bio->bi_rw & DRBD_REQ_PREFLUSH) != 0;

	if (bio->bi_vcnt == 0)
		bio->bi_num_requests = flush_request;
	else
		bio->bi_num_requests = (bio->bi_vcnt-1)/MAX_MDL_ELEMENTS + 1 + flush_request;

	if (bio->bi_num_requests == 0) {
		drbd_bio_endio(bio, 0);
		bio_put(bio);
		return 0;
	}

		/* In case we fail early, bi_irps[n].MdlAddress must be
		 * NULL.
		 */
	bio->bi_irps = kzalloc(sizeof(*bio->bi_irps)*bio->bi_num_requests, 0, 'XXXX');
	if (bio->bi_irps == NULL) {
		drbd_bio_endio(bio, BLK_STS_IOERR);
		bio_put(bio);
		return -ENOMEM;
	}
	atomic_set(&bio->bi_requests_completed, 0);

	orig_sector = sector = bio->bi_sector;
	orig_size = bio->bi_size;

	ret = 0;

	for (bio->bi_this_request=0; 
             bio->bi_this_request<(bio->bi_num_requests - flush_request); 
             bio->bi_this_request++) {
		bio->bi_first_element = bio->bi_this_request*MAX_MDL_ELEMENTS;
		bio->bi_last_element = (bio->bi_this_request+1)*MAX_MDL_ELEMENTS;
		if (bio->bi_vcnt < bio->bi_last_element)
			bio->bi_last_element = bio->bi_vcnt;

		total_size = 0;
		for (e = bio->bi_first_element; e < bio->bi_last_element; e++)
			total_size += bio->bi_io_vec[e].bv_len;

		bio->bi_sector = sector;
		bio->bi_size = total_size;

		ret = windrbd_generic_make_request(bio);
		if (ret < 0) {
			drbd_bio_endio(bio, BLK_STS_IOERR);
			goto out;
		}
		sector += total_size >> 9;
	}
	if (flush_request) {
		ret = make_flush_request(bio);
		if (ret < 0)
			drbd_bio_endio(bio, BLK_STS_IOERR);
	}

	if (ret > 0)
		ret = -ret;

out:
	bio->bi_sector = orig_sector;
	bio->bi_size = orig_size;

	bio_put(bio);

	return ret;
}

void bio_endio(struct bio *bio, int error)
{
	if (bio->bi_end_io != NULL) {
		if (error != 0)
			WDRBD_INFO("thread(%s) bio_endio error with err=%d.\n", current->comm, error);

		bio->bi_end_io(bio, error);
	} else
		WDRBD_WARN("thread(%s) bio(%p) no bi_end_io function.\n", current->comm, bio);
}

void __list_del_entry(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
}

void list_del_init(struct list_head *entry)
{
	__list_del_entry(entry);
	INIT_LIST_HEAD(entry);
}

int hlist_unhashed(const struct hlist_node *h)
{
	return !h->pprev;
}

void __hlist_del(struct hlist_node *n)
{
	struct hlist_node *next = n->next;
	struct hlist_node **pprev = n->pprev;
	*pprev = next;
	if (next)
		next->pprev = pprev;
}

void INIT_HLIST_NODE(struct hlist_node *h)
{
    h->next = NULL;
    h->pprev = NULL;
}

void hlist_del_init(struct hlist_node *n)
{
	if (!hlist_unhashed(n)) {
		__hlist_del(n);
		INIT_HLIST_NODE(n);
	}
}

void hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{
	struct hlist_node *first = h->first;
	n->next = first;
	if (first)
		first->pprev = &n->next;
	h->first = n;
	n->pprev = &h->first;
}

static const u32 crc32c_table[256] = { 
	0x00000000L, 0xF26B8303L, 0xE13B70F7L, 0x1350F3F4L,
	0xC79A971FL, 0x35F1141CL, 0x26A1E7E8L, 0xD4CA64EBL,
	0x8AD958CFL, 0x78B2DBCCL, 0x6BE22838L, 0x9989AB3BL,
	0x4D43CFD0L, 0xBF284CD3L, 0xAC78BF27L, 0x5E133C24L,
	0x105EC76FL, 0xE235446CL, 0xF165B798L, 0x030E349BL,
	0xD7C45070L, 0x25AFD373L, 0x36FF2087L, 0xC494A384L,
	0x9A879FA0L, 0x68EC1CA3L, 0x7BBCEF57L, 0x89D76C54L,
	0x5D1D08BFL, 0xAF768BBCL, 0xBC267848L, 0x4E4DFB4BL,
	0x20BD8EDEL, 0xD2D60DDDL, 0xC186FE29L, 0x33ED7D2AL,
	0xE72719C1L, 0x154C9AC2L, 0x061C6936L, 0xF477EA35L,
	0xAA64D611L, 0x580F5512L, 0x4B5FA6E6L, 0xB93425E5L,
	0x6DFE410EL, 0x9F95C20DL, 0x8CC531F9L, 0x7EAEB2FAL,
	0x30E349B1L, 0xC288CAB2L, 0xD1D83946L, 0x23B3BA45L,
	0xF779DEAEL, 0x05125DADL, 0x1642AE59L, 0xE4292D5AL,
	0xBA3A117EL, 0x4851927DL, 0x5B016189L, 0xA96AE28AL,
	0x7DA08661L, 0x8FCB0562L, 0x9C9BF696L, 0x6EF07595L,
	0x417B1DBCL, 0xB3109EBFL, 0xA0406D4BL, 0x522BEE48L,
	0x86E18AA3L, 0x748A09A0L, 0x67DAFA54L, 0x95B17957L,
	0xCBA24573L, 0x39C9C670L, 0x2A993584L, 0xD8F2B687L,
	0x0C38D26CL, 0xFE53516FL, 0xED03A29BL, 0x1F682198L,
	0x5125DAD3L, 0xA34E59D0L, 0xB01EAA24L, 0x42752927L,
	0x96BF4DCCL, 0x64D4CECFL, 0x77843D3BL, 0x85EFBE38L,
	0xDBFC821CL, 0x2997011FL, 0x3AC7F2EBL, 0xC8AC71E8L,
	0x1C661503L, 0xEE0D9600L, 0xFD5D65F4L, 0x0F36E6F7L,
	0x61C69362L, 0x93AD1061L, 0x80FDE395L, 0x72966096L,
	0xA65C047DL, 0x5437877EL, 0x4767748AL, 0xB50CF789L,
	0xEB1FCBADL, 0x197448AEL, 0x0A24BB5AL, 0xF84F3859L,
	0x2C855CB2L, 0xDEEEDFB1L, 0xCDBE2C45L, 0x3FD5AF46L,
	0x7198540DL, 0x83F3D70EL, 0x90A324FAL, 0x62C8A7F9L,
	0xB602C312L, 0x44694011L, 0x5739B3E5L, 0xA55230E6L,
	0xFB410CC2L, 0x092A8FC1L, 0x1A7A7C35L, 0xE811FF36L,
	0x3CDB9BDDL, 0xCEB018DEL, 0xDDE0EB2AL, 0x2F8B6829L,
	0x82F63B78L, 0x709DB87BL, 0x63CD4B8FL, 0x91A6C88CL,
	0x456CAC67L, 0xB7072F64L, 0xA457DC90L, 0x563C5F93L,
	0x082F63B7L, 0xFA44E0B4L, 0xE9141340L, 0x1B7F9043L,
	0xCFB5F4A8L, 0x3DDE77ABL, 0x2E8E845FL, 0xDCE5075CL,
	0x92A8FC17L, 0x60C37F14L, 0x73938CE0L, 0x81F80FE3L,
	0x55326B08L, 0xA759E80BL, 0xB4091BFFL, 0x466298FCL,
	0x1871A4D8L, 0xEA1A27DBL, 0xF94AD42FL, 0x0B21572CL,
	0xDFEB33C7L, 0x2D80B0C4L, 0x3ED04330L, 0xCCBBC033L,
	0xA24BB5A6L, 0x502036A5L, 0x4370C551L, 0xB11B4652L,
	0x65D122B9L, 0x97BAA1BAL, 0x84EA524EL, 0x7681D14DL,
	0x2892ED69L, 0xDAF96E6AL, 0xC9A99D9EL, 0x3BC21E9DL,
	0xEF087A76L, 0x1D63F975L, 0x0E330A81L, 0xFC588982L,
	0xB21572C9L, 0x407EF1CAL, 0x532E023EL, 0xA145813DL,
	0x758FE5D6L, 0x87E466D5L, 0x94B49521L, 0x66DF1622L,
	0x38CC2A06L, 0xCAA7A905L, 0xD9F75AF1L, 0x2B9CD9F2L,
	0xFF56BD19L, 0x0D3D3E1AL, 0x1E6DCDEEL, 0xEC064EEDL,
	0xC38D26C4L, 0x31E6A5C7L, 0x22B65633L, 0xD0DDD530L,
	0x0417B1DBL, 0xF67C32D8L, 0xE52CC12CL, 0x1747422FL,
	0x49547E0BL, 0xBB3FFD08L, 0xA86F0EFCL, 0x5A048DFFL,
	0x8ECEE914L, 0x7CA56A17L, 0x6FF599E3L, 0x9D9E1AE0L,
	0xD3D3E1ABL, 0x21B862A8L, 0x32E8915CL, 0xC083125FL,
	0x144976B4L, 0xE622F5B7L, 0xF5720643L, 0x07198540L,
	0x590AB964L, 0xAB613A67L, 0xB831C993L, 0x4A5A4A90L,
	0x9E902E7BL, 0x6CFBAD78L, 0x7FAB5E8CL, 0x8DC0DD8FL,
	0xE330A81AL, 0x115B2B19L, 0x020BD8EDL, 0xF0605BEEL,
	0x24AA3F05L, 0xD6C1BC06L, 0xC5914FF2L, 0x37FACCF1L,
	0x69E9F0D5L, 0x9B8273D6L, 0x88D28022L, 0x7AB90321L,
	0xAE7367CAL, 0x5C18E4C9L, 0x4F48173DL, 0xBD23943EL,
	0xF36E6F75L, 0x0105EC76L, 0x12551F82L, 0xE03E9C81L,
	0x34F4F86AL, 0xC69F7B69L, 0xD5CF889DL, 0x27A40B9EL,
	0x79B737BAL, 0x8BDCB4B9L, 0x988C474DL, 0x6AE7C44EL,
	0xBE2DA0A5L, 0x4C4623A6L, 0x5F16D052L, 0xAD7D5351L
};

uint32_t crc32c(uint32_t crc, const uint8_t *data, unsigned int length)
{
	while (length--)
		crc = crc32c_table[(crc ^ *data++) & 0xFFL] ^ (crc >> 8);

	return crc;
}

inline void __list_add_rcu(struct list_head *new, struct list_head *prev, struct list_head *next)
{
	new->next = next;
	new->prev = prev;
	rcu_assign_pointer(list_next_rcu(prev), new);
	next->prev = new;
}

void list_del_rcu(struct list_head *entry)
{
     __list_del(entry->prev, entry->next);
     entry->prev = LIST_POISON2;
}

void list_add_rcu(struct list_head *new, struct list_head *head)
{
    __list_add_rcu(new, head, head->next);
}

void list_add_tail_rcu(struct list_head *new, struct list_head *head)
{
     __list_add_rcu(new, head->prev, head);
}

 struct request_queue *blk_alloc_queue(gfp_t gfp_mask)
 {
     return kzalloc(sizeof(struct request_queue), 0, 'E5DW');
 }

void blk_cleanup_queue(struct request_queue *q)
{
	kfree(q);
}

struct gendisk *alloc_disk(int minors)
{	
	struct gendisk *p = kzalloc(sizeof(struct gendisk), 0, '44DW');
	return p;
}

void put_disk(struct gendisk *disk)
{
	kfree(disk);
}

void blk_queue_make_request(struct request_queue *q, make_request_fn *mfn)
{
	// not support
}

void blk_queue_flush(struct request_queue *q, unsigned int flush)
{
}

struct bio_set *bioset_create(unsigned int pool_size, unsigned int front_pad)
{
	// not support
	return NULL;
}

void bioset_free(struct bio_set *bs)
{
    /* Will never be called, as bioset_create() above isn't implemented. */
    (void)bs;
}


//
// porting netlink interface 
//
unsigned char *skb_put(struct sk_buff *skb, unsigned int len)
{
	unsigned char *tmp = skb_tail_pointer(skb);
	// SKB_LINEAR_ASSERT(skb);
	skb->tail += len;
	skb->len  += len;

	if (skb->tail > skb->end)
	{
#ifndef _WIN32
		// skb_over_panic(skb, len, __builtin_return_address(0));
#else
		WDRBD_ERROR("drbd:skb_put: skb_over_panic\n");
#endif
	}

	return tmp;
}

void *compat_genlmsg_put(struct sk_buff *skb, u32 pid, u32 seq,
				       struct genl_family *family, int flags, u8 cmd)
{
	struct nlmsghdr *nlh;
	struct genlmsghdr *hdr;

	nlh = nlmsg_put(skb, pid, seq, family->id, GENL_HDRLEN + family->hdrsize, flags);
	if (nlh == NULL)
		return NULL;

	hdr = nlmsg_data(nlh);
	hdr->cmd = cmd;
	hdr->version = family->version;
	hdr->reserved = 0;

	return (char *) hdr + GENL_HDRLEN;
}

void *genlmsg_put_reply(struct sk_buff *skb,
                         struct genl_info *info,
                         struct genl_family *family,
                         int flags, u8 cmd)
{
#ifndef _WIN32
	return genlmsg_put(skb, info->snd_pid, info->snd_seq, family, flags, cmd);
#else
	return genlmsg_put(skb, info->snd_portid, info->snd_seq, family, flags, cmd);
#endif
}

void genlmsg_cancel(struct sk_buff *skb, void *hdr)
{

}

#ifdef _WIN32 
int _DRBD_ratelimit(struct ratelimit_state *rs, const char * func, const char * __FILE, const int __LINE)
{
	int ret;
	
	if (!rs ||
		!rs->interval)
		return 1;

	if (KeGetCurrentIrql() > DISPATCH_LEVEL)
	{
		return 1;
	}

	//If we contend on this state's lock then almost by definition we are too busy to print a message, in addition to the one that will be printed by the entity that is holding the lock already
	if (!spin_trylock(&rs->lock))
		return 0;

	if (!rs->begin)
		rs->begin = jiffies;

	if (time_is_before_jiffies(rs->begin + rs->interval)){
		if (rs->missed)
			WDRBD_WARN("%s(%s@%d): %d callbacks suppressed\n", func, __FILE, __LINE, rs->missed);
		rs->begin = jiffies;
		rs->printed = 0;
		rs->missed = 0;
	}

	if (rs->burst && rs->burst > rs->printed){
		rs->printed++;
		ret = 1;
	} else {
		rs->missed++;
		ret = 0;
	}
	spin_unlock(&rs->lock);

	return ret;
}
#else
int _DRBD_ratelimit(size_t ratelimit_jiffies, size_t ratelimit_burst, struct drbd_conf *mdev, char * __FILE, int __LINE)
{ 
	int __ret;						
	static size_t toks = 0x80000000UL;
	static size_t last_msg; 
	static int missed;			
	size_t now = jiffies;
	toks += now - last_msg;					
	last_msg = now;						
	if (toks > (ratelimit_burst * ratelimit_jiffies))	
		toks = ratelimit_burst * ratelimit_jiffies;	
	if (toks >= ratelimit_jiffies) {

		int lost = missed;				
		missed = 0;					
		toks -= ratelimit_jiffies;			
		if (lost)					
			dev_warn(mdev, "%d messages suppressed in %s:%d.\n", lost, __FILE, __LINE);	
		__ret = 1;					
	}
	else {
		missed++;					
		__ret = 0;					
	}							
	return __ret;							
}
#endif

#ifdef _WIN32
 // disable.
#else
bool _expect(long exp, struct drbd_conf *mdev, char *file, int line)
{
	if (!exp)
	{
		WDRBD_ERROR("minor(%d) ASSERTION FAILED in file:%s line:%d\n", mdev->minor, file, line);
        BUG();
	}
	return exp;
}
#endif
static int idr_max(int layers)
{
	int bits = min_t(int, layers * IDR_BITS, MAX_IDR_SHIFT);
	return (1 << bits) - 1;
}

#ifndef _WIN32
#define __round_mask(x, y) ((__typeof__(x))((y) - 1))
#else
#define __round_mask(x, y) ((y) - 1)
#endif
#define round_up(x, y) ((((x) - 1) | __round_mask(x, y)) + 1)

void *idr_get_next(struct idr *idp, int *nextidp)
{
	struct idr_layer *p, *pa[MAX_IDR_LEVEL + 1];
	struct idr_layer **paa = &pa[0];
	int id = *nextidp;
	int n, max;

	/* find first ent */
	if (!idp)
		return NULL;

	n = idp->layers * IDR_BITS;
	max = 1 << n;
	p = rcu_dereference_raw(idp->top);
	if (!p)
		return NULL;

	while (id < max) {
		while (n > 0 && p) {
			n -= IDR_BITS;
			*paa++ = p;
			p = rcu_dereference_raw(p->ary[(id >> n) & IDR_MASK]);
		}

		if (p) {
			*nextidp = id;
			return p;
		}

		id += 1 << n;
		while (n < fls(id)) {
			n += IDR_BITS;
			p = *--paa;
		}
	}
	return NULL;
}


// DW-1105: refresh all volumes and handle changes.
/* TODO: ?? */
void adjust_changes_to_volume(PVOID pParam)
{
//	refresh_targetdev_list();
}

/* TODO: used? */

// DW-1105: request mount manager to notify us whenever there is a change in the mount manager's persistent symbolic link name database.
void monitor_mnt_change(PVOID pParam)
{
	OBJECT_ATTRIBUTES oaMntMgr = { 0, };
	UNICODE_STRING usMntMgr = { 0, };
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	HANDLE hMntMgr = NULL;
	HANDLE hEvent = NULL;
	IO_STATUS_BLOCK iosb = { 0, };
	
	RtlInitUnicodeString(&usMntMgr, MOUNTMGR_DEVICE_NAME);
	InitializeObjectAttributes(&oaMntMgr, &usMntMgr, OBJ_CASE_INSENSITIVE, NULL, NULL);

	do
	{
		status = ZwCreateFile(&hMntMgr,
			FILE_READ_DATA | FILE_WRITE_DATA,
			&oaMntMgr,
			&iosb,
			NULL,
			0,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			FILE_OPEN,
			FILE_NON_DIRECTORY_FILE,
			NULL,
			0);

		if (!NT_SUCCESS(status))
		{
			WDRBD_ERROR("could not open mount manager, status : 0x%x\n", status);
			break;
		}

		status = ZwCreateEvent(&hEvent, GENERIC_ALL, 0, NotificationEvent, FALSE);
		if (!NT_SUCCESS(status))
		{
			WDRBD_ERROR("could not create event, status : 0x%x\n", status);
			break;
		}

		// DW-1105: set state as 'working', this can be set as 'not working' by stop_mnt_monitor.
		atomic_set(&g_monitor_mnt_working, TRUE);

		MOUNTMGR_CHANGE_NOTIFY_INFO mcni1 = { 0, }, mcni2 = { 0, };
		while (TRUE == atomic_read(&g_monitor_mnt_working))
		{
			status = ZwDeviceIoControlFile(hMntMgr, hEvent, NULL, NULL, &iosb, IOCTL_MOUNTMGR_CHANGE_NOTIFY,
				&mcni1, sizeof(mcni1), &mcni2, sizeof(mcni2));

			if (!NT_SUCCESS(status))
			{
				WDRBD_ERROR("ZwDeviceIoControl with IOCTL_MOUNTMGR_CHANGE_NOTIFY has been failed, status : 0x%x\n", status);
				break;
			}
			else if (STATUS_PENDING == status)
			{
				status = ZwWaitForSingleObject(hEvent, TRUE, NULL);
			}

			// we've got notification, refresh all volume and adjust changes if necessary.
			HANDLE hVolRefresher = NULL;
			status = PsCreateSystemThread(&hVolRefresher, THREAD_ALL_ACCESS, NULL, NULL, NULL, adjust_changes_to_volume, NULL);
			if (!NT_SUCCESS(status))
			{
				WDRBD_ERROR("PsCreateSystemThread for adjust_changes_to_volume failed, status : 0x%x\n", status);
				break;
			}

			if (NULL != hVolRefresher)
			{
				ZwClose(hVolRefresher);
				hVolRefresher = NULL;
			}
			
			// prepare for next change.
			mcni1.EpicNumber = mcni2.EpicNumber;
		}

	} while (false);

	atomic_set(&g_monitor_mnt_working, FALSE);

	if (NULL != hMntMgr)
	{
		ZwClose(hMntMgr);
		hMntMgr = NULL;
	}
}

/* TODO: used? */	
// DW-1105: start monitoring mount change thread.
NTSTATUS start_mnt_monitor()
{
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	HANDLE	hVolMonitor = NULL;

	status = PsCreateSystemThread(&hVolMonitor, THREAD_ALL_ACCESS, NULL, NULL, NULL, monitor_mnt_change, NULL);
	if (!NT_SUCCESS(status))
	{
		WDRBD_ERROR("PsCreateSystemThread for monitor_mnt_change failed with status 0x%08X\n", status);
		return status;
	}

	if (NULL != hVolMonitor)
	{
		ZwClose(hVolMonitor);
		hVolMonitor = NULL;
	}

	return status;
}

// DW-1105: stop monitoring mount change thread.
void stop_mnt_monitor()
{
	atomic_set(&g_monitor_mnt_working, FALSE);
}

#define DRBD_REGISTRY_VOLUMES       L"\\volumes"

// DW-1109: delete drbd bdev when ref cnt gets 0, clean up all resources that has been created in create_drbd_block_device.
void delete_block_device(struct kref *kref)
{
	struct block_device *bdev = container_of(kref, struct block_device, kref);
// printk(KERN_INFO "delete_block_device %p\n", bdev);

	if (bdev->bd_disk) {
		if (bdev->bd_disk->queue)
			blk_cleanup_queue(bdev->bd_disk->queue);
		put_disk(bdev->bd_disk);
	}
	ObDereferenceObject(bdev->file_object);
	kfree(bdev->path_to_device.Buffer);

	list_del(&bdev->backing_devices_list);
	kfree(bdev);
}

static NTSTATUS resolve_nt_kernel_link(UNICODE_STRING *upath, UNICODE_STRING *link_target)
{
	NTSTATUS status;
	OBJECT_ATTRIBUTES device_attributes;
	ULONG link_target_length;
	HANDLE link_handle;
	IO_STATUS_BLOCK io_status_block;

	InitializeObjectAttributes(&device_attributes, upath, OBJ_FORCE_ACCESS_CHECK, NULL, NULL);
	status = ZwOpenSymbolicLinkObject(&link_handle, GENERIC_READ, &device_attributes);
	if (!NT_SUCCESS(status)) {
		WDRBD_WARN("ZwOpenSymbolicLinkObject: Cannot open link object, status = %x, path = %S\n", status, upath->Buffer);
		return status;
	}

	status = ZwQuerySymbolicLinkObject(link_handle, link_target, &link_target_length);
	if (!NT_SUCCESS(status)) {
		WDRBD_WARN("ZwQuerySymbolicLinkObject: Cannot get link target name, status = %x, path = %S\n", status, upath->Buffer);
		goto out_close_handle;
	}
	if (link_target_length >= link_target->MaximumLength) {
		WDRBD_WARN("ZwQuerySymbolicLinkObject: Link target name exceeds %lu bytes (is %lu bytes), path = %S\n", link_target->MaximumLength, link_target_length, upath->Buffer);
		goto out_close_handle;
	}
	link_target->Buffer[link_target_length] = 0;
	printk(KERN_INFO "Symbolic link points to %S\n", link_target->Buffer);

out_close_handle:
	ZwClose(link_handle);
	return status;
}


int resolve_ascii_path(const char *path, UNICODE_STRING *path_to_device)
{
	ANSI_STRING apath;
	UNICODE_STRING link_name;
	NTSTATUS status;

	RtlInitAnsiString(&apath, path);
	status = RtlAnsiStringToUnicodeString(&link_name, &apath, TRUE);

	path_to_device->Buffer = kmalloc(sizeof(WCHAR) * 1024, 0, 'BDRX');
	if (path_to_device->Buffer == NULL) {
		printk(KERN_ERR "Cannot allocate device name.\n");
		return -ENOMEM;
	}

	path_to_device->MaximumLength = 1024-1;
	path_to_device->Length = 0;

	WDRBD_TRACE("Link is %S\n", link_name.Buffer);
	if (resolve_nt_kernel_link(&link_name, path_to_device) != STATUS_SUCCESS) {
		WDRBD_ERROR("Could not resolve link.\n");
		return -EINVAL;
	}
	return 0;
}

static struct _DEVICE_OBJECT *find_windows_device(UNICODE_STRING *path, struct _FILE_OBJECT ** file_object)
{
	struct _DEVICE_OBJECT *windows_device;
	PFILE_OBJECT FileObject;
	NTSTATUS status;

	status = IoGetDeviceObjectPointer(path, STANDARD_RIGHTS_ALL | FILE_ALL_ACCESS, &FileObject, &windows_device);

	if (!NT_SUCCESS(status))
	{
		printk(KERN_ERR "Cannot get device object for %s status: %x, does it exist?\n", path, status);
		return NULL;
	}
	WDRBD_TRACE("IoGetDeviceObjectPointer %S succeeded, targetdev is %p\n", path->Buffer, windows_device);

	*file_object = FileObject;
	return windows_device;
}

static void backingdev_check_endio BIO_ENDIO_ARGS(struct bio *bio)
{
	struct completion *c = (struct completion*) bio->bi_private;
	complete(c);
}

static int check_if_backingdev_contains_filesystem(struct block_device *dev)
{
	struct bio *b = bio_alloc(0, 1, 'DRBD');
	int i;
	struct completion c;
	int ret;

	static char boot_sector[8192];
	struct page *p;

	mutex_lock(&read_bootsector_mutex);

	p = kzalloc(sizeof(struct page),0, 'D3DW'); 
	if (!p)	{
		printk(KERN_ERR "alloc_page struct page failed\n");
		mutex_unlock(&read_bootsector_mutex);
		return 1;
	}
	p->addr = boot_sector+(4096-((ULONG_PTR)boot_sector & 4095));

	bio_add_page(b, p, 512, 0);
	bio_set_op_attrs(b, REQ_OP_READ, 0);

	b->bi_end_io = backingdev_check_endio;
	b->bi_might_access_filesystem = true;
	b->dont_patch_boot_sector = true;

	DRBD_BIO_BI_SECTOR(b) = 0;
	init_completion(&c);
	b->bi_private = &c;
	bio_set_dev(b, dev);

	submit_bio(b);
	wait_for_completion(&c);

	ret = is_filesystem(p->addr);

/* TODO: this might cause a blue screen from time to time.
	Fix free_mdls_and_irp() to handle this */

/*
	bio_put(b);
*/

	kfree(p);

	mutex_unlock(&read_bootsector_mutex);

	return ret;
}

/* This creates a new block device associated with the windows
   device pointed to by path.
 */

struct block_device *blkdev_get_by_path(const char *path, fmode_t mode, void *holder)
{
	struct block_device *block_device;
	NTSTATUS status;
	struct _DEVICE_OBJECT *windows_device;
	struct _FILE_OBJECT *file_object;
	int err = 0;
	UNICODE_STRING path_to_device;

	err = resolve_ascii_path(path, &path_to_device);
	if (err < 0)
		return ERR_PTR(err);

	list_for_each_entry(struct block_device, block_device, &backing_devices, backing_devices_list) {
		if (RtlEqualUnicodeString(&block_device->path_to_device, &path_to_device, TRUE)) {
			printk(KERN_DEBUG "Block device for windows device %S already open, reusing it (block_device %p)\n", path_to_device.Buffer, block_device);

			kfree(path_to_device.Buffer);
				/* we got an extra reference in 
				 * find_windows_device()
				 */
			kref_get(&block_device->kref);
			return block_device;
		}
	}

	windows_device = find_windows_device(&path_to_device, &file_object);
	if (windows_device == NULL) {
		err = -ENOENT;
		goto out_no_windows_device;
	}

	block_device = kmalloc(sizeof(struct block_device), 0, 'DBRD');
	if (block_device == NULL) {
		WDRBD_ERROR("could not allocate block_device.\n");
		err = -ENOMEM;
		goto out_no_block_device;
	}
	block_device->windows_device = windows_device;
// printk("block_device: %p block_device->windows_device: %p\n", block_device, block_device->windows_device);

	block_device->bd_disk = alloc_disk(0);
	if (!block_device->bd_disk)
	{
		WDRBD_ERROR("Failed to allocate gendisk NonPagedMemory\n");
		err = -ENOMEM;
		goto out_no_disk;
	}

	block_device->bd_disk->queue = blk_alloc_queue(0);
	if (!block_device->bd_disk->queue)
	{
		WDRBD_ERROR("Failed to allocate request_queue NonPagedMemory\n");
		err = -ENOMEM;
		goto out_no_queue;
	}
	IoInitializeRemoveLock(&block_device->remove_lock, 'DRBD', 0, 0);
	status = IoAcquireRemoveLock(&block_device->remove_lock, NULL);
	if (!NT_SUCCESS(status)) {
		WDRBD_ERROR("Failed to acquire remove lock, status is %s\n", status);
		err = -EBUSY;
		goto out_remove_lock_error;
	}

        kref_init(&block_device->kref);
 
	block_device->bd_contains = block_device;
	block_device->bd_parent = NULL;

		/* TODO: not always? */
	block_device->bd_block_size = 512;
	block_device->bd_disk->queue->logical_block_size = 512;

	block_device->file_object = file_object;

	mutex_init(&block_device->vol_size_mutex);
	block_device->d_size = windrbd_get_volsize(block_device);
	if (block_device->d_size == -1) {
		printk(KERN_ERR "Cannot get volsize.\n");
		err = -EINVAL;
		goto out_get_volsize_error;
	}
	block_device->path_to_device = path_to_device;

	if (check_if_backingdev_contains_filesystem(block_device)) {
		printk(KERN_ERR "Backing device contains filesystem, refusing to use it.\n");
		printk(KERN_INFO "You may want to do something like windrbd hide-filesystem <drive-letter-of-backing-dev>\n");
		err = -EINVAL;
		goto out_get_volsize_error;
	}

	printk(KERN_DEBUG "blkdev_get_by_path succeeded %p windows_device %p.\n", block_device, block_device->windows_device);

	list_add(&block_device->backing_devices_list, &backing_devices);

#ifdef _HACK
hack_alloc_page(block_device);
#endif

	return block_device;

out_get_volsize_error:
	IoReleaseRemoveLock(&block_device->remove_lock, NULL);
out_remove_lock_error:
	blk_cleanup_queue(block_device->bd_disk->queue);
out_no_queue:
	put_disk(block_device->bd_disk);
out_no_disk:
	kfree(block_device);
out_no_block_device:
	ObDereferenceObject(file_object);
out_no_windows_device:
	kfree(path_to_device.Buffer);

	return ERR_PTR(err);
}

int call_usermodehelper(char *path, char **argv, char **envp, enum umh_wait wait)
{
	SOCKADDR_IN		LocalAddress = { 0 }, RemoteAddress = { 0 };
	NTSTATUS		Status = STATUS_UNSUCCESSFUL;
	PWSK_SOCKET		Socket = NULL;
	char *cmd_line;
	int leng;
	char ret = 0;

	if (0 == g_handler_use)
	{
		return -1;
	}

	leng = strlen(path) + 1 + strlen(argv[0]) + 1 + strlen(argv[1]) + 1 + strlen(argv[2]) + 1;
	cmd_line = kcalloc(leng, 1, 0, '64DW');
	if (!cmd_line)
	{
		WDRBD_ERROR("malloc(%d) failed", leng);
		return -1;
	}

    sprintf(cmd_line, "%s %s\0", argv[1], argv[2]); // except "drbdadm.exe" string
    WDRBD_INFO("malloc len(%d) cmd_line(%s)\n", leng, cmd_line);

    Socket = CreateSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, NULL, WSK_FLAG_CONNECTION_SOCKET);
	if (Socket == NULL) {
		WDRBD_ERROR("CreateSocket() returned NULL\n");
		kfree(cmd_line);
		return -1; 
	}

	LocalAddress.sin_family = AF_INET;
	LocalAddress.sin_addr.s_addr = INADDR_ANY;
	LocalAddress.sin_port = 0; 

	Status = Bind(Socket, (PSOCKADDR) &LocalAddress);
	if (!NT_SUCCESS(Status)) {
		goto error;
	}

	RemoteAddress.sin_family = AF_INET;
	RemoteAddress.sin_addr.S_un.S_un_b.s_b1 = 127;
	RemoteAddress.sin_addr.S_un.S_un_b.s_b2 = 0;
	RemoteAddress.sin_addr.S_un.S_un_b.s_b3 = 0;
	RemoteAddress.sin_addr.S_un.S_un_b.s_b4 = 1;
	RemoteAddress.sin_port = HTONS(g_daemon_tcp_port); 

	Status = Connect(Socket, (PSOCKADDR) &RemoteAddress);
	if (!NT_SUCCESS(Status)) {
		goto error;;
	}
	else if (Status == STATUS_TIMEOUT)
	{
		WDRBD_INFO("Connect() timeout. IRQL(%d)\n", KeGetCurrentIrql());
		goto error;
	}

	WDRBD_INFO("Connected to the %u.%u.%u.%u:%u  status:0x%08X IRQL(%d)\n", 
			RemoteAddress.sin_addr.S_un.S_un_b.s_b1,
			RemoteAddress.sin_addr.S_un.S_un_b.s_b2,
			RemoteAddress.sin_addr.S_un.S_un_b.s_b3,
			RemoteAddress.sin_addr.S_un.S_un_b.s_b4,
			HTONS(RemoteAddress.sin_port),
			Status, KeGetCurrentIrql());

	{
		LONG readcount;
		char hello[2];
		WDRBD_TRACE("Wait Hi\n");
		if ((readcount = Receive(Socket, &hello, 2, 0, g_handler_timeout)) == 2)
		{
			WDRBD_TRACE("recv HI!!! \n");
			//CloseSocket(Socket);
			//kfree(cmd_line);
			//return ret; 
		}
		else
		{
			if (readcount == -EAGAIN)
			{
				WDRBD_INFO("error rx hi timeout(%d) g_handler_retry(%d) !!!!\n", g_handler_timeout, g_handler_retry);
			}
			else
			{
				WDRBD_INFO("error recv status=0x%x\n", readcount);
			}
			ret = -1;

			// _WIN32_HANDLER_TIMEOUT
			goto error;
		}

#ifdef _WIN32
		if ((Status = SendLocal(Socket, cmd_line, strlen(cmd_line), 0, g_handler_timeout)) != (long) strlen(cmd_line))
#endif
		{
			WDRBD_ERROR("send command fail stat=0x%x\n", Status);
			ret = -1;
			goto error;
		}

		//WDRBD_INFO("send local done %s! Disconnect\n", Status);
		//Disconnect(Socket);

		if ((readcount = Receive(Socket, &ret, 1, 0, g_handler_timeout)) > 0)
		{
			WDRBD_TRACE("recv val=0x%x\n", ret);
			//CloseSocket(Socket);
			//kfree(cmd_line);
			//return ret; 
		}
		else
		{
			if (readcount == -EAGAIN)
			{
				WDRBD_INFO("recv retval timeout(%d)!\n", g_handler_timeout);
			}
			else
			{
			
				WDRBD_INFO("recv status=0x%x\n", readcount);
			}
			ret = -1;
			goto error;
		}

#ifdef _WIN32
		if ((Status = SendLocal(Socket, "BYE", 3, 0, g_handler_timeout)) != 3)
#endif
		{
			WDRBD_ERROR("send bye fail stat=0x%x\n", Status); // ignore!
		}

		WDRBD_TRACE("Disconnect:shutdown...\n", Status);
		Disconnect(Socket);

		/*
		if ((readcount = Receive(Socket, &ret, 1, 0, 0)) > 0)
		{
			WDRBD_INFO("recv dummy  val=0x%x\n", ret);// ignore!
		}
		else
		{
			WDRBD_INFO("recv dummy  status=%d\n", readcount);// ignore!
		}
		*/
	}

error:
	CloseSocket(Socket);
	kfree(cmd_line);
	return ret;
}

void panic(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	printk(fmt, args);
	va_end(args);
	KeBugCheckEx(0xddbd, (ULONG_PTR)__FILE__, (ULONG_PTR)__func__, 0x12345678, 0xd8bdd8bd);
}

int scnprintf(char * buf, size_t size, const char *fmt, ...)
{
	va_list args;
	int i = 0;

	va_start(args, fmt);
    i = _vsnprintf_s(buf, size, _TRUNCATE, fmt, args);
	va_end(args);
	return (-1 == i) ? (size - 1) : i;
}

int list_is_singular(const struct list_head *head)
{
	return !list_empty(head) && (head->next == head->prev);
}

void __list_cut_position(struct list_head *list, struct list_head *head, struct list_head *entry)
{
	struct list_head *new_first = entry->next;
	list->next = head->next;
	list->next->prev = list;
	list->prev = entry;
	entry->next = list;
	head->next = new_first;
	new_first->prev = head;
}
// from linux kernel 3.14 
void list_cut_position(struct list_head *list, struct list_head *head, struct list_head *entry)
{
	if (list_empty(head))
		return;
	if (list_is_singular(head) &&
		(head->next != entry && head != entry))
		return;
	if (entry == head)
		INIT_LIST_HEAD(list);
	else
		__list_cut_position(list, head, entry);
}

int drbd_backing_bdev_events(struct gendisk *device)
{
    /* TODO */
	return 0;
}

char * get_ip4(char *buf, struct sockaddr_in *sockaddr)
{
	sprintf(buf, "%u.%u.%u.%u:%u\0",
		sockaddr->sin_addr.S_un.S_un_b.s_b1,
		sockaddr->sin_addr.S_un.S_un_b.s_b2,
		sockaddr->sin_addr.S_un.S_un_b.s_b3,
		sockaddr->sin_addr.S_un.S_un_b.s_b4,
		HTONS(sockaddr->sin_port)
		);
	return buf;
}
#ifdef _WIN32
char * get_ip6(char *buf, struct sockaddr_in6 *sockaddr)
{
	sprintf(buf, "[%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x]:%u\0", 
			sockaddr->sin6_addr.u.Byte[0],
			sockaddr->sin6_addr.u.Byte[1],
			sockaddr->sin6_addr.u.Byte[2],
			sockaddr->sin6_addr.u.Byte[3],
			sockaddr->sin6_addr.u.Byte[4],
			sockaddr->sin6_addr.u.Byte[5],
			sockaddr->sin6_addr.u.Byte[6],
			sockaddr->sin6_addr.u.Byte[7],
			sockaddr->sin6_addr.u.Byte[8],
			sockaddr->sin6_addr.u.Byte[9],
			sockaddr->sin6_addr.u.Byte[10],
			sockaddr->sin6_addr.u.Byte[11],
			sockaddr->sin6_addr.u.Byte[12],
			sockaddr->sin6_addr.u.Byte[13],
			sockaddr->sin6_addr.u.Byte[14],
			sockaddr->sin6_addr.u.Byte[15],
			HTONS(sockaddr->sin6_port)
			);
	return buf;
}
#endif

struct blk_plug_cb *blk_check_plugged(blk_plug_cb_fn unplug, void *data,
				      int size)
{
#ifndef _WIN32
	struct blk_plug *plug = current->plug;
	struct blk_plug_cb *cb;

	if (!plug)
		return NULL;

	list_for_each_entry(struct blk_plug_cb, cb, &plug->cb_list, list)
		if (cb->callback == unplug && cb->data == data)
			return cb;

	/* Not currently on the callback list */
	BUG_ON(size < sizeof(*cb));
	cb = kzalloc(size, GFP_ATOMIC, 'D8DW');
	if (cb) {
		cb->data = data;
		cb->callback = unplug;
		list_add(&cb->list, &plug->cb_list);
	}
	return cb;
#else
	return NULL;
#endif
}
/* Save current value in registry, this value is used when drbd is loading.*/
NTSTATUS SaveCurrentValue(PCWSTR valueName, int value)
{
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	PROOT_EXTENSION pRootExtension = NULL;
	UNICODE_STRING usValueName = { 0, };
	OBJECT_ATTRIBUTES oa = { 0, };
	HANDLE hKey = NULL;

	if (NULL == mvolRootDeviceObject ||
		NULL == mvolRootDeviceObject->DeviceExtension)
	{
		return STATUS_UNSUCCESSFUL;
	}

	do
	{
		pRootExtension = mvolRootDeviceObject->DeviceExtension;

		InitializeObjectAttributes(&oa, &pRootExtension->RegistryPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

		status = ZwOpenKey(&hKey, KEY_ALL_ACCESS, &oa);
		if (!NT_SUCCESS(status))
		{
			break;
		}

		RtlInitUnicodeString(&usValueName, valueName);
		status = ZwSetValueKey(hKey, &usValueName, 0, REG_DWORD, &value, sizeof(value));
		if (!NT_SUCCESS(status))
		{
			break;
		}

	} while (FALSE);

	if (NULL != hKey)
	{
		ZwClose(hKey);
		hKey = NULL;
	}

	return status;
}

sector_t windrbd_get_capacity(struct block_device *bdev)
{
	if (bdev == NULL) {
		printk(KERN_WARNING "bdev is NULL\n");
		return 0;
	}

	return bdev->d_size >> 9;
}

int windrbd_thread_setup(struct drbd_thread *thi)
{
	struct drbd_resource *resource = thi->resource;
	struct drbd_connection *connection = thi->connection;
	int res;
	NTSTATUS status;

	thi->nt = ct_add_thread(KeGetCurrentThread(), thi->name, TRUE, 'B0DW');
	if (!thi->nt)
	{
		WDRBD_ERROR("DRBD_PANIC: ct_add_thread failed.\n");
		PsTerminateSystemThread(STATUS_SUCCESS);
	}

	KeSetEvent(&thi->start_event, 0, FALSE);
	KeWaitForSingleObject(&thi->wait_event, Executive, KernelMode, FALSE, NULL);

	res = drbd_thread_setup(thi);

	// TODO ct_delete_thread(thi->task->pid); ??
	if (res)
		WDRBD_ERROR("stop, result %d\n", res);
	else
		WDRBD_INFO("stopped.\n");

	status = PsTerminateSystemThread(STATUS_SUCCESS);
	printk(KERN_ERR "PsTerminateSystenThread() returned (status: %x). This is not good.\n", status);

	return STATUS_SUCCESS;
}

/* This fills a name of the form \DosDevices\x: into unicode string
   dos name where drive letter is 'C'+minor (C:=0, D:=1, ...). 
   If internal is set, the result is \Device\Drbd<n> instead where
   n is a number (Drbd1, ...)

   TODO: This should eventually be replaced by a udev-like mechanism 
   where the user specifies the name in the drbd.conf.

   Space is allocated by this function and must be freed by the
   caller.
 */

	/* Note that this is unicode characters */
	/* TODO: lookup windows constant and use it */

// #define MAX_WINDOWS_KERNEL_PATH_LENGTH 32767
	/* TODO: if string is too long RtlSomethingPrintf returns
	 * an error */
#define MAX_WINDOWS_KERNEL_PATH_LENGTH 200

static int minor_to_x_name(UNICODE_STRING *name, int minor, const char *mount_point)
{
	NTSTATUS status;

	name->Buffer = ExAllocatePool(NonPagedPool, MAX_WINDOWS_KERNEL_PATH_LENGTH * sizeof(name->Buffer[0]));

	if (name->Buffer == NULL) {
		WDRBD_WARN("minor_to_x_name: couldn't allocate memory for name buffer\n");
		return -ENOMEM;
	}
	name->Length = 0;
	name->MaximumLength = MAX_WINDOWS_KERNEL_PATH_LENGTH;

	if (mount_point) {
		ANSI_STRING a;
		UNICODE_STRING u;
	        RtlInitAnsiString(&a, mount_point);
		status = RtlAnsiStringToUnicodeString(&u, &a, TRUE);
		if (status == STATUS_SUCCESS) {
			status = RtlUnicodeStringPrintf(name, L"\\DosDevices\\%s", u.Buffer);
			RtlFreeUnicodeString(&u);
		}
	} else {
		status = RtlUnicodeStringPrintf(name, L"\\Device\\Drbd%d", minor);
	}

	if (status != STATUS_SUCCESS) {
		WDRBD_WARN("minor_to_dos_name: couldn't printf device name for minor %d status: %x\n", minor, status);

		ExFreePool(name->Buffer);
		return -EINVAL;
	}
	name->Buffer[name->Length / sizeof(name->Buffer[0])] = 0;
	return 0;
}

/* TODO: this should take a string as argument, check if the device
	 exists (by calling blkdev_get_by_path) and create the
	 new DRBD device if it does not exist.

	 For now, it should be an error if the device exists. */

/* TODO: 100 -> MAX_PATH_LEN */
/* TODO: name, dos_name into device extension */

struct block_device *bdget(dev_t device_no)
{
	dev_t minor = MINOR(device_no);
	UNICODE_STRING name;
        PDEVICE_OBJECT new_device;
	NTSTATUS status;
	struct block_device *block_device;
	struct _VOLUME_EXTENSION *pvext;
	UNICODE_STRING sddl;
//	UNICODE_STRING interfaceName;

	if (minor_to_x_name(&name, minor, NULL) < 0) {
		return NULL;
	}

		/* TODO: understand what this means */
	RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;BU)");

	status = IoCreateDeviceSecure(mvolDriverObject, 
		                sizeof(struct block_device), 
		                &name,
		                FILE_DEVICE_VIRTUAL_DISK,
                                0,
                                FALSE,
				&sddl,
				NULL,
//				&DRBD_DEVICE_CLASS_GUID,
                                &new_device);
/*
	status = IoCreateDevice(mvolDriverObject, 
		                sizeof(struct block_device), 
				NULL,
//		                &name,
		                FILE_DEVICE_VIRTUAL_DISK,
                                0,
                                FALSE,
                                &new_device);
*/

	if (status != STATUS_SUCCESS) {
		WDRBD_WARN("bdget: couldn't create new block device %S for minor %d status: %x\n", name.Buffer, minor, status);

		return NULL;
	}

	block_device = new_device->DeviceExtension;
	kref_init(&block_device->kref);

	IoInitializeRemoveLock(&block_device->remove_lock, 'DRBD', 0, 0);
	status = IoAcquireRemoveLock(&block_device->remove_lock, NULL);
	if (!NT_SUCCESS(status)) {
		WDRBD_ERROR("Failed to acquire remove lock, status is %s\n", status);
		goto out_remove_lock_failed;
	}

	block_device->windows_device = new_device;
	block_device->minor = minor;
	block_device->bd_block_size = 512;
	block_device->path_to_device = name;
	block_device->mount_point.Buffer = NULL;

	printk(KERN_INFO "Created new block device %S (minor %d).\n", name.Buffer, minor);
	
	new_device->Flags &= ~DO_DEVICE_INITIALIZING;

#if 0
	status = IoRegisterDeviceInterface(new_device, &MOUNTDEV_MOUNTED_DEVICE_GUID, NULL, &interfaceName);

	if (status != STATUS_SUCCESS) {
		WDRBD_WARN("bdget: couldn't register MOUNTED_DEVICE_GUID class for block device %S for minor %d status: %x\n", name.Buffer, minor, status);

		goto out_register_device_interface_failed;
	}
printk("karin interface name: %S\n", interfaceName.Buffer);
#endif

	return block_device;

#if 0
out_register_device_interface_failed:
	if (IoDeleteSymbolicLink(&dos_name) != STATUS_SUCCESS) {
		WDRBD_WARN("Failed to remove symbolic link again %S\n", dos_name.Buffer);
	}
#endif
out_remove_lock_failed:
	IoDeleteDevice(new_device);

	return NULL;
}

int windrbd_mount(struct block_device *dev, const char *mount_point)
{
	NTSTATUS status;

	if (mount_point == NULL)
		return 0;

	if (minor_to_x_name(&dev->mount_point, -1, mount_point) < 0)
		return -1;
	
	status = IoCreateSymbolicLink(&dev->mount_point, &dev->path_to_device);
	if (status != STATUS_SUCCESS) {
		printk("windrbd_mount: couldn't symlink %S to %S status: %x\n", dev->path_to_device.Buffer, dev->mount_point.Buffer, status);
		return -1;

	}
	printk(KERN_INFO "Assigned device %S the mount point %S\n", dev->path_to_device.Buffer, dev->mount_point.Buffer);
	return 0;
}

static void destroy_block_device(struct kref *kref)
{
	struct block_device *bdev = container_of(kref, struct block_device, kref);
	int minor = bdev->minor;

// printk(KERN_INFO "Destroying minor %d\n", minor);

	ExFreePool(bdev->path_to_device.Buffer);
	bdev->windows_device->DeviceExtension = NULL;
		/* TODO: this does not really delete the device
		 * if somebody still holds a reference. For example,
		 * if there is a cmd shell open it will access the
		 * device after this has happened.
		 */

	if (bdev->mount_point.Buffer != NULL) {
		if (IoDeleteSymbolicLink(&bdev->mount_point) != STATUS_SUCCESS) {
			WDRBD_WARN("Failed to remove symbolic link (drive letter) %S\n", bdev->mount_point.Buffer);
		}
		ExFreePool(bdev->mount_point.Buffer);
	}

	IoDeleteDevice(bdev->windows_device); /* should also delete the device extension, which is the bdev */
/* TODO: not freeing bdev? */
}

void bdput(struct block_device *this_bdev)
{
	kref_put(&this_bdev->kref, destroy_block_device);
}


/* TODO: Implement using Windows timers */
ktime_t ktime_get(void)
{
#if 0
	struct timekeeper *tk = &tk_core.timekeeper;
	unsigned int seq;
	ktime_t base;
	u64 nsecs;

	WARN_ON(timekeeping_suspended);

	do {
		seq = read_seqcount_begin(&tk_core.seq);
		base = tk->tkr_mono.base;
		nsecs = timekeeping_get_ns(&tk->tkr_mono);

	} while (read_seqcount_retry(&tk_core.seq, seq));

	return ktime_add_ns(base, nsecs);
#endif
	return (ktime_t) { .tv64 = 0 };
}
