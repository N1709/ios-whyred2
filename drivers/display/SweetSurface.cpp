/*
 * Built-in IOSurface for SWEET.
 *
 * Apple's IOSurface kext is closed. IOSurface.framework (iOS 16.7) finds
 * IOServiceNameMatching("IOCoreSurfaceRoot"), opens it with type 0 and
 * talks to the user client below:
 *
 *   sel 13  limits: struct out 0x18, u16 at +0x16 must be 1 (ABI version),
 *           +0 offset alignment mask, +4 bytes-per-row alignment mask
 *   sel 0   create: scalar [parent id], struct in = serialized property
 *           dictionary (binary or XML), struct out = lock result (0xa68)
 *   sel 4   lookup by id: scalar [id], struct out = lock result
 *   sel 34  lookup by mach port: scalar [port name], struct out = lock result
 *   sel 35  create mach port: scalars [id, options], scalar out [port name]
 *   traps   0/1 use count +/- (id, category), 2 lock / 3 unlock
 *           (id, options, &seed), 4/5 release (id)
 *
 * The lock result tells the library where the surface lives in the
 * caller's address space: +0 pixels, +8 shared state page (registry id,
 * seed at +0xc), +0x10 timestamps, then id, sizes and geometry (layout in
 * sweet_surface_fill_result). Every surface is wired kernel memory mapped
 * into each task that creates or looks it up; mappings are counted per
 * client and dropped on release or close.
 *
 * Values (9/10/11) live in a dictionary per surface, their seed and size in
 * the state page (+0x20/+0x28); bulk attachments (27: colour space, transfer
 * function...) are kept at +0x30 of the state page, where the framework reads
 * them; purgeability (20), accelerator binding (12) and the YCbCr matrix (5)
 * are remembered. Notifications, ownership and timestamps are accepted and
 * ignored (logged once per selector).
 */
#include "SweetSurface.h"

#if defined(SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/system.h>
#include <pexpert/pexpert.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSUnserialize.h>
#include <libkern/c++/OSSerialize.h>
#include <sys/proc.h>

enum {
	kSweetSurfaceCreate             = 0,
	kSweetSurfaceLookup             = 4,
	kSweetSurfaceSetYCbCrMatrix     = 5,
	kSweetSurfaceWrapClientImage    = 6,
	kSweetSurfaceWrapClientMemory   = 7,
	kSweetSurfaceSetValue           = 9,
	kSweetSurfaceCopyValue          = 10,
	kSweetSurfaceRemoveValue        = 11,
	kSweetSurfaceBindAccel          = 12,
	kSweetSurfaceSetPurgeable       = 20,
	kSweetSurfaceSetBulkAttachments = 27,
	kSweetSurfaceLimits             = 13,
	kSweetSurfaceLookupFromPort     = 34,
	kSweetSurfaceCreatePort         = 35,
};

enum {
	kSweetSurfaceTrapIncrementUse   = 0,
	kSweetSurfaceTrapDecrementUse   = 1,
	kSweetSurfaceTrapLock           = 2,
	kSweetSurfaceTrapUnlock         = 3,
	kSweetSurfaceTrapRelease        = 4,
	kSweetSurfaceTrapReleaseFailed  = 5,
};

#define SWEET_SURFACE_RESULT_SIZE       0xa68
#define SWEET_SURFACE_MAX_ID            4096
#define SWEET_SURFACE_MAX_DIM           16384
#define SWEET_SURFACE_MAX_ALLOC         (256u << 20)
#define SWEET_SURFACE_MAX_PROPS         (64u << 10)
#define SWEET_SURFACE_ROW_ALIGN         64
#define SWEET_SURFACE_TIMESTAMPS        0x800   /* offset in the state page */
#define SWEET_SURFACE_STATE_SEED        0xc     /* u32 in the state page */
#define SWEET_SURFACE_STATE_ACCEL       0x8     /* u16 accelerator id, u16 sub id */
#define SWEET_SURFACE_STATE_VSEED       0x20    /* u32 values seed */
#define SWEET_SURFACE_STATE_YCBCR       0x24    /* u32 */
#define SWEET_SURFACE_STATE_VSIZE       0x28    /* u32 room the values need when fetched, 0: none */
#define SWEET_SURFACE_STATE_BULK        0x30    /* 0x70 bytes of bulk attachments */
#define SWEET_SURFACE_BULK_SIZE         0x70
#define kIOSurfaceLockReadOnly          0x1

static IOLock       *sweet_surface_lock;
static SweetSurface *sweet_surface_table[SWEET_SURFACE_MAX_ID];
static uint32_t      sweet_surface_users[SWEET_SURFACE_MAX_ID];
static uint32_t      sweet_surface_next_id = 1;
static uint32_t      sweet_surface_creates_logged;
static uint64_t      sweet_surface_wired;       /* bytes of buffer surfaces */
#define SWEET_SURFACE_WIRED_MAX         (1536ull << 20)

/* topaz scanout: at most 6 contiguous 16 MiB surfaces */
#define SWEET_SCANOUT_SLOT      (16u << 20)
#define SWEET_SCANOUT_SLOTS     6
static uint32_t sweet_scanout_used;             /* bit per slot, sweet_surface_lock */
static uint32_t sweet_scanout_w[2], sweet_scanout_h[2];
static int      sweet_scanout_enabled = -1;

void
sweet_scanout_set_geometry(uint32_t w1, uint32_t h1, uint32_t w2, uint32_t h2)
{
	sweet_scanout_w[0] = w1;
	sweet_scanout_h[0] = h1;
	sweet_scanout_w[1] = w2;
	sweet_scanout_h[1] = h2;
}

static bool
sweet_scanout_eligible(const SweetSurface *s)
{
	if (sweet_scanout_enabled < 0) {
		int on = 1;
		PE_parse_boot_argn("sweetfb_direct", &on, sizeof(on));
		sweet_scanout_enabled = on;
	}
	if (!sweet_scanout_enabled || s->bytesPerElement != 4 || s->planeCount > 1 ||
	    s->elementWidth != 1 || s->elementHeight != 1 || s->allocSize > SWEET_SCANOUT_SLOT) {
		return false;
	}
	for (int i = 0; i < 2; i++) {
		if (sweet_scanout_w[i] && s->width == sweet_scanout_w[i] && s->height == sweet_scanout_h[i]) {
			return true;
		}
	}
	return false;
}

static int
sweet_scanout_alloc_slot(void)
{
	int slot = -1;
	IOLockLock(sweet_surface_lock);
	for (int i = 0; i < SWEET_SCANOUT_SLOTS; i++) {
		if (!(sweet_scanout_used & (1u << i))) {
			sweet_scanout_used |= 1u << i;
			slot = i;
			break;
		}
	}
	IOLockUnlock(sweet_surface_lock);
	return slot;
}

static void
sweet_scanout_free_slot(int slot)
{
	if (slot < 0) {
		return;
	}
	IOLockLock(sweet_surface_lock);
	sweet_scanout_used &= ~(1u << slot);
	IOLockUnlock(sweet_surface_lock);
}

OSDefineMetaClassAndStructors(SweetSurface, OSObject);

void
SweetSurface::free()
{
	if (OSDynamicCast(IOBufferMemoryDescriptor, memory) != NULL) {
		OSAddAtomic64(-(SInt64)allocSize, (volatile SInt64 *)&sweet_surface_wired);
	}
	OSSafeReleaseNULL(kernelMap);
	OSSafeReleaseNULL(values);
	if (wrapped && memory != NULL) {
		memory->complete();
	}
	OSSafeReleaseNULL(memory);
	OSSafeReleaseNULL(state);
	sweet_scanout_free_slot(scanoutSlot);
	scanoutSlot = -1;
	OSObject::free();
}

uintptr_t
SweetSurface::kernelAddress()
{
	IOBufferMemoryDescriptor *buffer = OSDynamicCast(IOBufferMemoryDescriptor, memory);
	if (buffer != NULL) {
		return (uintptr_t)buffer->getBytesNoCopy();
	}
	IOLockLock(sweet_surface_lock);
	if (kernelMap == NULL && memory != NULL) {
		kernelMap = memory->map(scanoutPhys || wrapped ? kIOMapDefaultCache : kIOMapWriteCombineCache);
	}
	IOLockUnlock(sweet_surface_lock);
	return kernelMap != NULL ? (uintptr_t)kernelMap->getVirtualAddress() : 0;
}

static uint32_t *
sweet_surface_seed(SweetSurface *s)
{
	return (uint32_t *)((uint8_t *)s->state->getBytesNoCopy() + SWEET_SURFACE_STATE_SEED);
}

/*
 * Give the surface an id and a table slot; the table holds one retain and a
 * user count (creators, client mappings, kernel holders). Lock held.
 */
static bool
sweet_surface_insert_locked(SweetSurface *s)
{
	for (uint32_t n = 0; n < SWEET_SURFACE_MAX_ID - 1; n++) {
		uint32_t id = sweet_surface_next_id;
		sweet_surface_next_id = id + 1 < SWEET_SURFACE_MAX_ID ? id + 1 : 1;
		if (sweet_surface_table[id] == NULL) {
			s->id = id;
			s->retain();
			sweet_surface_table[id] = s;
			sweet_surface_users[id] = 1;
			*(uint64_t *)s->state->getBytesNoCopy() = id;   /* registry id */
			return true;
		}
	}
	return false;
}

/* Add a user to a surface that may have left the table (port lookups). */
static bool
sweet_surface_get(SweetSurface *s)
{
	bool ok = true;
	IOLockLock(sweet_surface_lock);
	if (sweet_surface_table[s->id] == s) {
		sweet_surface_users[s->id]++;
	} else if (sweet_surface_table[s->id] == NULL) {
		s->retain();
		sweet_surface_table[s->id] = s;
		sweet_surface_users[s->id] = 1;
	} else {
		ok = false;
	}
	IOLockUnlock(sweet_surface_lock);
	return ok;
}

/* Drop a user; the last one removes the surface from the table. */
static void
sweet_surface_put(SweetSurface *s)
{
	bool drop = false;
	IOLockLock(sweet_surface_lock);
	if (sweet_surface_table[s->id] == s && --sweet_surface_users[s->id] == 0) {
		sweet_surface_table[s->id] = NULL;
		drop = true;
	}
	IOLockUnlock(sweet_surface_lock);
	if (drop) {
		s->release();
	}
}

SweetSurface *
sweet_surface_copy(uint32_t id)
{
	SweetSurface *s = NULL;
	if (id == 0 || id >= SWEET_SURFACE_MAX_ID || sweet_surface_lock == NULL) {
		return NULL;
	}
	IOLockLock(sweet_surface_lock);
	s = sweet_surface_table[id];
	if (s != NULL) {
		s->retain();
	}
	IOLockUnlock(sweet_surface_lock);
	return s;
}

/* The table lock; the framebuffer may publish a surface before the root starts. */
static void
sweet_surface_init_lock(void)
{
	if (sweet_surface_lock == NULL) {
		IOLock *lock = IOLockAlloc();
		if (!OSCompareAndSwapPtr(NULL, lock, &sweet_surface_lock)) {
			IOLockFree(lock);
		}
	}
}

static SweetSurface *
sweet_surface_alloc(void)
{
	sweet_surface_init_lock();
	SweetSurface *s = OSTypeAlloc(SweetSurface);
	if (s == NULL || !s->init()) {
		OSSafeReleaseNULL(s);
		return NULL;
	}
	s->state = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared, page_size, page_size);
	if (s->state == NULL) {
		s->release();
		return NULL;
	}
	bzero(s->state->getBytesNoCopy(), page_size);
	return s;
}

static bool
sweet_surface_publish(SweetSurface *s)
{
	bool ok;
	IOLockLock(sweet_surface_lock);
	ok = sweet_surface_insert_locked(s);
	IOLockUnlock(sweet_surface_lock);
	return ok;
}

SweetSurface *
sweet_surface_create_physical(uint64_t phys, uint32_t width, uint32_t height,
    uint32_t bytesPerRow, uint32_t pixelFormat)
{
	SweetSurface *s = sweet_surface_alloc();
	if (s == NULL) {
		return NULL;
	}
	s->width = width;
	s->height = height;
	s->bytesPerRow = bytesPerRow;
	s->bytesPerElement = 4;
	s->elementWidth = s->elementHeight = 1;
	s->pixelFormat = pixelFormat;
	s->allocSize = (uint32_t)round_page(bytesPerRow * height);
	s->memory = IOMemoryDescriptor::withPhysicalAddress(phys, s->allocSize, kIODirectionInOut);
	s->mapCache = kIOMapWriteCombineCache;
	if (s->memory == NULL || !sweet_surface_publish(s)) {
		s->release();
		return NULL;
	}
	return s;
}

mach_port_name_t
sweet_surface_port_in_task(SweetSurface *s, task_t task)
{
	mach_port_name_t name = 0;
	IOUserClient::copyPortNameForObjectInTask(task, s, &name);
	return name;
}

static uint32_t
sweet_dict_u32(OSDictionary *dict, const char *key, uint32_t def)
{
	OSNumber *n = OSDynamicCast(OSNumber, dict->getObject(key));
	return n != NULL ? n->unsigned32BitValue() : def;
}

static IOReturn sweet_surface_use_task_memory(SweetSurface *s, task_t task, uint64_t address, uint64_t size);

static uint32_t
sweet_div_up(uint32_t a, uint32_t b)
{
	return b > 1 ? (a + b - 1) / b : a;
}

static uint32_t
sweet_row_bytes(uint32_t width, uint32_t ew, uint32_t bpe)
{
	uint64_t row = (uint64_t)sweet_div_up(width, ew) * bpe;
	return (uint32_t)((row + SWEET_SURFACE_ROW_ALIGN - 1) & ~(uint64_t)(SWEET_SURFACE_ROW_ALIGN - 1));
}

/* Geometry from the creation properties (IOSurface.h keys). */
static IOReturn
sweet_surface_layout(SweetSurface *s, OSDictionary *props)
{
	uint64_t end;

	s->width = sweet_dict_u32(props, "IOSurfaceWidth", 0);
	s->height = sweet_dict_u32(props, "IOSurfaceHeight", 0);
	s->bytesPerElement = (uint16_t)sweet_dict_u32(props, "IOSurfaceBytesPerElement", 4);
	s->elementWidth = (uint8_t)sweet_dict_u32(props, "IOSurfaceElementWidth", 1);
	s->elementHeight = (uint8_t)sweet_dict_u32(props, "IOSurfaceElementHeight", 1);
	s->pixelFormat = sweet_dict_u32(props, "IOSurfacePixelFormat", 0);
	s->cacheMode = sweet_dict_u32(props, "IOSurfaceCacheMode", 0);
	s->offset = sweet_dict_u32(props, "IOSurfaceOffset", 0);
	if (s->width > SWEET_SURFACE_MAX_DIM || s->height > SWEET_SURFACE_MAX_DIM ||
	    s->bytesPerElement > 64 || s->elementWidth == 0 || s->elementHeight == 0) {
		return kIOReturnBadArgument;
	}
	s->bytesPerRow = sweet_dict_u32(props, "IOSurfaceBytesPerRow",
	    sweet_row_bytes(s->width, s->elementWidth, s->bytesPerElement));
	if (s->bytesPerRow < (uint64_t)sweet_div_up(s->width, s->elementWidth) * s->bytesPerElement) {
		return kIOReturnBadArgument;
	}
	end = (uint64_t)s->offset + (uint64_t)s->bytesPerRow * sweet_div_up(s->height, s->elementHeight);

	OSArray *planes = OSDynamicCast(OSArray, props->getObject("IOSurfacePlaneInfo"));
	s->planeCount = 0;
	if (planes != NULL) {
		uint64_t next = s->offset;
		if (planes->getCount() > SWEET_SURFACE_MAX_PLANES) {
			return kIOReturnBadArgument;
		}
		for (unsigned int i = 0; i < planes->getCount(); i++) {
			OSDictionary *pd = OSDynamicCast(OSDictionary, planes->getObject(i));
			struct sweet_surface_plane *p = &s->planes[i];
			if (pd == NULL) {
				return kIOReturnBadArgument;
			}
			p->width = sweet_dict_u32(pd, "IOSurfacePlaneWidth", s->width);
			p->height = sweet_dict_u32(pd, "IOSurfacePlaneHeight", s->height);
			p->bytesPerElement = (uint16_t)sweet_dict_u32(pd, "IOSurfacePlaneBytesPerElement", 1);
			p->elementWidth = (uint8_t)sweet_dict_u32(pd, "IOSurfacePlaneElementWidth", 1);
			p->elementHeight = (uint8_t)sweet_dict_u32(pd, "IOSurfacePlaneElementHeight", 1);
			if (p->width > SWEET_SURFACE_MAX_DIM || p->height > SWEET_SURFACE_MAX_DIM ||
			    p->bytesPerElement > 64 || p->elementWidth == 0 || p->elementHeight == 0) {
				return kIOReturnBadArgument;
			}
			p->bytesPerRow = sweet_dict_u32(pd, "IOSurfacePlaneBytesPerRow",
			    sweet_row_bytes(p->width, p->elementWidth, p->bytesPerElement));
			p->offset = sweet_dict_u32(pd, "IOSurfacePlaneOffset", (uint32_t)next);
			p->size = sweet_dict_u32(pd, "IOSurfacePlaneSize",
			    p->bytesPerRow * sweet_div_up(p->height, p->elementHeight));
			next = round_page((uint64_t)p->offset + p->size);
			if ((uint64_t)p->offset + p->size > end) {
				end = (uint64_t)p->offset + p->size;
			}
		}
		s->planeCount = planes->getCount();
	}

	uint64_t alloc = sweet_dict_u32(props, "IOSurfaceAllocSize", 0);
	if (alloc < end) {
		alloc = end;
	}
	alloc = round_page(alloc);
	if (alloc == 0 || alloc > SWEET_SURFACE_MAX_ALLOC) {
		return kIOReturnNoMemory;
	}
	s->allocSize = (uint32_t)alloc;
	return kIOReturnSuccess;
}

/* Per-client mapping of one surface. */
class SweetSurfaceRef : public OSObject {
	OSDeclareDefaultStructors(SweetSurfaceRef);

public:
	SweetSurface *surface;
	IOMemoryMap  *data;
	IOMemoryMap  *state;
	uint32_t      count;
	bool          detached;     /* trap 4 seen: the framework is releasing this view */
	bool          released;     /* released: only its state page stays mapped (fZombies) */

	bool init() APPLE_KEXT_OVERRIDE { detached = false; released = false; return OSObject::init(); }
	void free() APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetSurfaceRef, OSObject);

void
SweetSurfaceRef::free()
{
	OSSafeReleaseNULL(data);
	OSSafeReleaseNULL(state);
	if (surface != NULL) {
		if (!released) {
			sweet_surface_put(surface);
		}
		surface->release();
		surface = NULL;
	}
	OSObject::free();
}

class IOCoreSurfaceRoot : public IOService {
	OSDeclareDefaultStructors(IOCoreSurfaceRoot);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOCoreSurfaceRoot, IOService);

bool
IOCoreSurfaceRoot::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	sweet_surface_init_lock();
	setProperty(kIOUserClientClassKey, "IOSurfaceRootUserClient");
	registerService();
	IOLog("SWEET SURF: IOCoreSurfaceRoot registered\n");
	return true;
}

class IOSurfaceRootUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOSurfaceRootUserClient);

	task_t   fTask;
	IOLock  *fLock;
	OSArray *fRefs;
	OSArray *fZombies;      /* released views whose state page is still mapped, oldest first */

	SweetSurfaceRef *findRef(uint32_t id, unsigned int *index, bool detached = false);
	IOReturn addRef(SweetSurface *s, SweetSurfaceRef **refOut, bool *existing);
	IOReturn returnSurface(SweetSurface *s, IOExternalMethodArguments *args);
	IOReturn create(IOExternalMethodArguments *args);
	IOReturn wrapClient(uint32_t selector, IOExternalMethodArguments *args);
	IOReturn valueCall(uint32_t selector, IOExternalMethodArguments *args);
	IOReturn bulkAttachments(IOExternalMethodArguments *args);

public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	void free() APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	IOExternalTrap *getTargetAndTrapForIndex(IOService **targetP, UInt32 index) APPLE_KEXT_OVERRIDE;

	IOReturn trapUseCount(void *id, void *category, void *, void *, void *, void *);
	IOReturn trapLock(void *id, void *options, void *seedOut, void *, void *, void *);
	IOReturn trapUnlock(void *id, void *options, void *seedOut, void *, void *, void *);
	IOReturn trapRelease(void *id, void *, void *, void *, void *, void *);
	IOReturn trapDetach(void *id, void *, void *, void *, void *, void *);
};

OSDefineMetaClassAndStructors(IOSurfaceRootUserClient, IOUserClient);

static uint64_t sweet_surf_logged;      /* selectors / traps already logged (< 64) */

static void
sweet_surf_log_once(const char *what, uint32_t n)
{
	uint32_t bit = (what[0] == 't' ? 48 : 0) + n;
	if (bit >= 64 || (sweet_surf_logged & (1ULL << bit))) {
		return;
	}
	sweet_surf_logged |= 1ULL << bit;
	char name[32];
	proc_selfname(name, sizeof(name));
	IOLog("SWEET SURF: %s %s %u\n", name, what, n);
}

bool
IOSurfaceRootUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	fLock = IOLockAlloc();
	fRefs = OSArray::withCapacity(16);
	fZombies = OSArray::withCapacity(16);
	return fLock != NULL && fRefs != NULL;
}

void
IOSurfaceRootUserClient::free()
{
	OSSafeReleaseNULL(fRefs);
	OSSafeReleaseNULL(fZombies);
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOSurfaceRootUserClient::clientClose(void)
{
	IOLockLock(fLock);
	fRefs->flushCollection();
	if (fZombies != NULL) {
		fZombies->flushCollection();
	}
	IOLockUnlock(fLock);
	terminate();
	return kIOReturnSuccess;
}

/* Lock held. */
SweetSurfaceRef *
IOSurfaceRootUserClient::findRef(uint32_t id, unsigned int *index, bool detached)
{
	for (unsigned int i = 0; i < fRefs->getCount(); i++) {
		SweetSurfaceRef *ref = (SweetSurfaceRef *)fRefs->getObject(i);
		if (ref->surface->id == id && ref->detached == detached) {
			if (index != NULL) {
				*index = i;
			}
			return ref;
		}
	}
	return NULL;
}

/*
 * Count one more create/lookup of s by this client, mapping it into the
 * task the first time. Takes over the caller's user count on s.
 *
 * A lookup of a surface this client already has maps nothing and counts
 * nothing (*existing): IOSurface.framework then gets base address -1 in the
 * lock result and reuses its cached IOSurfaceClient. Counting it instead
 * made the framework build a second client for the same id; releasing that
 * one (traps 4 + 5) unmapped the memory under the first (backboardd
 * snapshot crash in CGDataProviderCreateWithData).
 */
IOReturn
IOSurfaceRootUserClient::addRef(SweetSurface *s, SweetSurfaceRef **refOut, bool *existing)
{
	*existing = false;
	IOLockLock(fLock);
	SweetSurfaceRef *ref = findRef(s->id, NULL);
	if (ref != NULL && ref->surface == s) {
		IOLockUnlock(fLock);
		sweet_surface_put(s);
		*refOut = ref;
		*existing = true;
		return kIOReturnSuccess;
	}
	IOLockUnlock(fLock);

	ref = OSTypeAlloc(SweetSurfaceRef);
	if (ref == NULL || !ref->init()) {
		OSSafeReleaseNULL(ref);
		sweet_surface_put(s);
		return kIOReturnNoMemory;
	}
	s->retain();
	ref->surface = s;
	ref->count = 1;
	ref->data = s->memory->createMappingInTask(fTask, 0, kIOMapAnywhere | s->mapCache);
	ref->state = s->state->createMappingInTask(fTask, 0, kIOMapAnywhere | kIOMapReadOnly);
	if (ref->data == NULL || ref->state == NULL) {
		ref->release();
		return kIOReturnVMError;
	}
	IOLockLock(fLock);
	SweetSurfaceRef *raced = findRef(s->id, NULL);
	if (raced != NULL && raced->surface == s) {
		/* Another thread mapped it meanwhile: count there, drop ours. */
		raced->count++;
		IOLockUnlock(fLock);
		ref->release();
		*refOut = raced;
		return kIOReturnSuccess;
	}
	fRefs->setObject(ref);
	IOLockUnlock(fLock);
	ref->release();         /* fRefs holds it */
	*refOut = ref;
	return kIOReturnSuccess;
}

static void
put32(uint8_t *p, uint32_t off, uint32_t v)
{
	memcpy(p + off, &v, 4);
}

static void
put64(uint8_t *p, uint32_t off, uint64_t v)
{
	memcpy(p + off, &v, 8);
}

/* IOSurface.framework's lock result (copied to IOSurfaceClient + 0x68). */
static void
sweet_surface_fill_result(SweetSurface *s, SweetSurfaceRef *ref, uint8_t *out)
{
	bzero(out, SWEET_SURFACE_RESULT_SIZE);
	put64(out, 0x00, ref->data->getAddress() + s->wrapDelta);
	put64(out, 0x08, ref->state->getAddress());
	put64(out, 0x10, ref->state->getAddress() + SWEET_SURFACE_TIMESTAMPS);
	put32(out, 0x18, s->id);
	put32(out, 0x1c, s->allocSize);
	put32(out, 0x20, s->width);
	put32(out, 0x24, s->height);
	put32(out, 0x28, s->bytesPerRow);
	put32(out, 0x2c, s->offset);
	put32(out, 0x30, s->pixelFormat);
	put32(out, 0x34, s->allocSize);
	put32(out, 0x38, s->planeCount);
	memcpy(out + 0x40, &s->bytesPerElement, 2);
	out[0x42] = s->elementWidth;
	out[0x43] = s->elementHeight;
	put32(out, 0x44, s->cacheMode);
	put32(out, 0x50, s->parentID);
	put32(out, 0x54, 1);
	for (uint32_t i = 0; i < s->planeCount; i++) {
		uint8_t *p = out + 0x68 + i * 0x80;
		const struct sweet_surface_plane *pl = &s->planes[i];
		put32(p, 0x00, pl->width);
		put32(p, 0x04, pl->height);
		put32(p, 0x08, pl->bytesPerRow);
		put32(p, 0x0c, pl->offset);
		put32(p, 0x14, pl->size);
		memcpy(p + 0x18, &pl->bytesPerElement, 2);
		p[0x1a] = pl->elementWidth;
		p[0x1b] = pl->elementHeight;
	}
}

static IOReturn
sweet_struct_out(IOExternalMethodArguments *args, const void *buf, uint32_t len)
{
	if (args->structureOutputDescriptor != NULL) {
		if (len > args->structureOutputDescriptor->getLength()) {
			return kIOReturnNoSpace;
		}
		args->structureOutputDescriptor->writeBytes(0, buf, len);
		args->structureOutputDescriptorSize = len;
		return kIOReturnSuccess;
	}
	if (args->structureOutput == NULL || len > args->structureOutputSize) {
		return kIOReturnNoSpace;
	}
	memcpy(args->structureOutput, buf, len);
	args->structureOutputSize = len;
	return kIOReturnSuccess;
}

/* Map s for this client and answer with its lock result. Takes s's user count. */
IOReturn
IOSurfaceRootUserClient::returnSurface(SweetSurface *s, IOExternalMethodArguments *args)
{
	SweetSurfaceRef *ref = NULL;
	bool existing = false;
	IOReturn ret = addRef(s, &ref, &existing);
	if (ret != kIOReturnSuccess) {
		return ret;
	}
	uint8_t *out = (uint8_t *)IOMallocZeroData(SWEET_SURFACE_RESULT_SIZE);
	if (out == NULL) {
		ret = kIOReturnNoMemory;
	} else {
		sweet_surface_fill_result(s, ref, out);
		if (existing) {
			put64(out, 0x00, ~0ULL);        /* "already yours": use the cached client */
		}
		ret = sweet_struct_out(args, out, SWEET_SURFACE_RESULT_SIZE);
		IOFreeData(out, SWEET_SURFACE_RESULT_SIZE);
	}
	if (ret != kIOReturnSuccess && !existing) {
		/* The caller never learns the id: undo this create/lookup. */
		trapRelease((void *)(uintptr_t)s->id, NULL, NULL, NULL, NULL, NULL);
	}
	return ret;
}

IOReturn
IOSurfaceRootUserClient::create(IOExternalMethodArguments *args)
{
	const char *buf = (const char *)args->structureInput;
	uint32_t len = args->structureInputSize;
	char *copy = NULL;
	IOReturn ret;

	if (args->structureInputDescriptor != NULL) {
		len = (uint32_t)args->structureInputDescriptor->getLength();
		if (len == 0 || len > SWEET_SURFACE_MAX_PROPS) {
			return kIOReturnBadArgument;
		}
		copy = (char *)IOMallocData(len);
		if (copy == NULL) {
			return kIOReturnNoMemory;
		}
		args->structureInputDescriptor->readBytes(0, copy, len);
		buf = copy;
	}
	OSObject *obj = (buf != NULL && len > 0) ? OSUnserializeXML(buf, len) : NULL;
	if (copy != NULL) {
		IOFreeData(copy, len);
	}
	OSDictionary *props = OSDynamicCast(OSDictionary, obj);
	if (props == NULL) {
		OSSafeReleaseNULL(obj);
		IOLog("SWEET SURF: create: bad property dictionary (%u bytes)\n", len);
		return kIOReturnBadArgument;
	}

	SweetSurface *s = sweet_surface_alloc();
	if (s == NULL) {
		props->release();
		return kIOReturnNoMemory;
	}
	s->parentID = args->scalarInputCount > 0 ? (uint32_t)args->scalarInput[0] : 0;
	ret = sweet_surface_layout(s, props);
	/*
	 * IOSurfaceAddress: a surface around memory the creator has already.
	 * CoreAnimation's GL renderer makes its pictures (applications' icons,
	 * layers' contents) into surfaces so; given fresh memory instead, every
	 * icon was a transparent square.
	 */
	OSNumber *client_address = OSDynamicCast(OSNumber, props->getObject("IOSurfaceAddress"));
	if (ret == kIOReturnSuccess && client_address != NULL) {
		uint64_t used = (uint64_t)s->offset + (uint64_t)s->bytesPerRow * sweet_div_up(s->height, s->elementHeight);
		for (uint32_t i = 0; i < s->planeCount; i++) {
			if ((uint64_t)s->planes[i].offset + s->planes[i].size > used) {
				used = (uint64_t)s->planes[i].offset + s->planes[i].size;
			}
		}
		/* the memory is as big as its owner says (IOSurfaceAllocSize), not rounded up by us */
		uint64_t owned = sweet_dict_u32(props, "IOSurfaceAllocSize", 0);
		if (owned != 0) {
			used = owned;
		}
		ret = sweet_surface_use_task_memory(s, fTask, client_address->unsigned64BitValue(), used);
		/*
		 * Published here: the two branches below only run for surfaces that still need
		 * memory. Unpublished, every such surface had id 0, and the framework took them
		 * all for one surface: releasing any of them unmapped the views of the others.
		 */
		if (ret == kIOReturnSuccess && !sweet_surface_publish(s)) {
			ret = kIOReturnNoResources;
		}
		static uint32_t told;
		if (told++ < 6) {
			IOLog("SWEET SURF: create around client memory %#llx (%llu bytes) %ux%u fmt 0x%x -> 0x%x\n",
			    client_address->unsigned64BitValue(), used, s->width, s->height, s->pixelFormat, ret);
		}
	}
	if (ret == kIOReturnSuccess && s->memory == NULL && sweet_scanout_eligible(s)) {
		int slot = sweet_scanout_alloc_slot();
		if (slot >= 0) {
			/*
			 * Real RAM, physically contiguous and below 4 GiB (the display SMMU is
			 * bypassed). A raw physical window outside the kernel's RAM maps as
			 * Device memory, where bzero/memcpy take alignment faults.
			 */
			IOBufferMemoryDescriptor *cmem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
			    kIODirectionInOut | kIOMemoryKernelUserShared | kIOMemoryPhysicallyContiguous,
			    s->allocSize, 0x00000000fffff000ull);
			uint64_t pa = cmem != NULL ? cmem->getPhysicalSegment(0, NULL, kIOMemoryMapperNone) : 0;
			if (cmem != NULL && (pa == 0 || pa + s->allocSize > 0x100000000ull)) {
				cmem->release();
				cmem = NULL;
			}
			if (cmem != NULL) {
				OSAddAtomic64(s->allocSize, (volatile SInt64 *)&sweet_surface_wired);
			}
			s->memory = cmem;
			if (s->memory != NULL) {
				s->scanoutPhys = pa;
				s->scanoutSlot = slot;
				s->mapCache = kIOMapDefaultCache;
				uintptr_t va = s->kernelAddress();
				if (va != 0) {
					bzero((void *)va, s->allocSize);
				}
				if (!sweet_surface_publish(s)) {
					ret = kIOReturnNoResources;
				}
			} else {
				sweet_scanout_free_slot(slot);
			}
		}
	}
	if (ret == kIOReturnSuccess && s->memory == NULL) {
		IOBufferMemoryDescriptor *mem = NULL;
		if ((uint64_t)OSAddAtomic64(s->allocSize, (volatile SInt64 *)&sweet_surface_wired) +
		    s->allocSize <= SWEET_SURFACE_WIRED_MAX) {
			mem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
			    kIODirectionInOut | kIOMemoryKernelUserShared, s->allocSize, page_size);
		}
		if (mem == NULL) {
			OSAddAtomic64(-(SInt64)s->allocSize, (volatile SInt64 *)&sweet_surface_wired);
			ret = kIOReturnNoMemory;
		} else {
			bzero(mem->getBytesNoCopy(), s->allocSize);
			s->memory = mem;
			s->mapCache = kIOMapDefaultCache;
			if (!sweet_surface_publish(s)) {
				ret = kIOReturnNoResources;
			}
		}
	}
	if (ret != kIOReturnSuccess || sweet_surface_creates_logged < 64) {
		char name[32];
		proc_selfname(name, sizeof(name));
		sweet_surface_creates_logged++;
		IOLog("SWEET SURF: %s create id %u %ux%u fmt 0x%x bpe %u bpr %u planes %u size %u keys %u -> 0x%x\n",
		    name, s->id, s->width, s->height, s->pixelFormat, s->bytesPerElement,
		    s->bytesPerRow, s->planeCount, s->allocSize, props->getCount(), ret);
	}
	props->release();
	if (ret != kIOReturnSuccess) {
		s->release();
		return ret;
	}
	ret = returnSurface(s, args);   /* takes the creator's user count */
	s->release();
	return ret;
}

/*
 * Make s's pixels the memory a task has already at address (size bytes): the
 * range is prepared (wired) for as long as the surface lives, and every task
 * that looks the surface up maps those same pages.
 */
static IOReturn
sweet_surface_use_task_memory(SweetSurface *s, task_t task, uint64_t address, uint64_t size)
{
	if (address == 0 || size == 0 || size > SWEET_SURFACE_MAX_ALLOC) {
		return kIOReturnBadArgument;
	}
	uint64_t first = trunc_page_64(address);
	uint64_t length = round_page_64(address + size) - first;
	IOMemoryDescriptor *md = NULL;
	IOReturn held = kIOReturnNoMemory;
	/* writable if it can be; a picture that is only shown may be read-only memory */
	for (int attempt = 0; attempt < 2 && held != kIOReturnSuccess; attempt++) {
		OSSafeReleaseNULL(md);
		md = IOMemoryDescriptor::withAddressRange(first, length, attempt == 0 ? kIODirectionInOut : kIODirectionOut, task);
		held = md != NULL ? md->prepare() : kIOReturnNoMemory;
	}
	if (held != kIOReturnSuccess) {
		OSSafeReleaseNULL(md);
		return kIOReturnVMError;
	}
	s->memory = md;
	s->wrapped = true;
	s->wrapDelta = (uint32_t)(address - first);
	s->mapCache = kIOMapDefaultCache;
	return kIOReturnSuccess;
}

/* The structure input of a call, wherever it came (inline or a descriptor), copied. Free with IOFreeData. */
static uint8_t *
sweet_struct_in(IOExternalMethodArguments *args, uint32_t *len)
{
	uint8_t *copy;

	if (args->structureInputDescriptor != NULL) {
		*len = (uint32_t)args->structureInputDescriptor->getLength();
	} else {
		*len = args->structureInputSize;
	}
	if (*len == 0 || *len > SWEET_SURFACE_MAX_PROPS || (copy = (uint8_t *)IOMallocData(*len)) == NULL) {
		return NULL;
	}
	if (args->structureInputDescriptor != NULL) {
		args->structureInputDescriptor->readBytes(0, copy, *len);
	} else {
		memcpy(copy, args->structureInput, *len);
	}
	return copy;
}

static uint32_t *
sweet_state32(SweetSurface *s, uint32_t offset)
{
	return (uint32_t *)((uint8_t *)s->state->getBytesNoCopy() + offset);
}

/* After a change of the values: a new (odd) seed and the room a fetch of all of them needs. Lock held. */
static void
sweet_values_changed(SweetSurface *s)
{
	uint32_t size = 0;

	if (s->values != NULL && s->values->getCount() != 0) {
		OSSerialize *ser = OSSerialize::binaryWithCapacity(4096);
		if (ser != NULL && s->values->serialize(ser)) {
			size = ser->getLength() + 8;
		}
		OSSafeReleaseNULL(ser);
	}
	*sweet_state32(s, SWEET_SURFACE_STATE_VSEED) = (*sweet_state32(s, SWEET_SURFACE_STATE_VSEED) + 2) | 1;
	*sweet_state32(s, SWEET_SURFACE_STATE_VSIZE) = size;
}

static IOReturn
sweet_reply_seed(IOExternalMethodArguments *args, uint32_t seed)
{
	if (args->structureOutput != NULL && args->structureOutputSize >= sizeof(seed)) {
		memcpy(args->structureOutput, &seed, sizeof(seed));
		args->structureOutputSize = sizeof(seed);
	}
	return kIOReturnSuccess;
}

/*
 * Values. Requests start with the surface id (8 bytes). SetValue: then a
 * serialized array, [value, key] or [dictionary of them all]. RemoveValue and
 * CopyValue: then a key, NUL-terminated (empty: all). Changes answer the new
 * seed; CopyValue answers 4 unused bytes, the seed, the serialized value.
 */
IOReturn
IOSurfaceRootUserClient::valueCall(uint32_t selector, IOExternalMethodArguments *args)
{
	uint32_t len = 0, id, seed = 0;
	uint8_t *in = sweet_struct_in(args, &len);
	IOReturn ret = kIOReturnBadArgument;
	SweetSurface *s;
	OSSerialize *ser = NULL;
	uint32_t length = 0;

	if (in == NULL) {
		return kIOReturnBadArgument;
	}
	if (len < 9 || (selector != kSweetSurfaceSetValue && in[len - 1] != 0)) {
		IOFreeData(in, len);
		return kIOReturnBadArgument;
	}
	memcpy(&id, in, sizeof(id));
	s = sweet_surface_copy(id);
	if (s == NULL) {
		IOFreeData(in, len);
		return kIOReturnNotFound;
	}
	OSObject *obj = selector == kSweetSurfaceSetValue ? OSUnserializeXML((const char *)in + 8, len - 8) : NULL;
	IOLockLock(sweet_surface_lock);
	if (s->values == NULL) {
		s->values = OSDictionary::withCapacity(4);
	}
	if (s->values == NULL) {
		ret = kIOReturnNoMemory;
	} else if (selector == kSweetSurfaceSetValue) {
		OSArray *array = OSDynamicCast(OSArray, obj);
		OSString *key = array != NULL ? OSDynamicCast(OSString, array->getObject(1)) : NULL;
		OSDictionary *all = array != NULL ? OSDynamicCast(OSDictionary, array->getObject(0)) : NULL;
		if (array != NULL && array->getCount() == 2 && key != NULL && array->getObject(0) != NULL) {
			s->values->setObject(key, array->getObject(0));
			ret = kIOReturnSuccess;
		} else if (array != NULL && array->getCount() == 1 && all != NULL) {
			s->values->merge(all);
			ret = kIOReturnSuccess;
		}
		if (ret == kIOReturnSuccess) {
			sweet_values_changed(s);
		}
	} else if (selector == kSweetSurfaceRemoveValue) {
		const char *key = (const char *)in + 8;
		if (key[0] == 0) {
			s->values->flushCollection();
		} else {
			s->values->removeObject(key);
		}
		sweet_values_changed(s);
		ret = kIOReturnSuccess;
	} else {
		const char *key = (const char *)in + 8;
		OSObject *value = key[0] != 0 ? s->values->getObject(key) : s->values;
		ret = kIOReturnSuccess;
		if (value != NULL && (key[0] != 0 || s->values->getCount() != 0)) {
			ser = OSSerialize::binaryWithCapacity(4096);
			if (ser == NULL || !value->serialize(ser)) {
				ret = kIOReturnNoMemory;
			} else {
				length = ser->getLength();
			}
		}
	}
	seed = *sweet_state32(s, SWEET_SURFACE_STATE_VSEED);
	IOLockUnlock(sweet_surface_lock);
	OSSafeReleaseNULL(obj);
	IOFreeData(in, len);

	if (ret == kIOReturnSuccess && selector == kSweetSurfaceCopyValue) {
		uint32_t header[2] = { 0, seed }, total = (uint32_t)sizeof(header) + length;
		if (args->structureOutputDescriptor != NULL) {
			IOMemoryDescriptor *out = args->structureOutputDescriptor;
			if (out->getLength() < total) {
				ret = kIOReturnNoSpace;
			} else if ((ret = out->prepare()) == kIOReturnSuccess) {
				out->writeBytes(0, header, sizeof(header));
				if (length != 0) {
					out->writeBytes(sizeof(header), ser->text(), length);
				}
				out->complete();
				args->structureOutputDescriptorSize = total;
			}
		} else if (args->structureOutput == NULL || args->structureOutputSize < total) {
			ret = kIOReturnNoSpace;
		} else {
			memcpy(args->structureOutput, header, sizeof(header));
			if (length != 0) {
				memcpy((uint8_t *)args->structureOutput + sizeof(header), ser->text(), length);
			}
			args->structureOutputSize = total;
		}
	} else if (ret == kIOReturnSuccess) {
		sweet_reply_seed(args, seed);
	}
	OSSafeReleaseNULL(ser);
	s->release();
	return ret;
}

/*
 * Bulk attachments (colour space, transfer function, ...): 0x70 bytes, then at
 * +0x70 their size, +0x78 a mask of the fields that changed, +0x80 the surface
 * id. The fields, by their bit, are these byte ranges of the 0x70.
 */
IOReturn
IOSurfaceRootUserClient::bulkAttachments(IOExternalMethodArguments *args)
{
	static const uint8_t field[15][2] = {
		{ 0x00, 0x20 }, { 0x20, 0x30 }, { 0x30, 0x38 }, { 0x38, 0x39 }, { 0x39, 0x3a }, { 0x3a, 0x3b }, { 0x3b, 0x3c },
		{ 0x3c, 0x3d }, { 0x3d, 0x3e }, { 0x3e, 0x3f }, { 0x3f, 0x40 }, { 0x40, 0x58 }, { 0x58, 0x5c }, { 0x5c, 0x64 },
		{ 0x64, 0x65 },
	};
	uint32_t len = 0, id;
	uint64_t size, mask;
	uint8_t *in = sweet_struct_in(args, &len);
	SweetSurface *s;

	if (in == NULL || len < 0x84) {
		if (in != NULL) {
			IOFreeData(in, len);
		}
		return kIOReturnBadArgument;
	}
	memcpy(&size, in + 0x70, 8);
	memcpy(&mask, in + 0x78, 8);
	memcpy(&id, in + 0x80, 4);
	if (size > SWEET_SURFACE_BULK_SIZE || (s = sweet_surface_copy(id)) == NULL) {
		IOFreeData(in, len);
		return size > SWEET_SURFACE_BULK_SIZE ? kIOReturnBadArgument : kIOReturnNotFound;
	}
	uint8_t *bulk = (uint8_t *)s->state->getBytesNoCopy() + SWEET_SURFACE_STATE_BULK;
	IOLockLock(sweet_surface_lock);
	for (unsigned i = 0; i < 15; i++) {
		if ((mask & (1ull << i)) && field[i][1] <= size) {
			memcpy(bulk + field[i][0], in + field[i][0], field[i][1] - field[i][0]);
		}
	}
	IOLockUnlock(sweet_surface_lock);
	IOFreeData(in, len);
	s->release();
	return kIOReturnSuccess;
}

/*
 * IOSurfaceWrapClientImage (6: struct of 0x20 bytes: +0 address, +8 width,
 * +0xc height, +0x10 pixel format, +0x14 bytes per element, +0x18 bytes per
 * row) and IOSurfaceWrapClientMemory (7: scalars address, size): a surface
 * made of the caller's own memory (CoreAnimation's GL renderer wraps the
 * shared memory of images, icons and wallpapers so). The range is prepared
 * (wired) for as long as the surface lives; every task maps it as usual.
 */
IOReturn
IOSurfaceRootUserClient::wrapClient(uint32_t selector, IOExternalMethodArguments *args)
{
	uint64_t address, size;
	uint32_t width, height, format = 0, bpe, bpr;

	if (selector == kSweetSurfaceWrapClientImage) {
		const uint8_t *in = (const uint8_t *)args->structureInput;
		if (in == NULL || args->structureInputSize < 0x1c) {
			return kIOReturnBadArgument;
		}
		memcpy(&address, in, 8);
		memcpy(&width, in + 0x08, 4);
		memcpy(&height, in + 0x0c, 4);
		memcpy(&format, in + 0x10, 4);
		memcpy(&bpe, in + 0x14, 4);
		memcpy(&bpr, in + 0x18, 4);
		if (width == 0 || height == 0 || bpe == 0 || bpe > 16 || bpr < (uint64_t)width * bpe) {
			static uint32_t told;
			if (told++ < 6) {
				uint32_t w[8];
				memcpy(w, in, sizeof(w) <= args->structureInputSize ? sizeof(w) : args->structureInputSize);
				IOLog("SWEET SURF: wrap image refused: %08x %08x | %08x %08x %08x %08x %08x %08x (%u bytes)\n",
				    w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], args->structureInputSize);
			}
			return kIOReturnBadArgument;
		}
		size = (uint64_t)bpr * height;
	} else {
		if (args->scalarInputCount < 2) {
			return kIOReturnBadArgument;
		}
		address = args->scalarInput[0];
		size = args->scalarInput[1];
		width = (uint32_t)size;
		height = 1;
		bpe = 1;
		bpr = (uint32_t)size;
	}
	if (address == 0 || size == 0 || size > (256ull << 20)) {
		return kIOReturnBadArgument;
	}
	SweetSurface *s = sweet_surface_alloc();
	if (s == NULL) {
		return kIOReturnNoMemory;
	}
	IOReturn wret = sweet_surface_use_task_memory(s, fTask, address, size);
	if (wret != kIOReturnSuccess) {
		s->release();
		return wret;
	}
	s->width = width;
	s->height = height;
	s->pixelFormat = format;
	s->bytesPerElement = (uint16_t)bpe;
	s->elementWidth = 1;
	s->elementHeight = 1;
	s->bytesPerRow = bpr;
	s->allocSize = (uint32_t)size;
	s->planeCount = 0;
	if (!sweet_surface_publish(s)) {
		s->release();
		return kIOReturnNoResources;
	}
	static uint32_t logged;
	if (logged < 8) {
		logged++;
		IOLog("SWEET SURF: wrap %s id %u %ux%u fmt 0x%x bpe %u bpr %u size %llu at +0x%x\n",
		    selector == kSweetSurfaceWrapClientImage ? "image" : "memory", s->id, width, height, format, bpe, bpr,
		    size, s->wrapDelta);
	}
	IOReturn ret = returnSurface(s, args);  /* takes the creator's user count */
	s->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	uint64_t arg0 = args->scalarInputCount > 0 ? args->scalarInput[0] : 0;

	switch (selector) {
	case kSweetSurfaceLimits: {
		uint8_t limits[0x18] = { 0 };
		put32(limits, 0x0, SWEET_SURFACE_ROW_ALIGN - 1);
		put32(limits, 0x4, SWEET_SURFACE_ROW_ALIGN - 1);
		limits[0x16] = 1;
		sweet_surf_log_once("sel", selector);
		return sweet_struct_out(args, limits, sizeof(limits));
	}
	case kSweetSurfaceCreate:
		return create(args);
	case kSweetSurfaceSetValue:
	case kSweetSurfaceCopyValue:
	case kSweetSurfaceRemoveValue:
		sweet_surf_log_once("sel", selector);
		return valueCall(selector, args);
	case kSweetSurfaceSetBulkAttachments:
		sweet_surf_log_once("sel", selector);
		return bulkAttachments(args);
	case kSweetSurfaceSetPurgeable:
	case kSweetSurfaceBindAccel:
	case kSweetSurfaceSetYCbCrMatrix: {
		/* in: surface, state (out: the old one) / surface, accelerator, sub id / surface, matrix */
		SweetSurface *s = args->scalarInputCount >= 2 ? sweet_surface_copy((uint32_t)arg0) : NULL;
		sweet_surf_log_once("sel", selector);
		if (s == NULL) {
			return args->scalarInputCount >= 2 ? kIOReturnNotFound : kIOReturnBadArgument;
		}
		IOLockLock(sweet_surface_lock);
		if (selector == kSweetSurfaceSetPurgeable) {
			if (args->scalarOutputCount >= 1) {
				args->scalarOutput[0] = s->purgeable;
				args->scalarOutputCount = 1;
			}
			if (args->scalarInput[1] != 3) {        /* 3: only ask */
				s->purgeable = (uint32_t)args->scalarInput[1];
			}
		} else if (selector == kSweetSurfaceBindAccel && args->scalarInputCount >= 3) {
			uint16_t accel[2] = { (uint16_t)args->scalarInput[1], (uint16_t)args->scalarInput[2] };
			memcpy((uint8_t *)s->state->getBytesNoCopy() + SWEET_SURFACE_STATE_ACCEL, accel, sizeof(accel));
		} else if (selector == kSweetSurfaceSetYCbCrMatrix) {
			*sweet_state32(s, SWEET_SURFACE_STATE_YCBCR) = (uint32_t)args->scalarInput[1];
		}
		IOLockUnlock(sweet_surface_lock);
		s->release();
		return kIOReturnSuccess;
	}
	case kSweetSurfaceWrapClientImage:
	case kSweetSurfaceWrapClientMemory:
		sweet_surf_log_once("sel", selector);
		return wrapClient(selector, args);
	case kSweetSurfaceLookup: {
		SweetSurface *s = sweet_surface_copy((uint32_t)arg0);
		IOReturn ret = kIOReturnNotFound;
		if (s != NULL) {
			ret = sweet_surface_get(s) ? returnSurface(s, args) : kIOReturnNotFound;
			s->release();
		}
		sweet_surf_log_once("sel", selector);
		return ret;
	}
	case kSweetSurfaceLookupFromPort: {
		OSObject *obj = NULL;
		IOReturn ret = IOUserClient::copyObjectForPortNameInTask(fTask, (mach_port_name_t)arg0, &obj);
		SweetSurface *s = OSDynamicCast(SweetSurface, obj);
		if (ret == kIOReturnSuccess) {
			ret = (s != NULL && sweet_surface_get(s)) ? returnSurface(s, args) : kIOReturnBadArgument;
		}
		OSSafeReleaseNULL(obj);
		sweet_surf_log_once("sel", selector);
		return ret;
	}
	case kSweetSurfaceCreatePort: {
		SweetSurface *s = sweet_surface_copy((uint32_t)arg0);
		if (s == NULL || args->scalarOutputCount < 1) {
			OSSafeReleaseNULL(s);
			return kIOReturnNotFound;
		}
		args->scalarOutput[0] = sweet_surface_port_in_task(s, fTask);
		args->scalarOutputCount = 1;
		s->release();
		sweet_surf_log_once("sel", selector);
		return kIOReturnSuccess;
	}
	default:
		/*
		 * A call that answers with a lock result (wrap client image 6 and
		 * memory 7, child surfaces) would hand IOSurface.framework a client
		 * made of zeroes: no shared state (CoreAnimation's GL renderer reads
		 * its EDR factor there and crashed at 0x30). Refused, the framework
		 * returns NULL and its callers take their other path.
		 */
		if ((args->structureOutputDescriptor != NULL &&
		    args->structureOutputDescriptor->getLength() >= SWEET_SURFACE_RESULT_SIZE) ||
		    (args->structureOutput != NULL && args->structureOutputSize >= SWEET_SURFACE_RESULT_SIZE)) {
			sweet_surf_log_once("sel (refused: lock result)", selector);
			return kIOReturnUnsupported;
		}
		/* Values, notifications, ownership, timestamps...: accept and ignore. */
		for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
			args->scalarOutput[i] = 0;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		sweet_surf_log_once("sel", selector);
		return kIOReturnSuccess;
	}
}

IOReturn
IOSurfaceRootUserClient::trapUseCount(__unused void *id, __unused void *category,
    void *, void *, void *, void *)
{
	return kIOReturnSuccess;
}

static IOReturn
sweet_surface_seed_out(SweetSurface *s, void *seedOut, bool bump)
{
	uint32_t seed = bump ? OSIncrementAtomic((volatile SInt32 *)sweet_surface_seed(s)) + 1 :
	    *sweet_surface_seed(s);
	if (seedOut != NULL && copyout(&seed, (user_addr_t)seedOut, sizeof(seed)) != 0) {
		return kIOReturnBadArgument;
	}
	return kIOReturnSuccess;
}

/* Retained surface this client created or looked up, or NULL. */
static SweetSurface *
sweet_surface_held(IOLock *lock, OSArray *refs, uint32_t id)
{
	SweetSurface *s = NULL;
	IOLockLock(lock);
	for (unsigned int i = 0; i < refs->getCount(); i++) {
		SweetSurfaceRef *ref = (SweetSurfaceRef *)refs->getObject(i);
		if (ref->surface->id == id) {
			s = ref->surface;
			s->retain();
			break;
		}
	}
	IOLockUnlock(lock);
	return s;
}

IOReturn
IOSurfaceRootUserClient::trapLock(void *id, __unused void *options, void *seedOut,
    void *, void *, void *)
{
	SweetSurface *s = sweet_surface_held(fLock, fRefs, (uint32_t)(uintptr_t)id);
	if (s == NULL) {
		return kIOReturnNotFound;
	}
	IOReturn ret = sweet_surface_seed_out(s, seedOut, false);
	s->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::trapUnlock(void *id, void *options, void *seedOut,
    void *, void *, void *)
{
	SweetSurface *s = sweet_surface_held(fLock, fRefs, (uint32_t)(uintptr_t)id);
	if (s == NULL) {
		return kIOReturnNotFound;
	}
	IOReturn ret = sweet_surface_seed_out(s, seedOut,
	    !((uintptr_t)options & kIOSurfaceLockReadOnly));
	s->release();
	return ret;
}

/*
 * The framework drops a surface in two steps: trap 4 under its cache lock,
 * trap 5 after it. A lookup in between must get a view of its own (not "you
 * have it already"): trap 5 then takes the detached view away, not that one.
 * Without this, a lookup racing a release got -1 ("cached") and then lost its
 * state page under it (backboardd: IOSurfaceClientGetSeed on unmapped memory).
 */
IOReturn
IOSurfaceRootUserClient::trapDetach(void *id, void *, void *, void *, void *, void *)
{
	IOLockLock(fLock);
	SweetSurfaceRef *ref = findRef((uint32_t)(uintptr_t)id, NULL);
	if (ref != NULL) {
		ref->detached = true;
	}
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::trapRelease(void *id, void *, void *, void *, void *, void *)
{
	unsigned int index;
	IOLockLock(fLock);
	SweetSurfaceRef *ref = findRef((uint32_t)(uintptr_t)id, &index, true);
	if (ref == NULL) {
		ref = findRef((uint32_t)(uintptr_t)id, &index);
	}
	if (ref != NULL && --ref->count == 0) {
		/*
		 * The pixels are unmapped now; the state page (read-only) stays mapped a while.
		 * In backboardd another IOSurface object can still hold a framework client of
		 * the same surface after this one is released (CoreAnimation and the GL layer
		 * both keep surfaces): reading its seed from an unmapped page crashed
		 * backboardd in IOSurfaceClientGetSeed. The last 256 released views keep
		 * their page (16 KB each), the oldest goes first.
		 */
		OSSafeReleaseNULL(ref->data);
		sweet_surface_put(ref->surface);
		ref->released = true;
		ref->detached = false;
		if (fZombies != NULL && fZombies->setObject(ref)) {
			if (fZombies->getCount() > 256) {
				fZombies->removeObject(0);
			}
		}
		fRefs->removeObject(index);
	}
	IOLockUnlock(fLock);
	return ref != NULL ? kIOReturnSuccess : kIOReturnNotFound;
}

static IOExternalTrap sweet_surface_traps[] = {
	[kSweetSurfaceTrapIncrementUse]  = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapUseCount },
	[kSweetSurfaceTrapDecrementUse]  = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapUseCount },
	[kSweetSurfaceTrapLock]          = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapLock },
	[kSweetSurfaceTrapUnlock]        = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapUnlock },
	/*
	 * IOSurfaceClientRelease calls 4 and then 5 for one release: 4 only says the
	 * client is about to go. Releasing at both dropped a surface's mapping while
	 * another IOSurfaceRef of the task still used it (backboardd crashed in
	 * IOSurfaceClientGetSeed reading the unmapped state page).
	 */
	[kSweetSurfaceTrapRelease]       = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapDetach },
	[kSweetSurfaceTrapReleaseFailed] = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapRelease },
};

IOExternalTrap *
IOSurfaceRootUserClient::getTargetAndTrapForIndex(IOService **targetP, UInt32 index)
{
	if (index >= sizeof(sweet_surface_traps) / sizeof(sweet_surface_traps[0])) {
		sweet_surf_log_once("trap", index);
		return NULL;
	}
	sweet_surf_log_once("trap", index);
	*targetP = this;
	return &sweet_surface_traps[index];
}
#endif /* SWEET */
