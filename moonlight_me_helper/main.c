#include <pspsdk.h>
#include <pspkernel.h>
#include <pspmodulemgr.h>
#include <psploadcore.h>
#include <pspsyscon.h>
#include <string.h>


#define VERS 1
#define REVS 0


PSP_MODULE_INFO("MediaEngine", 0x1006, VERS, REVS);
PSP_MAIN_THREAD_ATTR(0);


struct me_struct
{
	int start;
	int done;
	int (*func)(int);		// function ptr - func takes an int argument and returns int
	int param;				// function argument
	int result;				// function return value
	int precache_len;		// amount of space to invalidate before running func, -1 = all
	void *precache_addr;	// address of space to invalidate before running func
	int postcache_len;		// amount of space to flush after running func, -1 = all
	void *postcache_addr;	// address of space to flush after running func
	unsigned int signals;
	int init;
};


extern void me_stub(void);
extern void me_stub_end(void);

static volatile SceUID s_unload_target = -1;
static volatile int s_unload_requested = 0;

typedef struct {
    SceUID consumer_uid;
    SceLibraryStubTable *stub;
} AvcPatchedImport;

#define AVC_PATCHED_IMPORT_MAX 64
static AvcPatchedImport s_avc_patched_imports[AVC_PATCHED_IMPORT_MAX];
static unsigned int s_avc_patched_import_count;

static int avc_import_was_patched(SceUID consumer_uid,
                                  SceLibraryStubTable *stub)
{
    unsigned int i;
    for (i = 0; i < s_avc_patched_import_count; ++i) {
        if (s_avc_patched_imports[i].consumer_uid == consumer_uid &&
            s_avc_patched_imports[i].stub == stub) return 1;
    }
    return 0;
}

static int avc_remember_patched_import(SceUID consumer_uid,
                                       SceLibraryStubTable *stub)
{
    if (avc_import_was_patched(consumer_uid, stub)) return 0;
    if (s_avc_patched_import_count >= AVC_PATCHED_IMPORT_MAX) return -1;
    s_avc_patched_imports[s_avc_patched_import_count].consumer_uid=consumer_uid;
    s_avc_patched_imports[s_avc_patched_import_count].stub=stub;
    ++s_avc_patched_import_count;
    return 0;
}

/*
 * PSPLink's external modstun path can stop/unload Moonlight, while a
 * self-stop request made from inside the Moonlight PRX can remain blocked in
 * the module manager.  Run the equivalent stop+unload sequence from this
 * already-loaded helper module after Moonlight's main thread has exited.
 */
static int unload_target_thread(SceSize args, void *argp)
{
    SceUID target;
    int status = 0;

    (void)args;
    (void)argp;
    sceKernelDelayThread(50000);
    target = s_unload_target;
    if (target >= 0) {
        int stop_ret = sceKernelStopModule(target, 0, NULL, &status, NULL);
        if (stop_ret >= 0) {
            (void)sceKernelUnloadModule(target);
        }
    }
    s_unload_requested = 0;
    sceKernelExitDeleteThread(0);
    return 0;
}

int RequestModuleUnload(SceUID modid)
{
    SceUID thread_id;
    int start_ret;
    unsigned int k1;

    if (modid < 0 || s_unload_requested) {
        return -1;
    }

    k1 = pspSdkSetK1(0);
    s_unload_target = modid;
    s_unload_requested = 1;
    thread_id = sceKernelCreateThread("me_mod_unload",
                                      unload_target_thread,
                                      0x20,
                                      0x1000,
                                      0,
                                      NULL);
    if (thread_id < 0) {
        s_unload_requested = 0;
        pspSdkSetK1(k1);
        return (int)thread_id;
    }

    start_ret = sceKernelStartThread(thread_id, 0, NULL);
    if (start_ret < 0) {
        s_unload_requested = 0;
        sceKernelDeleteThread(thread_id);
        pspSdkSetK1(k1);
        return start_ret;
    }

    pspSdkSetK1(k1);
    return 0;
}


/*
 * cache functions
 *
 */

#define store_tag(index, hi, lo) __asm__ (".set push\n" \
                 ".set noreorder\n" \
                 "mtc0 %0, $28\n"   \
                 "mtc0 %1, $29\n"   \
                 ".set pop\n"       \
             ::"r"(lo),"r"(hi));    \
             __builtin_allegrex_cache(0x11, index);

void dcache_inv_all()
{
   for(int i = 0; i < 16384; i += 64) {
      store_tag(i, 0, 0);
      __builtin_allegrex_cache(0x13, i);
      __builtin_allegrex_cache(0x11, i);
   }
}

void dcache_inv_range(void *addr, int size)
{
   int i, j =(int)addr;
   for(i = j; i < size+j; i += 64)
      __builtin_allegrex_cache(0x19, i);
}

void dcache_wbinv_all()
{
   for(int i = 0; i < 8192; i += 64)
   {
      __builtin_allegrex_cache(0x14, i);
      __builtin_allegrex_cache(0x14, i);
   }
}

void dcache_wbinv_range(void *addr, int size)
{
   int i, j = (int)addr;
   for(i = j; i < size+j; i += 64)
      __builtin_allegrex_cache(0x1b, i);
}


static void me_loop(volatile struct me_struct *mei)
{
	unsigned int k1;
	k1 = pspSdkSetK1(0);

	while (mei->init) // ME runs this loop until killed
	{
		while (mei->start == 0); // wait for function
		mei->start = 0;
		if (mei->precache_len)
		{
				dcache_inv_all();
		}
		mei->result = mei->func(mei->param); // run function
		if (mei->postcache_len)
		{
				dcache_wbinv_all();
		}
		mei->done = 1;
	}

	pspSdkSetK1(k1);

	while (1); // loop forever until ME reset
}


int InitME(volatile struct me_struct *mei)
{
unsigned int k1;

k1 = pspSdkSetK1(0);

	if (mei == 0)
	{
   		pspSdkSetK1(k1);
   		return -1;
	}

	// initialize the MediaEngine Instance
	mei->start = 0;
	mei->done = 1;
	mei->func = 0;
	mei->param = 0;
	mei->result = 0;
	mei->precache_len = 0;
	mei->precache_addr = 0;
	mei->postcache_len = 0;
	mei->postcache_addr = 0;
	mei->signals = 0;
	mei->init = 1;

	// start the MediaEngine
	memcpy((void *)0xbfc00040, me_stub, (int)(me_stub_end - me_stub));
	_sw((unsigned int)me_loop,  0xbfc00600);	// k0
	_sw((unsigned int)mei, 0xbfc00604);			// a0
	sceKernelDcacheWritebackAll();
	sceSysregMeResetEnable();
	sceSysregMeBusClockEnable();
	sceSysregMeResetDisable();
	pspSdkSetK1(k1);

	return 0;
}


void KillME(volatile struct me_struct *mei)
{

	unsigned int k1 = pspSdkSetK1(0);

	if (mei == 0)
	{
		pspSdkSetK1(k1);
		return;
	}

	mei->init = 0;

	sceSysregMeResetEnable();
	pspSdkSetK1(k1);
}


int DisableMsLED(void)
{
	unsigned int k1 = pspSdkSetK1(0);
	int ret = sceSysconCtrlLED(0, 0);  /* LED 0 = Memory Stick, 0 = off */
	pspSdkSetK1(k1);
	return ret;
}

/* Run on the caller's decoder thread, outside module-manager startup/stop.
 * Only call while the application exclusively owns the firmware decoder. */
extern int sceMeBootStart660(int mode);
int BootAvcMode(int mode)
{
    static int last_mode = 3;
    unsigned int version = sceKernelDevkitVersion(), k1;
    int ret;
    if ((version != 0x06060010 && version != 0x06060110) ||
        (mode != 3 && mode != 4)) return -1;
    if (mode == last_mode) return 0;
    k1 = pspSdkSetK1(0);
    ret = sceMeBootStart660(mode);
    pspSdkSetK1(k1);
    if (ret == 0) last_mode = mode;
    return ret;
}

static void *find_module_export(SceModule *module, const char *library, u32 nid)
{
    SceLibraryEntryTable *entry;
    unsigned char *end;

    if (!module || !module->ent_top || !module->ent_size) return NULL;
    entry = (SceLibraryEntryTable *)module->ent_top;
    end = (unsigned char *)module->ent_top + module->ent_size;
    while ((unsigned char *)entry < end) {
        unsigned int count;
        u32 *table;
        unsigned int i;

        if (!entry->len) break;
        if (entry->libname && strcmp(entry->libname, library) == 0) {
            count = (unsigned int)entry->stubcount + entry->vstubcount;
            table = (u32 *)entry->entrytable;
            for (i = 0; table && i < count; ++i) {
                if (table[i] == nid) {
                    return (void *)(unsigned long)table[i + count];
                }
            }
            return NULL;
        }
        entry = (SceLibraryEntryTable *)((unsigned char *)entry +
                                         (unsigned int)entry->len * 4u);
    }
    return NULL;
}

/* pspSdkFixupImports() is linked to the caller module's own module_info.
 * Moonlight is a user PRX, so run this explicit equivalent in the kernel
 * helper and target the app's stub table by UID. */
int AvcFixupModuleImports(SceUID consumer_uid, SceUID provider_uid)
{
    SceModule *consumer;
    SceModule *provider;
    SceLibraryStubTable *stub;
    unsigned char *stub_end;
    int fixed = 0;
    int failure = 0;

    if (consumer_uid < 0 || provider_uid < 0) return -1;
    consumer = sceKernelFindModuleByUID(consumer_uid);
    provider = sceKernelFindModuleByUID(provider_uid);
    if (!consumer || !provider || !consumer->stub_top || !consumer->stub_size)
        return -2;

    stub = (SceLibraryStubTable *)consumer->stub_top;
    stub_end = (unsigned char *)consumer->stub_top + consumer->stub_size;
    while ((unsigned char *)stub < stub_end) {
        unsigned int count;
        unsigned int i;
        unsigned int entry_size;

        if (!stub->len) {
            failure = -3;
            break;
        }
        entry_size = (unsigned int)stub->len * 4u;
        if ((unsigned char *)stub + entry_size > stub_end) {
            failure = -3;
            break;
        }
        if ((stub->attribute == 9 ||
             (stub->attribute == 1 &&
              avc_import_was_patched(consumer_uid, stub))) &&
            stub->nidtable && stub->stubtable && stub->libname) {
            count = (unsigned int)stub->stubcount + stub->vstubcount;
            {
                int fixed_in_table = 0;
                if (stub->attribute == 9 &&
                    !avc_import_was_patched(consumer_uid, stub) &&
                    s_avc_patched_import_count >= AVC_PATCHED_IMPORT_MAX) {
                    failure = -4;
                    break;
                }

                for (i = 0; i < count; ++i) {
                    void *address = find_module_export(provider, stub->libname,
                                                       stub->nidtable[i]);
                    if (address) {
                        u32 *instructions = (u32 *)stub->stubtable + i * 2u;
                        instructions[0] = (((u32)(unsigned long)address &
                                            0x03FFFFFFu) >> 2) | 0x0A000000u;
                        instructions[1] = 0;
                        ++fixed_in_table;
                        ++fixed;
                    }
                }
                if (fixed_in_table) {
                    if (stub->attribute == 9) stub->attribute = 1;
                    if (avc_remember_patched_import(consumer_uid, stub) < 0) {
                        failure = -4;
                        break;
                    }
                }
            }
            if (failure < 0) break;
        }
        stub = (SceLibraryStubTable *)((unsigned char *)stub + entry_size);
    }

    sceKernelDcacheWritebackAll();
    sceKernelIcacheClearAll();
    return failure < 0 ? failure : fixed;
}

int module_start(SceSize args, void *argp)
{
	return 0;
}

int module_stop()
{
	return 0;
}
