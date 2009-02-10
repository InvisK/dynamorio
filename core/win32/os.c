/* **********************************************************
 * Copyright (c) 2000-2009 VMware, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * 
 * * Neither the name of VMware, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* Copyright (c) 2003-2007 Determina Corp. */
/* Copyright (c) 2001-2003 Massachusetts Institute of Technology */
/* Copyright (c) 2000-2001 Hewlett-Packard Company */

/*
 * os.c - win32 specific routines
 */

#include "globals.h"
#include "fragment.h"
#include "fcache.h"
#include "ntdll.h"
#include "os_private.h"
#include "nudge.h"
#include "moduledb.h"
#include "hotpatch.h"
#ifdef DEBUG
# include "vmareas.h"
#endif
#include "dispatch.h"
#include "instrument.h" /* is_in_client_lib() */

#include <windows.h>
#include <stddef.h> /* for offsetof */

#include "events.h"             /* event log messages */
#include "aslr.h"
#include "synch.h"

#ifdef DEBUG
DECLARE_CXTSWPROT_VAR(static mutex_t snapshot_lock, INIT_LOCK_FREE(snapshot_lock));
#endif

DECLARE_CXTSWPROT_VAR(static mutex_t dump_core_lock, INIT_LOCK_FREE(dump_core_lock));
DECLARE_CXTSWPROT_VAR(static mutex_t debugbox_lock, INIT_LOCK_FREE(debugbox_lock));

/* globals */
bool intercept_asynch = false;
bool intercept_callbacks = false;
/* we store here to enable TEB.ClientId.ProcessHandle as a spill slot */
process_id_t win32_pid = 0;
/* we store here to enable TEB.ProcessEnvironmentBlock as a spill slot */
void *peb_ptr;
static int os_version;
static const char *os_name;
app_pc vsyscall_page_start = NULL;
/* pc kernel will claim app is at while in syscall */
app_pc vsyscall_after_syscall = NULL;
/* pc of the end of the syscall instr itself */
app_pc vsyscall_syscall_end_pc = NULL;
/* atomic variable to prevent multiple threads from trying to detach at the same time */
DECLARE_CXTSWPROT_VAR(static volatile int dynamo_detaching_flag, LOCK_FREE_STATE);

static bool reached_image_entry = false;

#ifdef PROFILE_RDTSC
uint kilo_hertz; /* cpu clock speed */
#endif

#define HEAP_INITIAL_SIZE 1024*1024

/* pc values delimiting dynamo dll image */
app_pc dynamo_dll_start = NULL;
app_pc dynamo_dll_end = NULL; /* open-ended */

/* needed for randomizing DR library location */
static app_pc dynamo_dll_preferred_base = NULL;

/* thread-local storage slots */
enum {TLS_UNINITIALIZED = (ushort) 0U};
static ushort tls_local_state_offs = TLS_UNINITIALIZED;
/* we keep this cached for easy asm access */
static ushort tls_dcontext_offs = TLS_UNINITIALIZED;

/* used for early inject */
app_pc parent_early_inject_address = NULL; /* dynamo.c fills in */
/* note that this is the early inject location we'll use for child processes
 * dr_early_injected_location is the location (if any) that the current
 * process was injected at */
static int early_inject_location = INJECT_LOCATION_Invalid;
static app_pc early_inject_address = NULL;
static app_pc ldrpLoadDll_address_not_NT = NULL;
static app_pc ldrpLoadDll_address_NT = NULL;
static app_pc ldrpLoadImportModule_address = NULL;
dcontext_t *early_inject_load_helper_dcontext = NULL;

/* forwards */
static void
get_system_basic_info(void);
static bool
is_child_in_thin_client(HANDLE process_handle);
static const char*
get_process_SID_string(void);
static const PSID
get_Everyone_SID(void);
static const PSID
get_process_owner_SID(void);

bool
os_user_directory_supports_ownership(void);

/* Safely gets the target of the call (assumed to be direct) to the nth stack
 * frame (i.e. the entry point to that function), returns NULL on failure. 
 * NOTE - Be aware this routine may be called by DllMain before dr is
 * initialized (before even syscalls_init, though note that safe_read should
 * be fine as will just use the nt wrapper). */
static app_pc
get_nth_stack_frames_call_target(int num_frames, reg_t *ebp)
{
    reg_t *cur_ebp = ebp;
    reg_t next_frame[2];
    int i;
    
    /* walk up the stack */
    for (i = 0; i < num_frames; i++) {
        if (!safe_read(cur_ebp, sizeof(next_frame), next_frame))
            break;
        cur_ebp = (reg_t *)next_frame[0];
    }

    if (i == num_frames) {
        /* success walking frames, return address should be the after
         * call address of the call that targeted this frame */
        /* FIXME - would be nice to get this with decode_cti, but dr might
         * not even be initialized yet and this is safer */
        byte buf[5]; /* sizeof call rel32 */
        if (safe_read((byte *)(next_frame[1] - sizeof(buf)), sizeof(buf), &buf) && 
            buf[0] == CALL_REL32_OPCODE) {
            app_pc return_point = (app_pc)next_frame[1];
            return (return_point + *(int *)&buf[1]);
        }
    }
    return NULL;
}

/* Should be called from NtMapViewOfSection interception with *base
 * pointing to the just mapped region. */ 
void
check_for_ldrpLoadImportModule(byte *base, uint *ebp)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (query_virtual_memory(base, &mbi, sizeof(mbi)) == sizeof(mbi)
        && mbi.Type == MEM_IMAGE && is_readable_pe_base(base)) {
        /* loaded a module, check name */
        const char *name;
        bool match = false;
        
        name = get_dll_short_name(base); /* we only need pe name */
        if (name != NULL) {
            LOG(GLOBAL, LOG_TOP, 1,
                "early_inject hit mapview of image %s\n", name);
            string_option_read_lock();
            /* we control both the pe_name and the option value so use
             * strcmp (vs. strcasecmp), just to keep things tight */
            match = 
                strcmp(DYNAMO_OPTION(early_inject_helper_name),
                       name) == 0;
            string_option_read_unlock();
        }
        if (match) {
            /* Found it. We expect the stack to look like this
             *   (in NtMapViewOfSection)
             *   ntdll!LdrpMapDll
             *   ntdll!LdrpLoadImportModule (what we want)
             * After that don't really care (is one of the
             * Ldrp*ImportDescriptor* routines. So we walk the
             * stack back and get the desired address.
             * FIXME - would be nice if we had some way to double
             * check this address, could try to decode and check against
             * the versions we've seen.
             * Note that NtMapViewOfSection in all its various platform forms
             * (i.e. int, vsyscall, KiFastSystemCall etc.) doesn't set up a
             * new frame (nor do its callees) so will always be depth 2
             */
#define STACK_DEPTH_LdrpLoadImportModule 2
            ldrpLoadImportModule_address =
                get_nth_stack_frames_call_target(STACK_DEPTH_LdrpLoadImportModule,
                                                 (reg_t *)ebp);
            LOG(GLOBAL, LOG_TOP, 1,
                "early_inject found address "PFX" for LdrpLoadImportModule\n",
                ldrpLoadImportModule_address);
        }
    }
}
    
/****************************************************************************
 ** DllMain Routines
 **
 **/

#ifdef INTERNAL
/* we have interp not inline calls to this routine */
void
DllMainThreadAttach()
{
    if (INTERNAL_OPTION(noasynch) && dynamo_initialized && !dynamo_exited) {
        /* we normally intercept thread creation in callback.c, but with
         * noasynch, we do it here (which is later, but better than nothing)
         */
        LOG(GLOBAL, LOG_TOP|LOG_THREADS, 1,
            "DllMain: initializing new thread %d\n", get_thread_id());
        dynamo_thread_init();
    }
}
#endif

/* Hand-made DO_ONCE since DllMain is executed prior to DR init */
DECLARE_FREQPROT_VAR(static bool do_once_DllMain, false);

/* DLL entry point 
 * N.B.: dynamo interprets this routine!
 */
bool WINAPI /* get link warning 4216 if export via APIENTRY */
DllMain(HANDLE hModule, DWORD reason_for_call, LPVOID Reserved)
{
    switch (reason_for_call) {
    case DLL_PROCESS_ATTACH:
        /* case 8097: with -no_hide, DllMain will be called a second time
         * after all the statically-bound dlls are loaded (the loader
         * blindly calls all the init routines regardless of whether a dll
         * was explicitly loaded and already had its init routine called).
         * We make that 2nd time a nop via a custom DO_ONCE (since default
         * DO_ONCE will try to unprotect .data, but we're pre-init).
         */
        if (!do_once_DllMain) {
            byte *cur_ebp;
            do_once_DllMain = true;
            ASSERT(!dynamo_initialized);
            ASSERT(ldrpLoadDll_address_NT == NULL);
            ASSERT(ldrpLoadDll_address_not_NT == NULL);
            /* Carefully walk stack to find address of LdrpLoadDll. */
            /* Remember dr isn't initialized yet, no need to worry about
             * protect from app etc., but also can't check options. */
            GET_FRAME_PTR(cur_ebp);
            /* For non early_inject (late follow children, preinject) expect
             * stack to look like (for win2k and higher)
             *   here (our DllMain)
             *   ntdll!LdrpCallInitRoutine
             *   ntdll!LdrpRunInitializeRoutines
             *   ntdll!LdrpLoadDll
             *   ntdll!LdrLoadDll 
             * For NT is the same only doesn't have ntdll!LdrpCallInitRoutine.
             *
             * That's as far we care, after that is likely to be shimeng.dll
             * or kernel32 (possibly someone else?) depending on how we were
             * injected. For -early_inject, ntdll!LdrGetProcedureAddress is
             * usually the root of the call to our DLLMain (likely something
             * to do with load vs. init order at process startup? FIXME
             * understand better, is there a flag we can send to have this
             * called on load?), but in that case we use the address passed to
             * us by the parent. */
#define STACK_DEPTH_LdrpLoadDll_NT 3
#define STACK_DEPTH_LdrpLoadDll 4
            /* Since dr isn't initialized yet we can't call get_os_version()
             * so just grab both possible LdrpLoadDll addresses (NT and non NT)
             * and we'll sort it out later in early_inject_init(). */
            ldrpLoadDll_address_NT =
                get_nth_stack_frames_call_target(STACK_DEPTH_LdrpLoadDll_NT,
                                                 (reg_t *)cur_ebp);
            ldrpLoadDll_address_not_NT =
                get_nth_stack_frames_call_target(STACK_DEPTH_LdrpLoadDll,
                                                 (reg_t *)cur_ebp);
            /* FIXME - would be nice to have extra verification here,
             * but after this frame there are too many possibilites (many
             * of which are unexported) so is hard to find something we
             * can check. */
        } else
            ASSERT(dynamo_initialized);
        break;
#ifdef INTERNAL
    case DLL_THREAD_ATTACH:
        DllMainThreadAttach();
        break;
#endif
    /* we don't care about DLL_PROCESS_DETACH or DLL_THREAD_DETACH */
    }
    return true;
}

#ifdef WINDOWS_PC_SAMPLE
static profile_t *global_profile = NULL;
static profile_t *dynamo_dll_profile = NULL;
static profile_t *ntdll_profile = NULL;
file_t profile_file = INVALID_FILE;
DECLARE_CXTSWPROT_VAR(mutex_t profile_dump_lock, INIT_LOCK_FREE(profile_dump_lock));

static void
get_dll_bounds(wchar_t *name, app_pc *start, app_pc *end)
{
    HMODULE dllh;
    size_t len;
    PBYTE pb;
    MEMORY_BASIC_INFORMATION mbi;

    dllh = get_module_handle(name);
    ASSERT(dllh != NULL);
    pb = (PBYTE) dllh;
    /* FIXME: we should just call get_allocation_size() */
    len = query_virtual_memory(pb, &mbi, sizeof(mbi));
    ASSERT(len == sizeof(mbi));
    ASSERT(mbi.State != MEM_FREE);
    *start = (app_pc) mbi.AllocationBase;
    do {
        if (mbi.State == MEM_FREE || (app_pc) mbi.AllocationBase != *start)
            break;
        if (pb + mbi.RegionSize < pb)
            break;
        pb += mbi.RegionSize;
    } while (query_virtual_memory(pb, &mbi, sizeof(mbi)) == sizeof(mbi));
    *end = (app_pc) pb;
}

static void
init_global_profiles()
{
    app_pc start, end;

    /* create the profile file */
    /* if logging is on create in log directory, else use base directory */
    DOLOG(1, LOG_ALL, {
        char buf[MAX_PATH];
        uint size = BUFFER_SIZE_ELEMENTS(buf);
        if (get_log_dir(PROCESS_DIR, buf, &size)) {
            NULL_TERMINATE_BUFFER(buf);
            strncat(buf, "\\profile", BUFFER_SIZE_ELEMENTS(buf) - strlen(buf));
            NULL_TERMINATE_BUFFER(buf);
            profile_file = os_open(buf, OS_OPEN_REQUIRE_NEW|OS_OPEN_WRITE);
            LOG(GLOBAL, LOG_PROFILE, 1, "Profile file is \"%s\"\n", buf);
        }
    });
    if (profile_file == INVALID_FILE) {
        get_unique_logfile(".profile", NULL, 0, false, &profile_file); 
    }
    DOLOG(1, LOG_PROFILE, {
        if (profile_file == INVALID_FILE) 
            LOG(GLOBAL, LOG_PROFILE, 1, "Failed to create profile file\n");
    });
    ASSERT(profile_file != INVALID_FILE);

    /* Case 7533: put basic run info in profile file. */
    print_version_and_app_info(profile_file);

    /* set the interval, don't assert success, on my desktop anything less than
     * 1221 gets set to 1221 on laptop was different minimum value, at least
     * appears that os sets it close as possible to the requested (starting
     * value was 39021 for me) */
    LOG(GLOBAL, LOG_PROFILE, 1, "Profile interval was %d, setting to %d,", 
        nt_query_profile_interval(), dynamo_options.prof_pcs_freq);
    nt_set_profile_interval(dynamo_options.prof_pcs_freq);
    LOG(GLOBAL, LOG_PROFILE, 1, " is now %d (units of 100 nanoseconds)\n", 
        nt_query_profile_interval());
    print_file(profile_file, "Interval %d\n\n", nt_query_profile_interval());

    /* create profiles */
    /* Default shift of 30 gives 4 buckets for the global profile,
     * allows us to separate kernel and user space (even in the case
     * of 3GB user space).  Note if invalid range given we default to
     * 30, so we always have a global profile to use as denominator
     * later.
     */
    global_profile = create_profile(UNIVERSAL_REGION_BASE, UNIVERSAL_REGION_END, 
                                    DYNAMO_OPTION(prof_pcs_global), NULL);
    if (dynamo_options.prof_pcs_DR >= 2 && dynamo_options.prof_pcs_DR <= 32) {
        get_dll_bounds(L_DYNAMORIO_LIBRARY_NAME, &start, &end);
        dynamo_dll_profile = create_profile(start, end,
                                            dynamo_options.prof_pcs_DR, NULL);
    }
    if (dynamo_options.prof_pcs_ntdll >= 2 && 
        dynamo_options.prof_pcs_ntdll <= 32) {
        get_dll_bounds(L"ntdll.dll", &start, &end);
        ntdll_profile = create_profile(start, end, 
                                       dynamo_options.prof_pcs_ntdll, NULL);
    }
    
    /* start profiles */
    start_profile(global_profile);
    if (dynamo_dll_profile)
        start_profile(dynamo_dll_profile);
    if (ntdll_profile)
        start_profile(ntdll_profile);
}

static void
dump_dll_profile(profile_t *profile, uint global_sum, char *dll_name)
{
    uint dll_sum;
    uint top=0, bottom=0;

    dll_sum = sum_profile(profile);
    if (global_sum > 0)
        divide_uint64_print(dll_sum, global_sum, true, 2, &top, &bottom);
    print_file(profile_file, 
               "\nDumping %s profile\n%d hits out of %d, %u.%.2u%%\n",
               dll_name, dll_sum, global_sum, top, bottom);
    LOG(GLOBAL, LOG_PROFILE, 1, 
        "%s profile had %d hits out of %d total, %u.%.2u%%\n",
        dll_name, dll_sum, global_sum, top, bottom);
    dump_profile(profile_file, profile);
    free_profile(profile);
}

static void
exit_global_profiles()
{
    int global_sum;

    if (dynamo_dll_profile)
        stop_profile(dynamo_dll_profile);
    if (ntdll_profile)
        stop_profile(ntdll_profile);
    stop_profile(global_profile);

    global_sum = sum_profile(global_profile);

    /* we expect to be the last thread at this point. 
       FIXME: we can remove the mutex_lock/unlock then */
    mutex_lock(&profile_dump_lock);
    if (dynamo_dll_profile)
        dump_dll_profile(dynamo_dll_profile, global_sum, "dynamorio.dll");
    if (ntdll_profile)
        dump_dll_profile(ntdll_profile, global_sum, "ntdll.dll");

    print_file(profile_file, "\nDumping global profile\n%d hits\n", global_sum);
    dump_profile(profile_file, global_profile);
    mutex_unlock(&profile_dump_lock);
    LOG(GLOBAL, LOG_PROFILE, 1, 
        "\nDumping global profile\n%d hits\n", global_sum);
    DOLOG(1, LOG_PROFILE, dump_profile(GLOBAL, global_profile););
    free_profile(global_profile);

    DELETE_LOCK(profile_dump_lock);
}
#endif

/**
 **
 ****************************************************************************/

/* FIXME: Right now error reporting will work here, but once we have our
 * error reporting syscalls going through wrappers and requiring this
 * init routine, we'll have to have a fallback here that dynamically
 * determines the syscalls and finishes init, and then reports the error.
 * We may never be able to report errors for the non-NT OS family.
 * N.B.: this is too early for LOGs so don't do any -- any errors reported
 * will not die, they will simply skip LOG.
 * N.B.: this is prior to eventlog_init(), but then we've been reporting
 * usage errors prior to that for a long time now anyway.
 */
void
windows_version_init()
{
    PEB *peb = get_own_peb();
    /* choose appropriate syscall array (the syscall numbers change from
     * one version of windows to the next!
     * they may even change at different patch levels)
     */
    syscalls = NULL;

    DODEBUG({ check_syscall_array_sizes(); });

    if (peb->OSPlatformId == VER_PLATFORM_WIN32_NT) {
        /* WinNT or descendents */
        if (peb->OSMajorVersion == 6 && peb->OSMinorVersion == 0) {
            module_handle_t ntdllh = get_ntdll_base();
            /* Vista system call number differ between service packs, we use
             * the existence of NtReplacePartitionUnit to detect sp1 - see
             * PR 246402.  They also differ for 32-bit vs 64-bit/wow64. */
            if (get_proc_address(ntdllh, "NtReplacePartitionUnit") != NULL) {
                if (module_is_64bit(get_ntdll_base()) ||
                    is_wow64_process(NT_CURRENT_PROCESS)) {
                    syscalls = (int *) windows_vista_sp1_x64_syscalls;
                    os_name = "Microsoft Windows Vista x64 SP1";
                } else {
                    syscalls = (int *) windows_vista_sp1_syscalls;
                    os_name = "Microsoft Windows Vista SP1";
                }
            } else {
                if (module_is_64bit(get_ntdll_base()) ||
                    is_wow64_process(NT_CURRENT_PROCESS)) {
                    syscalls = (int *) windows_vista_sp0_x64_syscalls;
                    os_name = "Microsoft Windows Vista x64 SP0";
                } else {
                    syscalls = (int *) windows_vista_sp0_syscalls;
                    os_name = "Microsoft Windows Vista SP0";
                }
            }
            os_version = WINDOWS_VERSION_VISTA;
        } else if (peb->OSMajorVersion == 5 && peb->OSMinorVersion == 2) {
            /* Version 5.2 can mean 32- or 64-bit 2003, or 64-bit XP */
            /* Assumption: get_ntll_base makes no system calls */
            if (module_is_64bit(get_ntdll_base()) ||
                is_wow64_process(NT_CURRENT_PROCESS)) {
                /* We expect x64 2003 and x64 XP to have the same system call
                 * numbers but that has not been verified.  System call numbers
                 * remain the same even under WOW64 (ignoring the extra WOW
                 * system calls, anyway).  We do not split the version for WOW
                 * as most users do not care to distinguish; those that do must
                 * use a separate is_wow64_process() check.
                 */
                syscalls = (int *) windows_XP_x64_syscalls;
                /* We don't yet have need to split the version enum */
                os_version = WINDOWS_VERSION_2003;
                os_name = "Microsoft Windows x64 XP/2003";
            } else {
                syscalls = (int *) windows_2003_syscalls;
                os_version = WINDOWS_VERSION_2003;
                os_name = "Microsoft Windows 2003";
            }
        } else if (peb->OSMajorVersion == 5 && peb->OSMinorVersion == 1) {
            syscalls = (int *) windows_XP_syscalls;
            os_version = WINDOWS_VERSION_XP;
            os_name = "Microsoft Windows XP";
        } else if (peb->OSMajorVersion == 5 && peb->OSMinorVersion == 0) {
            syscalls = (int *) windows_2000_syscalls;
            os_version = WINDOWS_VERSION_2000;
            os_name = "Microsoft Windows 2000";
        } else if (peb->OSMajorVersion == 4) {
            module_handle_t ntdllh = get_ntdll_base();
            os_version = WINDOWS_VERSION_NT;
            /* NT4 syscalls differ among service packs.
             * Rather than reading the registry to find the service pack we
             * directly check which system calls are there.  We don't just
             * check the number of the last syscall in our list b/c we want to
             * avoid issues w/ hookers.
             * We rely on these observations:
             *   SP3: + Nt{Read,Write}FileScatter
             *   SP4: - NtW32Call
             */
            if (get_proc_address(ntdllh, "NtW32Call") != NULL) {
                /* < SP4 */
                /* we don't know whether SP1 and SP2 fall in line w/ SP0 or w/ SP3,
                 * or possibly are different from both, but we don't support them
                 */
                if (get_proc_address(ntdllh, "NtReadFileScatter") != NULL) {
                    /* > SP0 */
                    syscalls = (int *) windows_NT_sp3_syscalls;
                    os_name = "Microsoft Windows NT SP3";
                } else {
                    /* < SP3 */
                    syscalls = (int *) windows_NT_sp0_syscalls;
                    os_name = "Microsoft Windows NT SP0";
                }
            } else {
                syscalls = (int *) windows_NT_sp4_syscalls;
                os_name = "Microsoft Windows NT SP4, 5, 6, or 6a";
            }
        } else {
            SYSLOG_INTERNAL_ERROR("Unknown Windows NT-family version: major=%d, minor=%d\n",
                                  peb->OSMajorVersion, peb->OSMinorVersion);
            os_name = "Unrecognized Windows NT-family version";
            FATAL_USAGE_ERROR(BAD_OS_VERSION, 4, get_application_name(),
                              get_application_pid(), PRODUCT_NAME, os_name);
        }
    } else if (peb->OSPlatformId == VER_PLATFORM_WIN32_WINDOWS) {
        /* Win95 or Win98 */
        uint ver_high = (peb->OSBuildNumber >> 8) & 0xff;
        uint ver_low = peb->OSBuildNumber & 0xff;
        if (ver_low >= 90 || ver_high >= 5)
            os_name = "Windows ME";
        else if (ver_low >= 10 && ver_low < 90)
            os_name = "Windows 98";
        else if (ver_low < 5)
            os_name = "Windows 3.1 / WfWg";
        else if (ver_low < 10)
            os_name = "Windows 98";
        else
            os_name = "this unknown version of Windows";
        FATAL_USAGE_ERROR(BAD_OS_VERSION, 4, get_application_name(),
                          get_application_pid(), PRODUCT_NAME, os_name);
    } else {
        os_name = "Win32s";
        /* Win32S on Windows 3.1 */
        FATAL_USAGE_ERROR(BAD_OS_VERSION, 4, get_application_name(),
                          get_application_pid(), PRODUCT_NAME, os_name);
    }
}

/* Note that assigning a process to a Job is done only after it has
 * been created - with ZwAssignProcessToJobObject(), and we may start
 * before or after that has been done.
 */
static void
print_mem_quota()
{
    QUOTA_LIMITS qlimits;
    NTSTATUS res = get_process_mem_quota(NT_CURRENT_PROCESS, &qlimits);
    if (!NT_SUCCESS(res)) {
        ASSERT(false && "print_mem_quota");
        return;
    }
    LOG(GLOBAL, LOG_TOP, 1, "Process Memory Limits:\n");
    LOG(GLOBAL, LOG_TOP, 1, "\t Paged pool limit:         %6d KB\n",
        qlimits.PagedPoolLimit/1024);
    LOG(GLOBAL, LOG_TOP, 1, "\t Non Paged pool limit:     %6d KB\n",
        qlimits.NonPagedPoolLimit/1024);
    LOG(GLOBAL, LOG_TOP, 1, "\t Minimum working set size: %6d KB\n",
        qlimits.MinimumWorkingSetSize/1024);
    LOG(GLOBAL, LOG_TOP, 1, "\t Maximum working set size: %6d KB\n",
        qlimits.MaximumWorkingSetSize/1024);

    /* 4GB for unlimited */
    LOG(GLOBAL, LOG_TOP, 1, "\t Pagefile limit:          %6d KB\n",
        qlimits.PagefileLimit/1024);
    /* TimeLimit not supported on Win2k, but WSRM (Windows System
     * Resource Manager) can definitely set, so expected to be
     * supported on Win2003.  Time in 100ns units.
     */
    LOG(GLOBAL, LOG_TOP, 1, "\t TimeLimit:    0x%.8x%.8x\n",
        qlimits.TimeLimit.HighPart, qlimits.TimeLimit.LowPart);
}

/* os-specific initializations */
static void init_debugbox_title_buf(void);
void
os_init(void)
{
    PEB *peb = get_own_peb();
    uint alignment = 0;
    uint offs;
    int res;

    if (dynamo_options.max_supported_os_version <
        peb->OSMajorVersion * 10 + peb->OSMinorVersion) {
        SYSLOG(SYSLOG_WARNING, UNSUPPORTED_OS_VERSION, 3, 
               get_application_name(), get_application_pid(), os_name);
    }

    /* make sure we create the message box title string before we are 
     * multi-threaded and is no longer safe to do so on demand, this also
     * takes care of initializing the static buffer get_appilication_name
     * and get_application_pid */
    init_debugbox_title_buf();

    win32_pid =  get_process_id();
    LOG(GLOBAL, LOG_TOP, 1, "Process id: %d\n", win32_pid);
    peb_ptr = (void *) get_own_peb();
    LOG(GLOBAL, LOG_TOP, 1, "PEB: "PFX"\n", peb_ptr);
    ASSERT((PEB *)peb_ptr == get_own_teb()->ProcessEnvironmentBlock);

    /* match enums in os_exports.h with TEB definition from ntdll.h */
    ASSERT(EXCEPTION_LIST_TIB_OFFSET == offsetof(TEB, ExceptionList));
    ASSERT(TOP_STACK_TIB_OFFSET == offsetof(TEB, StackBase));
    ASSERT(BASE_STACK_TIB_OFFSET == offsetof(TEB, StackLimit));
    ASSERT(FIBER_DATA_TIB_OFFSET == offsetof(TEB, FiberData));
    ASSERT(SELF_TIB_OFFSET == offsetof(TEB, Self));
    ASSERT(TID_TIB_OFFSET == offsetof(TEB, ClientId) +
           offsetof(CLIENT_ID, UniqueThread));
    ASSERT(PID_TIB_OFFSET == offsetof(TEB, ClientId) +
           offsetof(CLIENT_ID, UniqueProcess));
    ASSERT(ERRNO_TIB_OFFSET == offsetof(TEB, LastErrorValue));
    ASSERT(WOW64_TIB_OFFSET == offsetof(TEB, WOW32Reserved));
    ASSERT(PEB_TIB_OFFSET == offsetof(TEB, ProcessEnvironmentBlock));

    /* windows_version_init should have already been called */
    ASSERT(syscalls != NULL);
    LOG(GLOBAL, LOG_TOP, 1, "Running on %s\n", os_name);

    ntdll_init();
    callback_init();

    eventlog_init(); /* os dependent and currently Windows specific */

    if (os_version >= WINDOWS_VERSION_XP) {
        /* FIXME: bootstrapping problem where we see 0x7ffe0300 before we see
         * the 1st sysenter...solution for now is to hardcode initial values so
         * we pass the 1st PROGRAM_SHEPHERDING code origins test, then re-set these once
         * we see the 1st syscall.
         */
        /* on XP service pack 2 the syscall enter and exit stubs are Ki 
         * routines in ntdll.dll FIXME : as a hack for now will leave 
         * page_start as 0 (as it would be for 2000, since region is 
         * executable no need for the code origins exception) and 
         * after syscall to the appropriate value, this means will still 
         * execute the return natively (as in xp/03) for simplicity even 
         * though we could intercept it much more easily than before since 
         * the ki routines are aligned (less concern about enough space for the
         * interception stub, nicely exported for us etc.)
         */
        /* initializing so get_module_handle should be safe, FIXME */
        module_handle_t ntdllh = get_ntdll_base();
        app_pc return_point = (app_pc) get_proc_address(ntdllh, 
                                                        "KiFastSystemCallRet");
        if (return_point) {
            vsyscall_after_syscall = (app_pc) return_point;
            vsyscall_syscall_end_pc = NULL; /* wait until 1st one */
        } else {
            /* FIXME : if INT syscalls are being used then this opens up a
             * security hole for the followin page */
            vsyscall_page_start = VSYSCALL_PAGE_START_BOOTSTRAP_VALUE;
            vsyscall_after_syscall = VSYSCALL_AFTER_SYSCALL_BOOTSTRAP_VALUE;
            vsyscall_syscall_end_pc = vsyscall_after_syscall;
        }
    }

    /* TLS alignment use either preferred on processor, or hardcoded option value */
    if (DYNAMO_OPTION(tls_align) == 0) {
        IF_X64(ASSERT_TRUNCATE(alignment, uint, proc_get_cache_line_size()));
        alignment = (uint) proc_get_cache_line_size();
    } else {
        alignment = DYNAMO_OPTION(tls_align);
    }
    /* case 3701 about performance gains, 
     * and case 6670 about TLS conflict in SQL2005 */
    
    /* FIXME: could control which entry should be cache aligned, but
     * we should be able to restructure the state to ensure first
     * entry is indeed important.  Should make sure we choose same
     * position in both release and debug, see local_state_t.stats.
     */

    /* allocate thread-private storage */
    res = tls_calloc(false/*no synch required*/, &offs, TLS_NUM_SLOTS,
                     alignment);

    DODEBUG({
        /* FIXME: elevate failure here to a release-build syslog? */
        if (!res)
            SYSLOG_INTERNAL_ERROR("Cannot allocate %d tls slots at %d alignment", TLS_NUM_SLOTS,
                                  alignment);
    });

    /* retry with no alignment on failure */
    if (!res) {
        alignment = 0;
        ASSERT_NOT_TESTED();
            
        /* allocate thread-private storage with no alignment */
        res = tls_calloc(false/*no synch required*/, &offs, TLS_NUM_SLOTS,
                         alignment);

        /* report even in release build that we really can't grab in TLS64 */
        if (!res) {
            ASSERT_NOT_TESTED();
            SYSLOG_INTERNAL_ERROR("Cannot allocate %d tls slots at %d alignment", TLS_NUM_SLOTS,
                                  alignment);

            report_dynamorio_problem(NULL, DUMPCORE_INTERNAL_EXCEPTION, NULL, NULL,
                                     "Unrecoverable error on TLS allocation",
                                     NULL, NULL, NULL);
        }
    }

    ASSERT(res);
    ASSERT(offs != TLS_UNINITIALIZED);
    ASSERT_TRUNCATE(tls_local_state_offs, ushort, offs);
    tls_local_state_offs = (ushort) offs;
    LOG(GLOBAL, LOG_TOP, 1, "%d TLS slots are @ %s:0x%x\n",
        TLS_NUM_SLOTS, IF_X64_ELSE("gs", "fs"), tls_local_state_offs);
    ASSERT_CURIOSITY(proc_is_cache_aligned(get_local_state()) || 
                     DYNAMO_OPTION(tls_align != 0));
    tls_dcontext_offs = os_tls_offset(TLS_DCONTEXT_SLOT);
    ASSERT(tls_dcontext_offs != TLS_UNINITIALIZED);

    DOLOG(1, LOG_VMAREAS, { print_modules(GLOBAL, DUMP_NOT_XML); });
    DOLOG(2, LOG_TOP, { print_mem_quota(); });
    
#ifdef WINDOWS_PC_SAMPLE
    if (dynamo_options.profile_pcs)
        init_global_profiles();
#endif
    
#ifdef PROFILE_RDTSC
    if (dynamo_options.profile_times) {
        ASSERT_NOT_TESTED();
        kilo_hertz = get_timer_frequency();
        LOG(GLOBAL, LOG_TOP|LOG_STATS, 1, "CPU MHz is %d\n", kilo_hertz/1000);
    }
#endif

    if (!dr_early_injected)
        inject_init();

    get_dynamorio_library_path(); 
    /* just to preserve side effects. If not done yet in eventlog,
     * path needs to be be preserved before hiding from module list.
     */

    aslr_init();

    /* ensure static cache buffers are primed, both for .data protection purposes and
     * because it may not be safe to get this information later */
    get_own_qualified_name();
    get_own_unqualified_name();
    get_own_short_qualified_name();
    get_own_short_unqualified_name();
    get_application_short_name();
    get_process_primary_SID();
    get_process_SID_string();
    get_process_owner_SID();
    get_Everyone_SID();

    /* avoid later .data-unprotection calls */
    get_dynamorio_dll_preferred_base();
    get_image_entry();
    get_system_basic_info();
    os_user_directory_supports_ownership();
    is_wow64_process(NT_CURRENT_PROCESS);
}

static void
print_mem_stats()
{
    VM_COUNTERS mem;
    bool ok = get_process_mem_stats(NT_CURRENT_PROCESS, &mem);
    ASSERT(ok);
    LOG(GLOBAL, LOG_TOP, 1, "Process Memory Statistics:\n");
    LOG(GLOBAL, LOG_TOP, 1, "\tPeak virtual size:         %6d KB\n",
        mem.PeakVirtualSize/1024);
    LOG(GLOBAL, LOG_TOP, 1, "\tPeak working set size:     %6d KB\n",
        mem.PeakWorkingSetSize/1024);
    LOG(GLOBAL, LOG_TOP, 1, "\tPeak paged pool usage:     %6d KB\n",
        mem.QuotaPeakPagedPoolUsage/1024);
    LOG(GLOBAL, LOG_TOP, 1, "\tPeak non-paged pool usage: %6d KB\n",
        mem.QuotaPeakNonPagedPoolUsage/1024);
    LOG(GLOBAL, LOG_TOP, 1, "\tPeak pagefile usage:       %6d KB\n",
        mem.PeakPagefileUsage/1024);
}

/* os-specific atexit cleanup 
 * note that this is called even on the fast exit release path so don't add
 * uneccesary cleanup without ifdef DEBUG, but be careful with ifdef DEBUG's 
 * also as Detach wants to leave nothing from us behind
 * Called by dynamo_shared_exit() and the fast path in dynamo_process_exit(). 
 */
void
os_fast_exit(void)
{
    /* make sure we never see an .exe that does all its work in
     * DllMain()'s -- unlikely, even .NET apps have an image entry
     * just to jump to mscoree
     *
     * The curiosity is relaxed for thin_client and hotp_only; if nothing else
     * in the core is has run into this, then reached_image_entry doesn't have
     * to be set for thin_client & hotp_only.  TODO: put in the image entry
     * hook or not?
     *
     * The curiosity is also relaxed if we enter DR using the API
     */
    ASSERT_CURIOSITY(reached_image_entry_yet() ||
                     RUNNING_WITHOUT_CODE_CACHE()
                     IF_APP_EXPORTS( || dr_api_entry));

    DOLOG(1, LOG_TOP, { print_mem_quota(); });
    DOLOG(1, LOG_TOP, {
        print_mem_stats();
    });

#ifdef WINDOWS_PC_SAMPLE
    if (dynamo_options.profile_pcs) {
        exit_global_profiles();
        /* check to see if we are using the fast exit path
         * if so dump profiles that were skipped */
# ifndef DEBUG
        if (dynamo_detaching_flag == LOCK_FREE_STATE) {
            /* fast exit path, get remaining ungathered profile data */
            if (dynamo_options.prof_pcs_gencode >= 2 && 
                dynamo_options.prof_pcs_gencode <= 32) {
                thread_record_t **threads;
                int num, i;
                /* get surviving threads */
                arch_profile_exit();
                mutex_lock(&thread_initexit_lock);
                get_list_of_threads(&threads, &num);
                for (i = 0; i < num; i++) {
                    arch_thread_profile_exit(threads[i]->dcontext);
                }
                global_heap_free(threads, num*sizeof(thread_record_t*) 
                                 HEAPACCT(ACCT_THREAD_MGT));
                mutex_unlock(&thread_initexit_lock);
            }
            if (dynamo_options.prof_pcs_fcache >= 2 && 
                dynamo_options.prof_pcs_fcache <= 32) {
                /* note that fcache_exit() is called before os_fast_exit(),
                 * we are here on fast exit path in which case fcache_exit() 
                 * is not called
                 */
                fcache_profile_exit();
            }
            if (dynamo_options.prof_pcs_stubs >= 2 && 
                dynamo_options.prof_pcs_stubs <= 32) {
                special_heap_profile_exit();
            }
        }
# endif
        print_file(profile_file, "\nFinished dumping all profile info\n");
        close_file(profile_file);
    }
#endif

    eventlog_fast_exit();

#ifdef DEBUG
    module_info_exit();
    DELETE_LOCK(snapshot_lock);
#endif

    /* case 10338: we don't free TLS on the fast path, in case there
     * are other active threads: we don't want to synchall on exit so
     * we let other threads run and try not to crash them until
     * the process is terminated.
     */

    DELETE_LOCK(dump_core_lock);
    DELETE_LOCK(debugbox_lock);

    callback_exit();
    ntdll_exit();
}

/* os-specific atexit cleanup since Detach wants to leave nothing from
 * us behind.  In addition any debug cleanup should only be DODEBUG.
 * Called by dynamo_shared_exit().
 * Note it is expected to be called _after_ os_fast_exit().
 */
void
os_slow_exit(void)
{
    /* free and zero thread-private storage (case 10338: slow path only) */
    DEBUG_DECLARE(int res = )
        tls_cfree(true/*need to synch*/, (uint) tls_local_state_offs,
                  TLS_NUM_SLOTS);
    ASSERT(res);

    aslr_exit();
    eventlog_slow_exit();
}

/* FIXME: what are good values here? */
#define KILL_PROC_EXIT_STATUS -1
#define KILL_THREAD_EXIT_STATUS -1

byte *
os_terminate_static_arguments(bool exit_process)
{
    byte *arguments;
        
    /* arguments for NtTerminate{Process,Thread} */
    typedef struct _terminate_args_t {
        union {
            const byte *debug_code;
            byte pad_bytes[SYSCALL_PARAM_MAX_OFFSET];
        } padding;
        struct {
            IN HANDLE ProcessOrThreadHandle;
            IN NTSTATUS ExitStatus;
        } args;
    } terminate_args_t;
    /* It is not safe to use app stack and hope application will work.
     * We need to stick the arguments for NtTerminate* in a place that
     * doesn't exacerbate the problem - esp may have been in attacker's
     * hands - so we place args in const static (read only) dr memory.
     */
    /* To facilitate detecting syscall failure for SYSENTER, we set a
     * retaddr at edx (two extra slots there) as esp will be set to edx 
     * by the kernel at the return from the sysenter. The kernel then sends
     * control to a native ret which targets the debug infinite loop.
     * (DEBUG only).
     */
    static const terminate_args_t term_thread_args = {
        IF_DEBUG_ELSE_0((byte *)debug_infinite_loop), /* 0 -> NULL for release */
        {NT_CURRENT_THREAD, KILL_THREAD_EXIT_STATUS}
    };
    static const terminate_args_t term_proc_args = {
        IF_DEBUG_ELSE_0((byte *)debug_infinite_loop), /* 0 -> NULL for release */
        {NT_CURRENT_PROCESS, KILL_PROC_EXIT_STATUS}
    };
    /* special sygate froms (non-const) */
    static terminate_args_t sygate_term_thread_args = {
        0, /* will be set to sysenter_ret_address */
        {NT_CURRENT_THREAD, KILL_THREAD_EXIT_STATUS}
    };
    static terminate_args_t sygate_term_proc_args = {
        0, /* will be set to sysenter_ret_address */
        {NT_CURRENT_PROCESS, KILL_PROC_EXIT_STATUS}
    };
    
    /* for LOG statement just pick proc vs. thread here, will adjust for
     * offset below */
    if (exit_process) {
        if (DYNAMO_OPTION(sygate_sysenter) && 
            get_syscall_method() == SYSCALL_METHOD_SYSENTER) {
            byte *tgt = (byte *)&sygate_term_proc_args;
            /* Note we overwrite every time we use this, but is ATOMIC and
             * always with the same value so is ok */
            SELF_UNPROTECT_DATASEC(DATASEC_RARELY_PROT);
            ATOMIC_ADDR_WRITE(tgt, sysenter_ret_address, false);
            DODEBUG({
                    ATOMIC_ADDR_WRITE(tgt+sizeof(sysenter_ret_address),
                                      (byte *)debug_infinite_loop, false);});
            SELF_PROTECT_DATASEC(DATASEC_RARELY_PROT);
            arguments = (byte *)&sygate_term_proc_args;
        } else
            arguments = (byte *)&term_proc_args;
    } else {
        if (DYNAMO_OPTION(sygate_sysenter) && 
            get_syscall_method() == SYSCALL_METHOD_SYSENTER) {
            byte *tgt = (byte *)&sygate_term_thread_args;
            SELF_UNPROTECT_DATASEC(DATASEC_RARELY_PROT);
            ATOMIC_ADDR_WRITE(tgt, sysenter_ret_address, false);
            DODEBUG({
                tgt += sizeof(sysenter_ret_address);
                ATOMIC_ADDR_WRITE(tgt, (byte *)debug_infinite_loop, false);});
            SELF_PROTECT_DATASEC(DATASEC_RARELY_PROT);
            arguments = (byte *)&sygate_term_thread_args;
        } else
            arguments = (byte *)&term_thread_args;
    }
    
    LOG(THREAD_GET, LOG_SYSCALLS, 2,
        "Placing terminate arguments tombstone at "PFX" offset=0x%x\n", 
        arguments, SYSCALL_PARAM_OFFSET());
    
    arguments += offsetof(terminate_args_t, args) - SYSCALL_PARAM_OFFSET();
    return arguments;
}

/* dcontext is not needed for TERMINATE_PROCESS, so can pass NULL in
 */
void
os_terminate(dcontext_t *dcontext, terminate_flags_t terminate_type)
{
    HANDLE currentThreadOrProcess = NT_CURRENT_PROCESS;
    bool exit_process = true;
    
    ASSERT(TEST(TERMINATE_PROCESS, terminate_type) != /* xor */
           TEST(TERMINATE_THREAD, terminate_type));

    /* We could be holding the bb_building_lock at this point -- if we cleanup,
     * we will get a rank order violation with all_threads_synch_lock.  if we
     * unlock the bb lock, we risk an error about the non-owning thread
     * releasing the lock.
     * Our solution is for the caller to release it when possible --
     * on an attack we know if we hold it or not.  But for other callers
     * they do not know who holds it...for now we do nothing, none of them
     * terminate just a thread, so the process is going down anyway, and it's
     * just a debug build assertion :)
     */

    /* clean up may be dangerous - just terminate */
    if (terminate_type == TERMINATE_PROCESS) {
        /* skip synchronizing dynamic options, is risky and caller has almost 
         * certainly already done so for a syslog */
        if (TESTANY(DETACH_ON_TERMINATE|DETACH_ON_TERMINATE_NO_CLEAN, 
                    DYNAMO_OPTION(internal_detach_mask))) {
            /* FIXME : if we run into stack problems we could reset the stack
             * here though caller has likely alredy gone as deep as detach 
             * will since almost everyone SYSLOG's before calling this */
            detach_helper(TEST(DETACH_ON_TERMINATE_NO_CLEAN, 
                               DYNAMO_OPTION(internal_detach_mask)) ? 
                          DETACH_BAD_STATE_NO_CLEANUP : DETACH_BAD_STATE);
            /* skip option synch, make this as safe as possible */
            SYSLOG_INTERNAL_NO_OPTION_SYNCH(SYSLOG_WARNING, 
                                            "detach on terminate failed or already started by another thread, killing thread %d\n", 
                                            get_thread_id());
            /* if we get here, either we recursed or someone is already trying
             * to detach, just kill this thread so progress is made we don't
             * have anything better to do with it */
            /* skip cleanup, our state is likely messed up and we'd just like 
             * to get out alive, also avoids recursion problems, see caveat at 
             * remove_thread below */
            terminate_type = TERMINATE_THREAD;
        } else {
            nt_terminate_process(currentThreadOrProcess, KILL_PROC_EXIT_STATUS);
            ASSERT_NOT_REACHED();
        }
    }

    /* CHECK: Can a process disallow PROCESS_TERMINATE or THREAD_TERMINATE 
       access even to itself? 
    */
    if (TEST(TERMINATE_THREAD, terminate_type)) {
        exit_process = (get_num_threads() == 1 && !dynamo_exited);
        if (!exit_process) {
            currentThreadOrProcess = NT_CURRENT_THREAD;
        } 
    }
    
    STATS_INC(num_threads_killed);
    if (TEST(TERMINATE_CLEANUP, terminate_type)) {
        byte *arguments = os_terminate_static_arguments(exit_process);

        /* Make sure debug loop pointer is in expected place since this makes
         * assumptions about offsets.  We don't use the debug loop pointer for
         * int2e/syscall/wow64 system calls (since they return to the invocation
         * and can be handled there).  For SYSENTER the SYSCALL_PARAM_OFFSET should
         * match up with arguments such that arguments is pointing to debugme */
        ASSERT(does_syscall_ret_to_callsite() ||
               *(byte **)arguments == (byte *)&debug_infinite_loop ||
               (DYNAMO_OPTION(sygate_sysenter) && 
                *(((byte **)arguments)+1) == (byte *)&debug_infinite_loop));
      
        STATS_INC(num_threads_killed_cleanly);

        /* we enter from several different places, so rewind until top-level kstat */
        KSTOP_REWIND_UNTIL(thread_measured);

        /* now we issue a syscall by number */
        /* we can't use issue_system_call_for_app because it relies on 
         * dstack that we should release */
        /* FIXME: what happens now if we get some callbacks that are still on 
         * their way? Shouldn't happen since Terminate* are believed to be 
         * non-alertable. */
        /* FIXME: we only want the last part of cleanup_and_terminate */
        ASSERT(dcontext != NULL);
        cleanup_and_terminate
            (dcontext, 
             syscalls[exit_process ? SYS_TerminateProcess : SYS_TerminateThread],
             (ptr_uint_t)
             IF_X64_ELSE((exit_process ? NT_CURRENT_PROCESS : NT_CURRENT_THREAD),
                         arguments),
             (ptr_uint_t)
             IF_X64_ELSE((exit_process ? KILL_PROC_EXIT_STATUS:KILL_THREAD_EXIT_STATUS),
                         arguments /* no 2nd arg, just a filler */),
             exit_process);
    } else {
        /* may have decided to terminate process */
        if (exit_process) {
            nt_terminate_process(currentThreadOrProcess, KILL_PROC_EXIT_STATUS);
            ASSERT_NOT_REACHED();
        } else {
            /* FIXME: this is now very dangerous - we even leave our own state */
            /* we should at least remove this thread from the all threads list 
             * to avoid synchronizing issues, though we are running the risk of
             * an infinite loop with a failure in this function and detach on 
             * failure */
            if (all_threads != NULL)
                remove_thread(NULL, get_thread_id());
            nt_terminate_thread(currentThreadOrProcess, KILL_THREAD_EXIT_STATUS);
            ASSERT_NOT_REACHED();
        }

        /* CHECK: who is supposed to clean up the thread's stack?  
           ZwFreeVirtualMemory can be called by another thread
           waiting on the thread object, hopefully someone will do it */
    }
    ASSERT_NOT_REACHED();
}

void
os_tls_init()
{
    /* everything was done in os_init, even TEB TLS slots are initialized to 0 for us */
}

void
os_tls_exit(local_state_t *local_state)
{
    /* not needed for windows, everything is done is os_slow_exit including zeroing
     * the freed TEB tls slots */
}

void
os_thread_init(dcontext_t *dcontext)
{
    NTSTATUS res;
    DEBUG_DECLARE(bool ok;)
    os_thread_data_t *ostd = (os_thread_data_t *)
        heap_alloc(dcontext, sizeof(os_thread_data_t) HEAPACCT(ACCT_OTHER));
    dcontext->os_field = (void *) ostd;
    /* init ostd fields here */
    ostd->stack_base = NULL;
    ostd->stack_top = NULL;
    ostd->teb_stack_no_longer_valid = false;
    DEBUG_DECLARE(ok = )get_stack_bounds(dcontext, NULL, NULL);
    ASSERT(ok);
    
    /* case 8721: save the win32 start address and print it in the ldmp */
    res = query_win32_start_addr(NT_CURRENT_THREAD, &dcontext->win32_start_addr);
    if (!NT_SUCCESS(res)) {
        ASSERT(false && "failed to obtain win32 start address");
        dcontext->win32_start_addr = (app_pc)0;
    } else {
        LOG(THREAD, LOG_THREADS, 2, "win32 start addr is "PFX"\n",
            dcontext->win32_start_addr);
    }
    aslr_thread_init(dcontext);
}

void
os_thread_exit(dcontext_t *dcontext)
{
    os_thread_data_t *ostd = (os_thread_data_t *) dcontext->os_field;
    aslr_thread_exit(dcontext);
#ifdef DEBUG
    /* for non-debug we do fast exit path and don't free local heap */
    /* clean up ostd fields here */
    heap_free(dcontext, ostd, sizeof(os_thread_data_t) HEAPACCT(ACCT_OTHER));
#endif
}

void
os_thread_stack_exit(dcontext_t *dcontext)
{
    os_thread_data_t *ostd = (os_thread_data_t *) dcontext->os_field;
    ASSERT_OWN_MUTEX(true, &thread_initexit_lock);
    /* see case 3768: a thread's stack is not de-allocated by this process,
     * so we remove its stack from our executable region here
     * ref also case 5518 where it is sometimes freed in process, we watch for
     * that and set stack_base to NULL
     * note: thin_client doesn't have executable or aslr areas, so this is moot.
     */
    if (DYNAMO_OPTION(thin_client))
        return;

    if (ostd->stack_base != NULL) {
        LOG(THREAD, LOG_SYSCALLS|LOG_VMAREAS, 1,
            "os_thread_stack_exit : removing "PFX" - "PFX"\n",
            ostd->stack_base, ostd->stack_top);

        ASSERT(ostd->stack_top != NULL);
        DODEBUG({
            /* ASSERT that os region matches region stored in ostd */
            byte *alloc_base;
            size_t size = get_allocation_size(ostd->stack_base, &alloc_base);
            /* Xref case 5877, this assert can fire if the exiting thread has already
             * exited (resulting in freed stack) before we clean it up.  This could be do
             * to using THREAD_SYNCH_TERMINATED_AND_CLEANED with a synch_with* routine
             * (no current uses) or a race with detach resuming a translated thread
             * before cleaning it up.  The detach race is harmless so we allow it. */
            ASSERT(doing_detach ||
                   ((size == (size_t)(ostd->stack_top - ostd->stack_base) ||
                     /* PR 252008: for WOW64 nudges we allocate an extra page. */
                     (size == PAGE_SIZE + (size_t)(ostd->stack_top - ostd->stack_base) &&
                      is_wow64_process(NT_CURRENT_PROCESS) &&
                      dcontext->nudge_target != NULL)) &&
                    ostd->stack_base == alloc_base));
        });
        /* believe <= win2k frees the stack in process, would like to check
         * that but we run into problems with stacks that are never freed
         * (TerminateThread, threads killed by TerminateProcess 0, last thread
         * calling TerminateProcess, etc.) FIXME figure out way to add an
         * assert_curiosity */
        /* make sure we use our dcontext (dcontext could belong to another thread
         * from other_thread_exit) since flushing will end up using this dcontext
         * for synchronization purposes */
        app_memory_deallocation(get_thread_private_dcontext(), ostd->stack_base,
                                ostd->stack_top - ostd->stack_base,
                                true /* own thread_initexit_lock */,
                                false /* not image */);
        if (TEST(ASLR_HEAP_FILL, DYNAMO_OPTION(aslr))) {
            size_t stack_reserved_size = ostd->stack_top - ostd->stack_base;
            /* verified above with get_allocation_size() this is not only the committed portion */
            aslr_pre_process_free_virtual_memory(dcontext, ostd->stack_base, 
                                                 stack_reserved_size);
        }
    } else {
        LOG(THREAD, LOG_SYSCALLS|LOG_VMAREAS, 1,
            "os_thread_stack_exit : Thread's os stack has alread been freed\n");
        /* believe >= XP free the stack out of process */
        ASSERT(ostd->stack_top == NULL);
        ASSERT_CURIOSITY(get_os_version() <= WINDOWS_VERSION_2000);
    }
}

void
os_thread_under_dynamo(dcontext_t *dcontext)
{
    /* add cur thread to callback list */
    set_asynch_interception(get_thread_id(), true);
}

void
os_thread_not_under_dynamo(dcontext_t *dcontext)
{
    /* remove cur thread from callback list */
    set_asynch_interception(get_thread_id(), false);
}

#ifdef CLIENT_SIDELINE /* PR 222812: tied to sideline usage */
/* We have two choices for the stack:
 * 1) Hide it from user, allocate a 1-or-2-page stack to get to the apc
 *    dispatcher and swap to our dstack, which the client uses; then free
 *    the dstack using existing mechanisms, and let the small stack leak
 *    or eventually free it if worth the effort.
 * 2) Expose the stack size to the user and do not create a dcontext
 *    or a dstack.  Then we need a custom stack de-allocation.
 * I'm going with #2.  dr_terminate_client_thread() de-allocates.
 * 
 * FIXME PR 210591: transparency issues:
 * 1) All dlls will be notifed of thread creation by DLL_THREAD_ATTACH
 * 2) The thread will show up in the list of threads accessed by
 *    NtQuerySystemInformation's SystemProcessesAndThreadsInformation structure.
 *
 * FIXME PR 202669: if the client leaves reservation space we should have
 * the stack auto-expand.
 */
DR_API bool
dr_create_client_thread(void (*func)(void *param), void *arg,
                        size_t stack_reserve, size_t stack_commit)
{
    HANDLE hthread;
    bool res;
    thread_id_t tid;
    CLIENT_ASSERT(stack_reserve >= stack_commit,
                  "stack_reserve must be >= stack_commit");
    /* FIXME PR 225714: does this work on Vista? */
    hthread = create_thread(NT_CURRENT_PROCESS, (void *)func, arg, NULL, 0,
                            stack_reserve, stack_commit, false, &tid);
    CLIENT_ASSERT(hthread != INVALID_HANDLE_VALUE, "error creating thread");
    if (hthread == INVALID_HANDLE_VALUE)
        return false;
    /* mark the thread now as a native thread */
    add_thread(hthread, tid, false/*!under_dynamo_control*/, NULL/*no dcontext*/);
    /* FIXME: what about all of our check_sole_thread() checks? */
    res = close_handle(hthread);
    CLIENT_ASSERT(res, "error closing thread handle");
    return res;        
}

DR_API bool
dr_terminate_client_thread(void)
{
    thread_id_t tid = get_thread_id();
    thread_record_t *tr = thread_lookup(tid);
    byte *stack_base;
    byte *term_args;
    if (tr == NULL || tr->dcontext != NULL || tr->under_dynamo_control) {
        CLIENT_ASSERT(false, "dr_terminate_client_thread called on non-client thread");
        return false;
    }
    /* We don't have a dcontext+dstack to clean up so we do not call os_terminate;
     * we simply do the necessary cleanup here.
     */
    /* increment _exiting_thread_count so that we don't get killed after 
     * we're off the all_threads list */
    ATOMIC_INC(int, exiting_thread_count);
    remove_thread(NT_CURRENT_THREAD, tid);
    /* Once we're using CreateThreadEx, for get_os_version() >= WINDOWS_VERSION_VISTA
     * the kernel will clean up the stack for us and we can directly call
     * nt_terminate_thread here.
     */
    get_stack_bounds(NULL, &stack_base, NULL);
    term_args = os_terminate_static_arguments(false/*thread only*/);
    cleanup_and_terminate_client_thread
        (syscalls[SYS_TerminateThread], stack_base,
         IF_X64_ELSE(NT_CURRENT_THREAD, (ptr_uint_t)term_args),
         IF_X64_ELSE(KILL_THREAD_EXIT_STATUS, (ptr_uint_t)term_args/*filler*/));
    ASSERT_NOT_REACHED();
    return true; /* satisfy compiler */
}
#endif CLIENT_SIDELINE /* PR 222812: tied to sideline usage */

int
get_os_version()
{
    return os_version;
}

bool
is_in_dynamo_dll(app_pc pc)
{
    ASSERT(dynamo_dll_start != NULL && dynamo_dll_end != NULL);
    return (pc >= dynamo_dll_start && pc < dynamo_dll_end);
}

static char *
mem_state_string(uint state)
{
    switch (state) {
    case 0:            return "none";
    case MEM_COMMIT:   return "COMMIT";
    case MEM_FREE:     return "FREE";
    case MEM_RESERVE:  return "RESERVE";
    }
    return "(error)";
}

static char *
mem_type_string(uint type)
{
    switch (type) {
    case 0:            return "none";
    case MEM_IMAGE:    return "IMAGE";
    case MEM_MAPPED:   return "MAPPED";
    case MEM_PRIVATE:  return "PRIVATE";
    }
    return "(error)";
}

char *
prot_string(uint prot)
{
    uint ignore_extras = prot & ~PAGE_PROTECTION_QUALIFIERS;
    switch (ignore_extras) {
    case PAGE_NOACCESS:          return "----";
    case PAGE_READONLY:          return "r---";
    case PAGE_READWRITE:         return "rw--";
    case PAGE_WRITECOPY:         return "rw-c";
    case PAGE_EXECUTE:           return "--x-";
    case PAGE_EXECUTE_READ:      return "r-x-";
    case PAGE_EXECUTE_READWRITE: return "rwx-";
    case PAGE_EXECUTE_WRITECOPY: return "rwxc";
    }
    return "(error)";
}

static bool
prot_is_readable(uint prot) 
{
    prot &= ~PAGE_PROTECTION_QUALIFIERS;
    /* FIXME: consider just E to be unreadable?
     * do not do exclusions, sometimes prot == 0 or something
     */
    switch (prot) {
    case PAGE_READONLY:         
    case PAGE_READWRITE:        
    case PAGE_WRITECOPY:        
    case PAGE_EXECUTE:          
    case PAGE_EXECUTE_READ:     
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: return true;
    }
    return false;
}

bool
prot_is_writable(uint prot) 
{
    prot &= ~PAGE_PROTECTION_QUALIFIERS;
    return (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
            prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY);
}

bool
prot_is_executable(uint prot) 
{
    prot &= ~PAGE_PROTECTION_QUALIFIERS;
    return (prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ ||
            prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY);
}

/* true when page hasn't been written to */
bool
prot_is_copyonwrite(uint prot) 
{
    prot &= ~PAGE_PROTECTION_QUALIFIERS;
    /* although really providing an enumeration, the known PAGE_
     * values use separate bit flags.  We use TESTANY in case new
     * PAGE_PROTECTION_QUALIFIERS show up.
     */
    return TESTANY(PAGE_WRITECOPY|PAGE_EXECUTE_WRITECOPY, prot);
}

/* translate platform independent protection bits to native flags */
int
memprot_to_osprot(uint prot)
{
    uint os_prot = 0;
    if (TEST(MEMPROT_EXEC, prot)) {
        ASSERT(TEST(MEMPROT_READ, prot));
        if (TEST(MEMPROT_WRITE, prot))
            os_prot = PAGE_EXECUTE_READWRITE;
        else
            os_prot = PAGE_EXECUTE_READ;
    } else if (TEST(MEMPROT_READ, prot)) {
        if (TEST(MEMPROT_WRITE, prot))
            os_prot = PAGE_READWRITE;
        else
            os_prot = PAGE_READONLY;
    } else
        os_prot = PAGE_NOACCESS;
    return os_prot;
}

/* translate native flags to platform independent protection bits */
int
osprot_to_memprot(uint prot)
{
    uint mem_prot = 0;
    if (prot_is_readable(prot))
        mem_prot |= MEMPROT_READ;
    if (prot_is_writable(prot))
        mem_prot |= MEMPROT_WRITE;
    if (prot_is_executable(prot))
        mem_prot |= MEMPROT_EXEC;
    return mem_prot;
}

static int
osprot_add_writecopy(uint prot)
{
    int pr = prot & ~PAGE_PROTECTION_QUALIFIERS;
    switch (pr) {
    case PAGE_READWRITE: return (prot & (~pr)) | PAGE_WRITECOPY;
    case PAGE_EXECUTE_READWRITE: return (prot & (~pr)) | PAGE_EXECUTE_WRITECOPY;
    default: ASSERT_NOT_REACHED();
    }
    return prot;
}

/* returns osprot flags preserving all native protection flags except
 * for RWX, which are replaced according to memprot */
uint
osprot_replace_memprot(uint old_osprot, uint memprot)
{
    uint old_qualifiers = 
        old_osprot & PAGE_PROTECTION_QUALIFIERS;
    uint new_osprot = memprot_to_osprot(memprot);

    /* preserve any old WRITECOPY 'flag' if page hasn't been touched */
    if (prot_is_copyonwrite(old_osprot) && prot_is_writable(new_osprot))
        new_osprot = osprot_add_writecopy(new_osprot);
    new_osprot |= old_qualifiers;
    return new_osprot;
}

void
dump_mbi(file_t file, MEMORY_BASIC_INFORMATION *mbi, bool dump_xml)
{
    print_file(file, dump_xml ?
               "\t\tBaseAddress=         \""PFX"\"\n"
               "\t\tAllocationBase=      \""PFX"\"\n"
               "\t\tAllocationProtect=   \"0x%08x %s\"\n"
               "\t\tRegionSize=          \"0x%08x\"\n"
               "\t\tState=               \"0x%08x %s\"\n"
               "\t\tProtect=             \"0x%08x %s\"\n"
               "\t\tType=                \"0x%08x %s\"\n"
               :
               "BaseAddress:       "PFX"\n"
               "AllocationBase:    "PFX"\n"
               "AllocationProtect: 0x%08x %s\n"
               "RegionSize:        0x%08x\n"
               "State:             0x%08x %s\n"
               "Protect:           0x%08x %s\n"
               "Type:              0x%08x %s\n",
               mbi->BaseAddress,
               mbi->AllocationBase,
               mbi->AllocationProtect, prot_string(mbi->AllocationProtect),
               mbi->RegionSize,
               mbi->State, mem_state_string(mbi->State),
               mbi->Protect, prot_string(mbi->Protect),
               mbi->Type, mem_type_string(mbi->Type));
}

void
dump_mbi_addr(file_t file, app_pc target, bool dump_xml)
{
    MEMORY_BASIC_INFORMATION mbi;
    size_t len;
    len = query_virtual_memory(target, &mbi, sizeof(mbi));
    if (len == sizeof(mbi))
        dump_mbi(file, &mbi, dump_xml);
    else {
        if (dump_xml) {
            print_file(file, "<-- Unable to dump mbi for addr "PFX"\n -->",
                       target);
        } else {
            print_file(file, "Unable to dump mbi for addr "PFX"\n", target);
        }
    }
}

/* FIXME:
 * We need to be able to distinguish our own pid from that of a child
 * process.  We observe that after CreateProcess a child has pid of 0 (as
 * determined by process_id_from_handle, calling NtQueryInformationProcess).
 * For our current injection methods pid is always set when we take over,
 * but for future early-injection methods what if the pid is still 0 when
 * we start executing in the process' context?
 */
bool
is_pid_me(process_id_t pid)
{
    return (pid ==  get_process_id());
}

bool
is_phandle_me(HANDLE phandle)
{
    /* make the common case of NT_CURRENT_PROCESS faster */
    if (phandle == NT_CURRENT_PROCESS) {
        return true;
    } else {
        /* we know of no way to detect whether two handles point to the same object,
         * so we go to pid
         */
        process_id_t pid = process_id_from_handle(phandle);
        return is_pid_me(pid);
    }
}

/* used only in get_dynamorio_library_path() but file level namespace
 * so it is easily available to windbg scripts */
static char dynamorio_library_path[MAXIMUM_PATH];

/* get full path to our own library, (cached), used for forking and message file name */
char*
get_dynamorio_library_path()
{
    /* This operation could be dangerous, so it is still better that we do it
     *  once at startup when there is a single thread only 
     */
    if (!dynamorio_library_path[0]) { /* not cached */
        /* get_module_name can take any pc in the dll,
         * so we simply take the address of this function
         * instead of using get_module_handle to find the base
         */
        app_pc pb = (app_pc)&get_dynamorio_library_path;

        /* here's where we set the library path */
        get_module_name(pb, dynamorio_library_path, MAXIMUM_PATH);
    }
    return dynamorio_library_path;
}

/* based on a process handle to a process that is not yet running,
 * verify whether we should be taking control over it */
/* if target process should be injected into returns true, and
 * inject_settings is set if non-NULL */
bool
should_inject_into_process(dcontext_t *dcontext, HANDLE process_handle,
                           int *rununder_mask, /* OPTIONAL OUT */
                           inject_setting_mask_t *inject_settings /* OPTIONAL OUT */)
{
    bool inject = false;
    synchronize_dynamic_options();
    if (DYNAMO_OPTION(follow_children) || DYNAMO_OPTION(follow_explicit_children) ||
        DYNAMO_OPTION(follow_systemwide)) {

        inject_setting_mask_t should_inject = 
            systemwide_should_inject(process_handle, rununder_mask);

        if (DYNAMO_OPTION(follow_systemwide) && TEST(INJECT_TRUE, should_inject)) {
            LOG(THREAD, LOG_SYSCALLS|LOG_THREADS, 1,
                "\tconfigured child should be injected\n");
            inject = true;
        }

        if (!inject && DYNAMO_OPTION(follow_explicit_children) &&
            TESTALL(INJECT_EXPLICIT|INJECT_TRUE, should_inject)) {
            LOG(THREAD, LOG_SYSCALLS|LOG_THREADS, 1,
                "\texplicit child should be injected\n");
            inject = true;
        }

        if (!inject && DYNAMO_OPTION(follow_children)) {
            inject = true; /* -follow_children defaults to inject */

            /* check if child should be excluded from running under dr */
            if (TEST(INJECT_EXCLUDED, should_inject)) {
                LOG(THREAD, LOG_SYSCALLS|LOG_THREADS, 1,
                    "\tchild is excluded, not injecting\n");
                inject = false;
            }

            /* check if we should leave injection to preinjector */
            if (TEST(INJECT_TRUE, should_inject) && systemwide_inject_enabled() &&
                !TEST(INJECT_EXPLICIT, should_inject)) {
                ASSERT(!DYNAMO_OPTION(follow_systemwide));
                LOG(THREAD, LOG_SYSCALLS|LOG_THREADS, 1,
                    "\tletting preinjector inject into child\n");
                inject = false;
            }

            DODEBUG({
                if (inject)
                    LOG(THREAD, LOG_SYSCALLS|LOG_THREADS, 1,
                        "\tnon-excluded, non-preinjected child should be injected\n");
            });
        }
        if (inject) {
            ASSERT(!TEST(INJECT_EXCLUDED, should_inject));
            if (inject_settings != NULL)
                *inject_settings = should_inject;
        }
    }
    DODEBUG({
        if (inject) {
            LOG(THREAD, LOG_SYSCALLS|LOG_THREADS, 1,
                "\tinjecting into child process\n");
            
        } else {
            LOG(THREAD, LOG_SYSCALLS|LOG_THREADS, 1, "\tletting child execute natively "
                "(may still get injected by systemwide injector!)\n");
        }
    });
    return inject;
}

/* cxt may be NULL if -inject_at_create_process */
static int
inject_into_process(dcontext_t *dcontext, HANDLE process_handle, CONTEXT *cxt,
                    inject_setting_mask_t should_inject)
{
    /* Here in fact we don't want to have the default argument override
       mechanism take place.  If an app specific AUTOINJECT value is
       provided, then we should of course use it.  However, if no
       specific one is given we should not use the global default when
       follow_children.  For follow_explicit_children it is actually OK
       to use the global default value, it will be the GUI's
       responsibility to set both the parent and child if it is desired
       to have them use the same library.
    */
    char  library_path_buf[MAXIMUM_PATH];
    char *library = library_path_buf;
    bool res;

    int err = get_process_parameter(process_handle, L_DYNAMORIO_VAR_AUTOINJECT,
                                    library_path_buf, sizeof(library_path_buf));

    /* If there is no app-specific subkey, then we should check in what mode are we injecting */
    /* If we are in fact in follow_children - meaning all children are followed, 
       and there is no app specific option then we should use the parent library,
       unless the child is in fact explicit in which case we just use the global library.
    */

    switch (err) {
    case GET_PARAMETER_SUCCESS:
        break;
    case GET_PARAMETER_NOAPPSPECIFIC:
        /* We got the global key's library, use parent's library instead if the only
         * reason we're injecting is -follow_children (i.e. reading RUNUNDER gave us 
         * !INJECT_TRUE). */
        if (!TEST(INJECT_TRUE, should_inject)) {
            ASSERT(DYNAMO_OPTION(follow_children));
            library = get_dynamorio_library_path();
        }
        break;
    case GET_PARAMETER_BUF_TOO_SMALL:
    case GET_PARAMETER_FAILURE:
        library = get_dynamorio_library_path();
        break;
    default:
        ASSERT_NOT_REACHED();
    }
                   
    LOG(THREAD, LOG_SYSCALLS|LOG_THREADS, 1, "\tinjecting %s into child process\n", library);
    
    if (DYNAMO_OPTION(aslr_dr) &&
        /* case 8749 - can't aslr dr for thin_clients */
        process_handle != NULL && !is_child_in_thin_client(process_handle)) {
        aslr_force_dynamorio_rebase(process_handle);
    }

    /* Can't early inject 32-bit DR into a wow64 process as there is no
     * ntdll32.dll at early inject point, so thread injection only.  PR 215423.
     */
    if (DYNAMO_OPTION(early_inject) && !is_wow64_process(process_handle)) {
        ASSERT(early_inject_address != NULL);
        /* FIXME if early_inject_address == NULL then early_inject_init failed
         * to find the correct address to use.  Don't expect that to happen,
         * but if it does could fall back to late injection (though we can't
         * be sure that would work, i.e. early thread process for ex.) or
         * do a SYSLOG error. */
        res = inject_into_new_process(process_handle, library,
                                      DYNAMO_OPTION(early_inject_map),
                                      early_inject_location,
                                      early_inject_address);
    } else {
        ASSERT(cxt != NULL);
        res = inject_into_thread(process_handle, cxt, NULL, library);
    }
    
    if (!res) {
        SYSLOG_INTERNAL_ERROR("ERROR: injection into child process failed");
        ASSERT_NOT_REACHED();
        return false;       /* for compilation correctness and release builds */
    }
    return true;
}

bool
is_first_thread_in_new_process(HANDLE process_handle, CONTEXT *cxt)
{
    /* ASSUMPTION: based on what I've seen, on win2k a new process has 
     * pid 0 until its first thread is created.  This is not true on XP
     * so we also check if the argument value is the PEB address 
     * (which it should be if it is the first thread in the process,
     * according to inside win2k).  This is a slight risk of double
     * or late injection if someone creates a remote thread that 
     * happens to have an argument that equals the address of PEB.  
     * Better would be able to tell from Eip if it is pointing at the 
     * kernel32 thread start thunk or the kernel32 process start thunk,
     * or to check if the number of threads in the process equals 0, 
     * but no easy way to do either here.  FIXME 
     */
    process_id_t pid = process_id_from_handle(process_handle);
    return (pid == 0 || 
            (!is_pid_me(pid) && cxt->CXT_XBX == (ptr_uint_t)get_peb(process_handle)));
}

/* Depending on registry and options maybe inject into child process with
 * handle process_handle.  Called by SYS_CreateThread in pre_system_call (in
 * which case cxt is non-NULL) and by CreateProcess[Ex] in post_system_call (in
 * which case cxt is NULL). */
void
maybe_inject_into_process(dcontext_t *dcontext, HANDLE process_handle,
                          CONTEXT *cxt)
{
    /* if inject_at_create_process becomes dynamic, need to move this check below
     * the synchronize dynamic options */
    /* FIXME - can't read process parameters, at process create time is NULL
     * value in peb field except in Vista.  Could pass it in. */
    /* Can't early inject 32-bit DR into a wow64 process as there is no
     * ntdll32.dll at early inject point, so thread injection only.  PR 215423.
     */
    if ((cxt == NULL && (DYNAMO_OPTION(inject_at_create_process) ||
                         (get_os_version() == WINDOWS_VERSION_VISTA && 
                          DYNAMO_OPTION(vista_inject_at_create_process)))
         && !is_wow64_process(process_handle)) ||
        (cxt != NULL && is_first_thread_in_new_process(process_handle, cxt))) {
        int rununder_mask;
        inject_setting_mask_t should_inject;
        /* Creating a new process & at potential inject point */
        DEBUG_DECLARE(process_id_t pid = process_id_from_handle(process_handle);)
        DOLOG(3, LOG_SYSCALLS|LOG_THREADS, {
            SYSLOG_INTERNAL_INFO("found a fork: pid %d", pid);
        });
        LOG(THREAD, LOG_SYSCALLS|LOG_THREADS, 1, "found a fork: pid %d\n", pid);

        if (should_inject_into_process(dcontext, process_handle,
                                       &rununder_mask, &should_inject)) {
            ASSERT(cxt != NULL || DYNAMO_OPTION(early_inject));
            /* FIXME : if not -early_inject, we are going to read and write
             * to cxt, which may be unsafe */
            if (inject_into_process(dcontext, process_handle, cxt,
                                    should_inject)) {
                check_for_run_once(process_handle, rununder_mask);
            }
        }
    }
}

/* For case 8749: can't aslr dr for thin_client because cygwin apps will die. */
static bool
is_child_in_thin_client(HANDLE process_handle)
{
    bool res;
    const options_t *opts;

    /* Shouldn't be using this for the current process. */
    ASSERT(process_handle != NT_CURRENT_PROCESS && 
           process_handle != NT_CURRENT_THREAD && process_handle != NULL);

    opts = get_process_options(process_handle);
    ASSERT_OWN_READWRITE_LOCK(true, &options_lock);
    ASSERT(opts != NULL);

    /* In this case the option is used only for preventing aslr_dr, so be safe
     * if you can't read it and say yes which will prevent aslr dr.  Note: this
     * isn't the secure option, which is to say no, so that we alsr dr.
     * Interesting tradeoff; choosing safety as this scenario is rare in which
     * case first goal is to do no harm.
     */
    if (opts == NULL) {
        res = false;
    } else {
        res = opts->thin_client;
    }
    write_unlock(&options_lock);
    return res;
}

app_pc
get_dynamorio_dll_start() {
    if (dynamo_dll_start == NULL)
        dynamo_dll_start = get_allocation_base((app_pc) get_dynamorio_dll_start);
    return dynamo_dll_start;
}

app_pc
get_dynamorio_dll_preferred_base(void)
{
    if (dynamo_dll_preferred_base == NULL) {
        dynamo_dll_preferred_base = get_module_preferred_base(get_dynamorio_dll_start());
        ASSERT(dynamo_dll_preferred_base != NULL);
    }
    return dynamo_dll_preferred_base;
}

/* IF_X64(ASSERT_NOT_IMPLEMENTED(false)) -- need to update */
static app_pc highest_user_address = (app_pc)(ptr_uint_t)0x7ffeffff;
/* 0x7ffeffff on 2GB:2GB default */
/* or 0xbffeffff with /3GB in boot.ini, */
/* /userva switch may also change the actual value seen */

static void
get_system_basic_info(void)
{
    SYSTEM_BASIC_INFORMATION sbasic_info;

    NTSTATUS result = query_system_info(SystemBasicInformation, 
                                        sizeof(SYSTEM_BASIC_INFORMATION), 
                                        &sbasic_info);
    ASSERT(NT_SUCCESS(result));
    highest_user_address = (app_pc)sbasic_info.HighestUserAddress;
    /* typically we have 2GB:2GB split between user and kernel virtual memory 
     * lkd> dd nt!MmUserProbeAddress  l1
     *  8055ee34  7fff0000
     * lkd> dd nt!MmHighestUserAddress  l1
     *  8055ee3c  7ffeffff
     */

    LOG(GLOBAL, LOG_VMAREAS, 1, "get_system_basic_info: "
        "HighestUserAddress "PFX"\n", highest_user_address);

    /* for testing purposes we can pretend all other addresses are inaccessible */
    if (INTERNAL_OPTION(stress_fake_userva) != 0) {
        if (highest_user_address > (app_pc)INTERNAL_OPTION(stress_fake_userva)) {
            highest_user_address = (app_pc)INTERNAL_OPTION(stress_fake_userva);
            SYSLOG_INTERNAL_WARNING("using stress_fake_userva "PFX"\n", 
                                    highest_user_address);
        } else {
            ASSERT_CURIOSITY("useless stress_fake_userva");
        }
    }

    ASSERT(OS_ALLOC_GRANULARITY == sbasic_info.AllocationGranularity);
}

bool
is_user_address(app_pc pc)
{
    /* we don't worry about LowestUserAddress which is the first 64KB
     * page which should normally be invalid.
     *
     * FIXME: case 10899 although users can in fact allocate in the
     * NULL allocation region (by using base=1), as typically done in
     * a local NULL pointer attack.  Natively the address is still
     * visible for execution, and the OS should handle base=NULL on
     * our queries, but we should check if we will.  Of course, this
     * is likely an attack so it is OK for us to fail it. 
     * 
     * we only check upper bound and treat all smaller addresses as user addresses
     */
    return pc <= highest_user_address;
}

void
merge_writecopy_pages(app_pc start, app_pc end)
{
    MEMORY_BASIC_INFORMATION mbi;
    PBYTE pb = start;
    uint prot;
    size_t len = query_virtual_memory(pb, &mbi, sizeof(mbi));
    ASSERT(len == sizeof(mbi));
    LOG(GLOBAL, LOG_VMAREAS, 2, "merge_writecopy_pages "PFX"-"PFX"\n", start, end);
    do {
        if ((app_pc)mbi.BaseAddress >= end)
            break;
        ASSERT(mbi.State == MEM_COMMIT);
        ASSERT(prot_is_writable(mbi.Protect));
        prot = mbi.Protect &
            ~PAGE_PROTECTION_QUALIFIERS;
        if (prot == PAGE_WRITECOPY) {
            /* HACK (xref case 8069): make a process-local copy to try and merge
             * entire section into single region, for more efficient protection!
             * Yes all the writable regions are already contiguous, but they
             * have different flags, and so are different regions, and
             * NtProtectVirtualMemory refuses to do more than one region at a time.
             * However, regions seem to be merged when they have the
             * same flags, so we just remove the C flag.
             * Calling NtProtectVirtualMemory w/ PAGE_READWRITE to try
             * and remove the copy-on-write bits does not work, so we
             * write to every page!
             * FIXME: test on other versions of windows!
             * it's not documented so it may not be everywhere!
             * works on Win2K Professional
             * N.B.: since make_writable doesn't preserve copy-on-write,
             * it's a good thing we do this hack.
             * FIXME: how many of these pages would never have been made private?
             * (case 8069 covers that inquiry)
             */
            volatile app_pc pc = mbi.BaseAddress;
            app_pc stop = ((app_pc)mbi.BaseAddress) + mbi.RegionSize;
            ASSERT(stop <= end);
            LOG(GLOBAL, LOG_VMAREAS, 2, "writing to "SZFMT" pages to get local copy of"
                " copy-on-write section @"PFX"\n", mbi.RegionSize/PAGE_SIZE,
                pc);
            while (pc < stop) {
                *pc = *pc;
                pc += PAGE_SIZE;
            }
        }
        if (pb + mbi.RegionSize < pb)
            break;
        pb += mbi.RegionSize;
    } while (query_virtual_memory(pb, &mbi, sizeof(mbi)) == sizeof(mbi));

    LOG(GLOBAL, LOG_VMAREAS, 2, "checking that "PFX"-"PFX" merger worked\n",
        start, end);
    len = query_virtual_memory(start, &mbi, sizeof(mbi));
    ASSERT(len == sizeof(mbi));
    ASSERT(prot_is_writable(mbi.Protect));
    /* OS could merge w/ another writable region so may not end at end */
    ASSERT(end <= start + mbi.RegionSize);
    /* we only call this on DR data sections right now */
    ASSERT(dynamo_dll_end == NULL || /* FIXME: init it earlier */
           (is_in_dynamo_dll(start) && is_in_dynamo_dll(end)));
    LOG(GLOBAL, LOG_VMAREAS, 2, "DR regions post-merger:\n");
    DOLOG(1, LOG_VMAREAS, {
        print_dynamo_regions();
        LOG(GLOBAL, LOG_VMAREAS, 2, "\n");
    });
}

int
find_dynamo_library_vm_areas()
{
    /* walk through memory regions in our own dll */
    size_t len;
    PBYTE pb;
    MEMORY_BASIC_INFORMATION mbi;
    int num_regions = 0;

    get_dynamorio_library_path(); /* just to preserve side effects */
    LOG(GLOBAL, LOG_VMAREAS, 1, PRODUCT_NAME" dll path: %s\n", get_dynamorio_library_path());

    get_dynamorio_dll_start(); /* for side effects: probably already called though */
    ASSERT(dynamo_dll_start != NULL);
    pb = dynamo_dll_start;
    len = query_virtual_memory(pb, &mbi, sizeof(mbi));
    ASSERT(len == sizeof(mbi));
    ASSERT(mbi.State != MEM_FREE);

    LOG(GLOBAL, LOG_VMAREAS, 1, "\nOur regions:\n");
    do {
        if (mbi.State == MEM_FREE || (app_pc) mbi.AllocationBase != dynamo_dll_start)
            break;
        if (mbi.State == MEM_COMMIT) {
            /* only look at committed regions */
            LOG(GLOBAL, LOG_VMAREAS, 1, PFX"-"PFX" %s\n", mbi.BaseAddress,
                ((app_pc)mbi.BaseAddress) + mbi.RegionSize,
                prot_string(mbi.Protect));
            num_regions++;
            add_dynamo_vm_area(mbi.BaseAddress,
                               ((app_pc)mbi.BaseAddress) + mbi.RegionSize,
                               osprot_to_memprot(mbi.Protect),
                               true /* from image */
                               _IF_DEBUG(prot_string(mbi.Protect)));
            /* we need all writable regions to be inside the
             * sections that we protect */
            ASSERT(!prot_is_writable(mbi.Protect) ||
                   data_sections_enclose_region((app_pc)mbi.BaseAddress,
                                                ((app_pc)mbi.BaseAddress) +
                                                mbi.RegionSize));
        }
        if (pb + mbi.RegionSize < pb)
            break;
        pb += mbi.RegionSize;
    } while (query_virtual_memory(pb, &mbi, sizeof(mbi)) == sizeof(mbi));

    dynamo_dll_end = (app_pc) pb;
    LOG(GLOBAL, LOG_VMAREAS, 1, PRODUCT_NAME" dll: from "PFX" to "PFX"\n\n",
        dynamo_dll_start, dynamo_dll_end);
    return num_regions;
}

void
print_dynamo_regions()
{
    /* walk through memory regions in our own dll */
    size_t len;
    PBYTE pb;
    MEMORY_BASIC_INFORMATION mbi;
    /* dynamo_dll_start is a global defined in find_dynamo_library_vm_areas */
    ASSERT(dynamo_dll_start != NULL);
    pb = dynamo_dll_start;
    len = query_virtual_memory(pb, &mbi, sizeof(mbi));
    ASSERT(len == sizeof(mbi));
    ASSERT(mbi.State != MEM_FREE);

    do {
        if (mbi.State == MEM_FREE || (app_pc) mbi.AllocationBase != dynamo_dll_start)
            break;
        if (mbi.State == MEM_COMMIT) {
            /* only look at committed regions */
            LOG(GLOBAL, LOG_ALL, 1, PFX"-"PFX" %s\n", mbi.BaseAddress,
                ((app_pc)mbi.BaseAddress) + mbi.RegionSize,
                prot_string(mbi.Protect));
        }
        if (pb + mbi.RegionSize < pb)
            break;
        pb += mbi.RegionSize;
    } while (query_virtual_memory(pb, &mbi, sizeof(mbi)) == sizeof(mbi));
}

#ifdef DEBUG /* MEMORY STATS ****************************************/
/* to make it easy to control log statement */
# define MEM_STATS_ADD(stat, sz) do {                   \
    if ((sz) != 0) {                                    \
        STATS_ADD(stat, sz);                            \
        LOG(GLOBAL, LOG_MEMSTATS, 4, #stat" sz "SZFMT"\n", sz); \
    }                                                   \
} while (0);

/* N.B.: "reserved" here means reserved but not committed, so reserved
 * and committed are disjoint, returns whether or not it was our memory
 */
static bool
add_mem_stats(app_pc region, size_t r_commit, size_t r_reserve, bool r_is_stack,
              uint r_type, size_t r_exec, size_t r_rw, size_t r_ro)
{
    bool ours = false;
    /* add region to stats */
    if (r_type == MEM_IMAGE) {
        if (is_in_dynamo_dll(region)) {
            ours = true;
            MEM_STATS_ADD(dr_library_space, r_commit);
            ASSERT(r_reserve == 0);
        } else {
            /* an image can have reserve-only sections (e.g., mscorlib has 2!) */
            MEM_STATS_ADD(app_image_capacity, r_commit+r_reserve);
        }
    } else {
        if (is_dynamo_address(region)) {
            ours = true;
        } else if (r_type == MEM_MAPPED) {
            MEM_STATS_ADD(app_mmap_capacity, r_commit);
        } else {
            if (r_is_stack) {
                MEM_STATS_ADD(app_stack_capacity, r_commit);
            } else {
                MEM_STATS_ADD(app_heap_capacity, r_commit);
            }
        }
    }
    LOG(GLOBAL, LOG_MEMSTATS, 4,
        "Region "PFX"-"PFX" commit="SZFMT" reserve="SZFMT" stack="SZFMT" ours="SZFMT"\n",
        region, region+r_commit+r_reserve, r_commit, r_reserve, r_is_stack, ours);
    if (ours) {
        MEM_STATS_ADD(dr_commited_capacity, r_commit);
        MEM_STATS_ADD(dr_reserved_capacity, r_reserve);
        MEM_STATS_ADD(dr_vsize, r_commit + r_reserve);
    } else {
        MEM_STATS_ADD(app_reserved_capacity, r_reserve);
        MEM_STATS_ADD(app_committed_capacity, r_commit);
        MEM_STATS_ADD(app_vsize, r_commit + r_reserve);
        MEM_STATS_ADD(app_exec_capacity, r_exec);
        MEM_STATS_ADD(app_rw_capacity, r_rw);
        MEM_STATS_ADD(app_ro_capacity, r_ro);
    }
    /* yes, on windows vsize includes reserved */
    MEM_STATS_ADD(total_vsize, r_commit + r_reserve);
    /* count unaligned allocations (PEB TEB etc. see inside win2k pg 420) */
    if (!ALIGNED(region, OS_ALLOC_GRANULARITY)) {
        STATS_INC(unaligned_allocations);
    }
    return ours;
}

/* Since incremental app memory stats are too hard, we use snapshots */
void
mem_stats_snapshot()
{
    PBYTE pb = NULL;
    MEMORY_BASIC_INFORMATION mbi;
    /* stats strategy: have to wait until end of region to know everything,
     * so locally cache sum-of-block values until then
     */
    size_t r_commit = 0, r_reserve = 0, r_exec = 0, r_ro = 0, r_rw = 0;
    bool r_is_stack = false;
    uint r_type = 0;
    app_pc r_start = NULL;
    if (!dynamo_initialized) {
        /* Now that vm_areas_init() is after dynamo_thread_init()'s call to
         * dump_global_stats() we come here prior to dynamo_areas or DR's
         * library bounds being set up: best to just abort until we can
         * gather accurate stats.
         */
        return;
    }
    /* It's too hard to keep track of these incrementally -- would have to
     * record prior to NtAllocateVirtualMemory all of the reserved regions to
     * know which went from reserved to committed, and on freeing to know what
     * was committed and what reserved, etc., so we only do complete snapshots,
     * resetting the stats to 0 each time.
     */
    mutex_lock(&snapshot_lock);
    STATS_RESET(unaligned_allocations);
    STATS_RESET(dr_library_space);
    STATS_RESET(dr_commited_capacity);
    STATS_RESET(dr_reserved_capacity);
    STATS_RESET(total_wasted_vsize);
    STATS_RESET(dr_wasted_vsize);
    STATS_RESET(app_wasted_vsize);
    STATS_RESET(total_vsize);
    STATS_RESET(dr_vsize);
    STATS_RESET(app_vsize);
    STATS_RESET(app_reserved_capacity);
    STATS_RESET(app_committed_capacity);
    STATS_RESET(app_stack_capacity);
    STATS_RESET(app_heap_capacity);
    STATS_RESET(app_image_capacity);
    STATS_RESET(app_mmap_capacity);
    STATS_RESET(app_exec_capacity);
    STATS_RESET(app_ro_capacity);
    STATS_RESET(app_rw_capacity);
    /* walk through every block in memory */
    while (query_virtual_memory(pb, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        /* Standard block iteration that groups blocks with the same
         * allocation base into a single region
         */
        if (mbi.State == MEM_FREE || mbi.AllocationBase == mbi.BaseAddress) {
            bool ours = false;
            if (r_start != NULL)
                ours = add_mem_stats(r_start, r_commit, r_reserve, r_is_stack, 
                                     r_type, r_exec, r_ro, r_rw);
            /* reset for next region */
            r_commit = r_reserve = r_exec = r_ro = r_rw = 0;
            r_is_stack = false;
            r_type = mbi.Type;
            if (mbi.State == MEM_FREE) {
                LOG(GLOBAL, LOG_MEMSTATS, 4, "Free "PFX"-"PFX"\n",
                    mbi.BaseAddress, ((app_pc)mbi.BaseAddress)+mbi.RegionSize);
                if (r_start != NULL && 
                    !ALIGNED(mbi.BaseAddress, OS_ALLOC_GRANULARITY)) {
                    /* wasted virtual address space, at least part of this free
                     * region is unusable */
                    size_t wasted = ALIGN_FORWARD(mbi.BaseAddress, 
                                                  OS_ALLOC_GRANULARITY) - 
                        (ptr_uint_t)mbi.BaseAddress;
                    if (ours) {
                        /* last region is ours, we are wasting */
                        MEM_STATS_ADD(dr_wasted_vsize, (stats_int_t)wasted);
                    } else {
                        /* last region apps, its wasting */
                        MEM_STATS_ADD(app_wasted_vsize, (stats_int_t)wasted);
                    }
                    MEM_STATS_ADD(total_wasted_vsize, (stats_int_t)wasted);
                }
                r_start = NULL;
            } else {
                r_start = mbi.AllocationBase;
            }
        }
        /* incremental work until have end of region */
        if (mbi.State == MEM_RESERVE) {
            r_reserve += mbi.RegionSize;
        } else if (mbi.State == MEM_COMMIT) {
            r_commit += mbi.RegionSize;
            if (TEST(PAGE_GUARD, mbi.Protect)) {
                /* if any guard blocks inside region, assume entire region
                 * is a stack
                 */
                r_is_stack = true;
            }
            /* protection stats could be incremental but that would duplicate checks
             * for being DR memory.
             * mbi.Protect is invalid for reserved memory, only useful for committed.
             */
            if (prot_is_executable(mbi.Protect))
                r_exec += mbi.RegionSize;
            else if (prot_is_writable(mbi.Protect))
                r_rw += mbi.RegionSize;
            else if (prot_is_readable(mbi.Protect))
                r_ro += mbi.RegionSize;
            /* we don't add up no-access memory! */
        }
        if (pb + mbi.RegionSize < pb)
            break;
        pb += mbi.RegionSize;
    }
    if (r_start != NULL)
        add_mem_stats(r_start, r_commit, r_reserve, r_is_stack, r_type, r_exec, r_ro, r_rw);
    STATS_PEAK(unaligned_allocations);
    STATS_PEAK(dr_commited_capacity);
    STATS_PEAK(dr_reserved_capacity);
    STATS_PEAK(total_wasted_vsize);
    STATS_PEAK(dr_wasted_vsize);
    STATS_PEAK(app_wasted_vsize);
    STATS_PEAK(total_vsize);
    STATS_PEAK(dr_vsize);
    STATS_PEAK(app_vsize);
    STATS_PEAK(app_reserved_capacity);
    STATS_PEAK(app_committed_capacity);
    STATS_PEAK(app_stack_capacity);
    STATS_PEAK(app_heap_capacity);
    STATS_PEAK(app_image_capacity);
    STATS_PEAK(app_mmap_capacity);
    STATS_PEAK(app_exec_capacity);
    STATS_PEAK(app_ro_capacity);
    STATS_PEAK(app_rw_capacity);
    mutex_unlock(&snapshot_lock);
}
#endif /* DEBUG (MEMORY STATS) ****************************************/

/* update our data structures that record info on PE modules */
/* rewalking is set when walking existing memory mappings, and is
 * unset if called when processing a system call for (un)map 
 */
static void
process_image(app_pc base, size_t size, uint prot, bool add, bool rewalking)
{
    const char *name = NULL;
    bool module_is_native_exec = false;
    /* ensure header is readable */
    ASSERT(prot_is_readable(prot));
    ASSERT(!rewalking || add);  /* when rewalking can only add */

    /* FIXME: we only know that we are in a MEM_IMAGE
     * we still need to be careful to check it is a real PE
     * We could optimize out these system calls, but for now staying safe
     */
    if (!is_readable_pe_base(base)) {
        ASSERT_CURIOSITY(false);
        return;
    }
    /* Our WOW64 design for 32-bit DR involves ignoring all 64-bit dlls
     * (several are visible: wow64cpu.dll, wow64win.dll, wow64.dll, and ntdll.dll)
     * For 64-bit DR both should be handled.
     */
#ifdef X64
    DODEBUG({
        if (module_is_32bit(base)) {
            LOG(GLOBAL, LOG_VMAREAS, 1,
                "image "PFX"-"PFX" is 32-bit dll (wow64 process?)\n",
                base, base+size);
            ASSERT(is_wow64_process(NT_CURRENT_PROCESS));
        }
    });
#else
    if (module_is_64bit(base)) {
        LOG(GLOBAL, LOG_VMAREAS, 1,
            "image "PFX"-"PFX" is 64-bit dll (wow64 process?): ignoring it!\n", 
            base, base+size);
        ASSERT(is_wow64_process(NT_CURRENT_PROCESS));
        return;
    }
#endif

    /* Track loaded module list.  Needs to be done before
     * hotp_process_image() and any caller of get_module_short_name()
     * or other data that we cache in the list.
     */
    if (add) /* add first */
        module_list_add(base, size, !rewalking /* !rewalking <=> at_map */);
    else
        os_module_set_flag(base, MODULE_BEING_UNLOADED);

    /* DYNAMO_OPTION(native_exec) and DYNAMO_OPTION(use_moduledb) are the
     * primary user of module name */
    name = os_get_module_name_strdup(base HEAPACCT(ACCT_VMAREAS));
    LOG(GLOBAL, LOG_VMAREAS, 1, "image %-15s %smapped @ "PFX"-"PFX"\n",
        name == NULL ? "<no name>" : name, add ? "" : "un", base, base+size);

    if (DYNAMO_OPTION(native_exec) && name != NULL &&
        on_native_exec_list(base, name)) {
        LOG(GLOBAL, LOG_INTERP|LOG_VMAREAS, 1,
            "module %s is on native_exec list\n", name);
        module_is_native_exec = true;

#ifdef GBOP
        /* FIXME: if some one just loads a vm, our gbop would become useless;
         * need better dgc identification for gbop; see case 8087.
         */
        if (add && TEST(GBOP_IS_DGC, DYNAMO_OPTION(gbop)) && !gbop_vm_loaded) {
            /* !gbop_vm_loaded in the check above would prevent this memory 
             * protection change from happenning for each vm load, not that any
             * process load a vm multiple times or multiple vms.
             */
            SELF_UNPROTECT_DATASEC(DATASEC_RARELY_PROT);
            gbop_vm_loaded = true;
            SELF_PROTECT_DATASEC(DATASEC_RARELY_PROT);
        }
#endif
    }

    moduledb_process_image(name, base, add);

    /* Case 7266: add all exes and dlls with managed code to native_exec_areas,
     * for now.
     * FIXME: should try to execute non-managed code under DR, when possible.
     */
    if (DYNAMO_OPTION(native_exec) && DYNAMO_OPTION(native_exec_managed_code) &&
        module_has_cor20_header(base)) {
        DODEBUG({
            if (add) {
                LOG(GLOBAL, LOG_INTERP|LOG_VMAREAS, 1,
                    "process_image: module=%s, base="PFX" has cor20 header, "
                    "adding to native exec areas\n", name ? name : "<noname>", base);
                SYSLOG_INTERNAL_INFO_ONCE("cor20 module %s added to native exec area",
                                          name ? name : "<noname>");
            }
        });
        module_is_native_exec = true;
    }
    /* xref case 10998 - we native exec modules with .pexe sections to handle all the
     * int 3 strangeness.  FIXME - restrict further? only observed on Vista, known .pexe
     * sections from problematic dlls all begin with mostly the same 0x60 first bytes,
     * .pexe is observed to always be the first section, etc. */
    if (DYNAMO_OPTION(native_exec) && DYNAMO_OPTION(native_exec_dot_pexe) && 
        get_named_section_bounds(base, ".pexe", NULL, NULL)) {
        DODEBUG({
            if (add) {
                LOG(GLOBAL, LOG_INTERP|LOG_VMAREAS, 1,
                    "process_image: module=%s, base="PFX" has .pexe section, "
                    "adding to native exec areas\n", name ? name : "<noname>", base);
                SYSLOG_INTERNAL_INFO(".pexe module %s added to native exec area",
                                     name ? name : "<noname>");
                /* check is one of the known .pexe dlls */
                ASSERT_CURIOSITY(name != NULL && check_filter("AuxiliaryDisplayCpl.dll;AuxiliaryDisplayDriverLib.dll;AuxiliaryDisplayServices.dll;NetProjW.dll;P2PGraph.dll;localspl.dll;lsasrv.dll;mssrch.dll;p2psvc.dll;pmcsnap.dll;shell32.dll;spoolss.dll;uDWM.dll", name));
            }
        });
        module_is_native_exec = true;
    }
    if (module_is_native_exec && add) {
        RSTATS_INC(num_native_module_loads);
        vmvector_add(native_exec_areas, base, base+size, NULL);
    } else {
        /* For safety we'll just always remove the region (even if add==true) to avoid
         * any possibility of having stale entries in the vector overlap into new 
         * non-native regions. Also see case 7628. */
        ASSERT(!module_is_native_exec || /* if not native_exec shouldn't be in vector */
               !vmvector_overlap(native_exec_areas, base, base+size));
        vmvector_remove(native_exec_areas, base, base+size);
    }

    if (!IS_STRING_OPTION_EMPTY(patch_proof_list) ||
        !IS_STRING_OPTION_EMPTY(patch_proof_default_list)) {
        /* even if name is not valid we should match ALL */
        if ((IS_LISTSTRING_OPTION_FORALL(patch_proof_list) ||
             IS_LISTSTRING_OPTION_FORALL(patch_proof_default_list))
            || (name != NULL && 
                check_list_default_and_append(dynamo_options.patch_proof_default_list,
                                              dynamo_options.patch_proof_list,
                                              name))) {
            if (add) {
                LOG(GLOBAL, LOG_INTERP|LOG_VMAREAS, 1,
                    "module %s is on patch proof list\n", name ? name : "<noname>");
                STATS_INC(num_patch_proof_module_loads);
                /* assuming code sections are added as non-writable we
                 * will prevent them from becoming writable */

                /* Note adding full module region here,
                 * app_memory_protection_change() will filter out only
                 * CODE.  FIXME: [minor perf] alternatively could walk
                 * module and add only code sections here.
                 */
                vmvector_add(patch_proof_areas, base, base+size, NULL);
            } else {
                /* remove all areas in range */
                vmvector_remove(patch_proof_areas, base, base+size);
            }
        }
    }

#ifdef HOT_PATCHING_INTERFACE
    if (DYNAMO_OPTION(hot_patching)) {
        if (!DYNAMO_OPTION(hotp_only)) {
            hotp_process_image(base, add, false, false, NULL, NULL, 0);
        } else {
            bool needs_processing = false;
            int num_threads = 0;
            thread_record_t **all_threads = NULL;

            /* For hotp_only, image processing is done in two steps.  The 
             * first one is done without suspending all threads (expensive if 
             * done for each dll load or unload).  Only if the first step 
             * identified a module match, all threads (known to the core, of 
             * course) are suspended and the image is processed, i.e., hot 
             * patches are either injected or removed both of which in 
             * hotp_only need all threads to be suspended. 
             */
            hotp_process_image(base, add, false/*no locks*/, 
                               /* Do single-step at init: assume no other threads.
                                * Risk is low; rest of DR assumes it as well.
                                * Can't do two-step since have no dcontext yet
                                * and hit synch_with_all_threads assert. */
                               dynamo_initialized/*just check?*/,
                               dynamo_initialized ? &needs_processing : NULL,
                               NULL, 0);
            if (needs_processing) {
                DEBUG_DECLARE(bool ok =)
                    synch_with_all_threads(THREAD_SYNCH_SUSPENDED, &all_threads, 
                                           /* Case 6821: other synch-all-thread uses that
                                            * only care about threads carrying fcache
                                            * state can ignore us
                                            */
                                           &num_threads, THREAD_SYNCH_NO_LOCKS_NO_XFER,
                                           /* if we fail to suspend a thread (e.g.,
                                            * privilege problems) ignore it.
                                            * FIXME: retry instead? */
                                           THREAD_SYNCH_SUSPEND_FAILURE_IGNORE);
                ASSERT(ok);
                hotp_process_image(base, add, false, false, NULL, all_threads, 
                                   num_threads);
                end_synch_with_all_threads(all_threads, num_threads, true/*resume*/);
            }
        }
    }
#endif

    if (DYNAMO_OPTION(IAT_convert)) { /* case 85 */
        /* add IAT areas to a vmarea for faster lookup */
        app_pc IAT_start, IAT_end;
        bool valid = get_IAT_section_bounds(base, &IAT_start, &IAT_end);
        if (valid && IAT_start != IAT_end) {
            LOG(GLOBAL, LOG_INTERP, 2,
                "module %s IAT("PFX","PFX") %s\n", name ? name : "<noname>", 
                IAT_start, IAT_end, add ? "added" : "removed");
            ASSERT_CURIOSITY(IAT_start != NULL && IAT_end != NULL);
            ASSERT(IAT_start < IAT_end);
            if (add) {
                ASSERT(!vmvector_overlap(IAT_areas, IAT_start, IAT_end));
                STATS_INC(num_IAT_areas);
                if (!module_is_native_exec) {
                    LOG(GLOBAL, LOG_INTERP, 1,
                        "module %s IAT("PFX","PFX") added\n", name ? name : "<noname>", 
                        IAT_start, IAT_end);
                    vmvector_add(IAT_areas, IAT_start, IAT_end, NULL);
                } else {
                    LOG(GLOBAL, LOG_INTERP, 1,
                        "skipping native module %s IAT("PFX","PFX"), native modules seen\n", 
                        name ? name : "<noname>", 
                        IAT_start, IAT_end);
                }
            } else {
                STATS_DEC(num_IAT_areas);
                vmvector_remove(IAT_areas, IAT_start, IAT_end);
            }
        } else {
            ASSERT(!valid || IAT_start == base);
            ASSERT_CURIOSITY(valid && "bad module");
        }
    }

#ifdef RETURN_AFTER_CALL
    DODEBUG({
        if (!add && DYNAMO_OPTION(ret_after_call)) { 
            /* case 5329 (see comments in process_image_post_vmarea()) --
             * here we just check for exec areas before we flush them */
            /* although some have no .text section
             * e.g. hpzst3zm.dll from case 9121 
             */
            if (!executable_vm_area_overlap(base, 
                                            base + size, false/*have no lock*/)) {
                SYSLOG_INTERNAL_WARNING_ONCE("DLL with no executable areas "PFX"-"PFX"\n",
                                             base, base + size);
            }
        }
    });
#endif /* RET_AFTER_CALL */

    /* add module and its export symbols to our list only if logging */
    DOLOG(1, LOG_SYMBOLS, { 
        if (add) {
            /* we need to touch memory to check for PE and that doesn't always work 
             * FIXME: but, this is MEM_IMAGE, and above we verify the header
             * is readable, so we can get rid of all of these system calls here
             */
            add_module_info((app_pc)base, size);
        } else {
            /* remove module if we have it added to our list */
            remove_module_info((app_pc)base, size); 
        }
    });

    if (name != NULL)
        dr_strfree(name HEAPACCT(ACCT_VMAREAS));
}

/* Image processing that must be done after vmarea processing (mainly
 * persisted cache loading)
 */
/* rewalking is set when walking existing memory mappings, and is
 * unset if called when processing a system call for (un)map 
 */
static void
process_image_post_vmarea(app_pc base, size_t size, uint prot, bool add, bool rewalking)
{
    /* ensure header is readable */
    ASSERT(prot_is_readable(prot));
    ASSERT(!rewalking || add);  /* when rewalking can only add */

    /* FIXME: we only know that we are in a MEM_IMAGE
     * we still need to be careful to check it is a real PE
     * We could optimize out these system calls, but for now staying safe
     */
    if (!is_readable_pe_base(base)) {
        ASSERT_CURIOSITY(false);
        return;
    }
    /* Our WOW64 design for 32-bit DR involves ignoring all 64-bit dlls
     * (several are visible: wow64cpu.dll, wow64win.dll, wow64.dll, and ntdll.dll)
     * For 64-bit DR both should be handled.
     */
#ifndef X64
    if (module_is_64bit(base))
        return;
#endif

#ifdef RCT_IND_BRANCH
    if (TEST(OPTION_ENABLED, DYNAMO_OPTION(rct_ind_call)) ||
        TEST(OPTION_ENABLED, DYNAMO_OPTION(rct_ind_jump))) {
        /* we need to know about module addition or removal
         * whether or not we'll act on it right now
         */
        rct_process_module_mmap(base, size, add, rewalking);
    }
#endif /* RCT_IND_BRANCH */

    if (!add) /* remove last */
        module_list_remove(base, size);
}

/* returns true if it added an executable region 
 * ok for dcontext to be NULL if init==true and add==true
 */
static bool
process_memory_region(dcontext_t *dcontext, MEMORY_BASIC_INFORMATION *mbi,
                      bool init, bool add)
{
    bool from_image = (mbi->Type == MEM_IMAGE);
    /* Our WOW64 design involves ignoring all 64-bit dlls
     * (several are visible: wow64cpu.dll, wow64win.dll, wow64.dll, and ntdll.dll)
     * We go ahead and track the memory, but we do not treat as an image
     */
    if (is_wow64_process(NT_CURRENT_PROCESS) &&
        from_image && module_is_64bit(mbi->AllocationBase/*NOT BaseAddress*/))
        from_image = false;
    ASSERT(dcontext != NULL || (init && add));
    DOLOG(2, LOG_VMAREAS, {
        if (mbi->State != MEM_FREE) {
            LOG(GLOBAL, LOG_VMAREAS, prot_is_executable(mbi->Protect) ? 1U : 2U,
                PFX"-"PFX" %s %s allocbase="PFX"\n",
                mbi->BaseAddress, ((app_pc)mbi->BaseAddress) + mbi->RegionSize,
                prot_string(mbi->Protect),
                (mbi->State == MEM_RESERVE) ? "reserve" : "commit ", mbi->AllocationBase);
        }
    });
    /* MEM_RESERVE has meaningless mbi->Protect field, so we ignore it here */
    if (mbi->State != MEM_COMMIT)
        return false;
    /* call these even if not marked as x, esp. the de-alloc, since some policy
     * could have them on future list or something
     */
    if (add) {
        return app_memory_allocation(dcontext, mbi->BaseAddress, mbi->RegionSize,
                                     osprot_to_memprot(mbi->Protect), from_image
                                     _IF_DEBUG(from_image ? "module" : "alloc"));
    } else {
        app_memory_deallocation(dcontext, mbi->BaseAddress, mbi->RegionSize,
                                false /* don't own thread_initexit_lock */,
                                from_image);
    }
    return false;
}

/* returns the number of executable areas added to DR's list */
int
find_executable_vm_areas()
{
    PBYTE pb = NULL;
    MEMORY_BASIC_INFORMATION mbi;
    PBYTE image_base = NULL;
    size_t view_size = 0;
    uint image_prot = 0;
    int num_executable = 0;
    LOG(GLOBAL, LOG_VMAREAS, 2, "Executable regions:\n");
    DOLOG(1, LOG_MEMSTATS, {
        mem_stats_snapshot();
    });
    /* Strategy: walk through every block in memory */
    while (query_virtual_memory(pb, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        ASSERT(pb == mbi.BaseAddress);
        if (mbi.State != MEM_FREE && mbi.Type == MEM_IMAGE && pb == mbi.AllocationBase) {
            /* first region in an image */
            MEMORY_BASIC_INFORMATION mbi_image;
            PBYTE pb_image = pb + mbi.RegionSize;
            image_base = pb;
            image_prot = mbi.Protect;

            /* We want to add to our module list right away so we can use it to
             * obtain info when processing each +x region. We need the view size
             * to call process_image with so we walk the image here. */
            /* FIXME - if it ever becomes a perf issue we can prob. change process_image
             * to not require the view size (by moving more things into
             * process_image_post_vmarea or remembering the queryies). */
            while (query_virtual_memory(pb_image, &mbi_image, sizeof(mbi_image)) ==
                   sizeof(mbi_image) &&
                   mbi_image.State != MEM_FREE && mbi_image.AllocationBase == pb) {
                ASSERT(mbi_image.Type == MEM_IMAGE);
                pb_image += mbi_image.RegionSize;
            }
            view_size = pb_image - pb;
            process_image(image_base, view_size, image_prot,
                          true /* add */, true /* rewalking */);
        }
        if (process_memory_region(NULL, &mbi, true/*init*/, true/*add*/))
            num_executable++;
        if (pb + mbi.RegionSize < pb)
            break;
        pb += mbi.RegionSize;
        if (image_base != NULL && pb >= image_base + view_size) {
            ASSERT(pb == image_base + view_size);
            process_image_post_vmarea(image_base, view_size, image_prot, 
                                      true /* add */, true /* rewalking */);
            image_base = NULL;
        }
    }
    ASSERT(image_base == NULL); /* check we don't have outstanding post call */
    LOG(GLOBAL, LOG_VMAREAS, 2, "\n");
    STATS_ADD(num_app_code_modules, num_executable);
    return num_executable;
}

/* all_memory_areas is linux only, dummy on win32 */
void all_memory_areas_lock()   { /* do nothing */ }
void all_memory_areas_unlock() { /* do nothing */ }
void update_all_memory_areas(app_pc start, app_pc end, uint prot) { /* do nothing */ }
bool remove_from_all_memory_areas(app_pc start, app_pc end) { return true; }

/* Processes a mapped-in section, which may or may not be an image.
 * if add is false, assumes caller has already called flush_fragments_and_remove_region
 * for all executable areas in region (probably just for entire super-region).
 * returns the number of executable areas added to DR's list
 */
int
process_mmap(dcontext_t *dcontext, app_pc pc, size_t size, bool add)
{
    PBYTE pb;
    MEMORY_BASIC_INFORMATION mbi;
    app_pc region_base;
    int num_executable = 0;
    bool image = false;
    uint image_prot = 0;

    ASSERT(!DYNAMO_OPTION(thin_client));
    LOG(GLOBAL, LOG_VMAREAS, 2, "%s exec areas in region "PFX"\n",
        add ? "adding" : "removing", pc);
    pb = (PBYTE) pc;
    if (query_virtual_memory(pb, &mbi, sizeof(mbi)) != sizeof(mbi))
        ASSERT(false);
    if (mbi.State == MEM_FREE)
        return num_executable;
    region_base = (app_pc) mbi.AllocationBase;
    if (mbi.Type == MEM_IMAGE) {
        process_image(region_base, size, mbi.Protect, add, false /* not rewalking */);
        image = true;
        image_prot = mbi.Protect;
    }
    /* Now update our vm areas executable region lists.
     * The protection flag doesn't tell us if there are executable areas inside,
     * must walk all the individual regions.
     * FIXME: for remove, optimize to do single flush but multiple area removals?
     */
    while (query_virtual_memory(pb, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_FREE || mbi.AllocationBase != region_base)
            break;
        if (process_memory_region(dcontext, &mbi, false/*!init*/, add)) {
            num_executable++;
            STATS_INC(num_app_code_modules);
        }
        if (pb + mbi.RegionSize < pb) /* overflow check */
            break;
        pb += mbi.RegionSize;
    }
    if (image) {
        process_image_post_vmarea(region_base, size, image_prot, add,
                                  false /* not rewalking */);
    }
    LOG(GLOBAL, LOG_SYSCALLS|LOG_VMAREAS, 3,
        "Executable areas are now:\n");
    DOLOG(3, LOG_SYSCALLS|LOG_VMAREAS, { print_executable_areas(GLOBAL); });
    return num_executable;
}

app_pc
get_image_entry()
{
    static app_pc image_entry_point = NULL;
    if (image_entry_point == NULL) {
        SELF_UNPROTECT_DATASEC(DATASEC_RARELY_PROT);
        /* Note that peb->ImageBaseAddress = GetModuleHandle(NULL) */
        image_entry_point = get_module_entry(get_own_peb()->ImageBaseAddress);
        SELF_PROTECT_DATASEC(DATASEC_RARELY_PROT);
    }
    return image_entry_point;
}

bool
check_for_image_entry(app_pc bb_start)
{
    if (!reached_image_entry && bb_start == get_image_entry()) {
        LOG(THREAD_GET, LOG_ALL, 1, "Reached image entry point "PFX"\n", bb_start);
        SELF_UNPROTECT_DATASEC(DATASEC_RARELY_PROT);
        reached_image_entry = true;
        SELF_PROTECT_DATASEC(DATASEC_RARELY_PROT);
        return true;
    }
    return false;
}

void
set_reached_image_entry()
{
    SELF_UNPROTECT_DATASEC(DATASEC_RARELY_PROT);
    reached_image_entry = true;
    SELF_PROTECT_DATASEC(DATASEC_RARELY_PROT);
}

bool
reached_image_entry_yet()
{
    return reached_image_entry;
}

/* converts a local_state_t offset to a segment offset */
ushort
os_tls_offset(ushort tls_offs)
{
    ASSERT_TRUNCATE(tls_offs, ushort, tls_local_state_offs + tls_offs);    
    return (ushort) (tls_local_state_offs + tls_offs);
}

local_state_t *
get_local_state()
{
    byte *teb_addr = (byte *) get_own_teb();
    return (local_state_t *) (teb_addr + tls_local_state_offs);
}

local_state_extended_t *
get_local_state_extended()
{
    ASSERT(DYNAMO_OPTION(ibl_table_in_tls));
    return (local_state_extended_t *) get_local_state();
}

/* returns the thread-private dcontext pointer for the calling thread */
dcontext_t*
get_thread_private_dcontext()
{
    /* This routine cannot be used before processwide os_init sets up the TLS index. */
    if (tls_dcontext_offs == TLS_UNINITIALIZED)
        return NULL;
    /*
     * We don't need to check whether this thread has been initialized under us - 
     * Windows sets the value to 0 for us, so we'll just return NULL.
     */
    return (dcontext_t *) get_tls(tls_dcontext_offs);
}

/* sets the thread-private dcontext pointer for the calling thread */
void
set_thread_private_dcontext(dcontext_t *dcontext)
{
    set_tls(tls_dcontext_offs, dcontext);
}

#ifdef WINDOWS_PC_SAMPLE
/* routines for pc sampling on windows */

profile_t *
create_profile(void *start, void *end, uint bucket_shift, dcontext_t *dcontext)
{
    profile_t *profile;
    size_t buffer_size = ((((ptr_uint_t)end - (ptr_uint_t)start) >> bucket_shift) + 1)
        * sizeof(uint);
    if (dcontext == NULL) {
        LOG(GLOBAL, LOG_PROFILE, 1, 
            "Creating global profile from "PFX" to "PFX" with shift %d "
            "for buffer size "SZFMT" bytes\n", start, end, bucket_shift, buffer_size); 
        profile = (profile_t *) global_heap_alloc(sizeof(*profile) 
                                                HEAPACCT(ACCT_STATS));
        profile->buffer = (uint *) UNPROTECTED_GLOBAL_ALLOC(buffer_size 
                                                            HEAPACCT(ACCT_STATS));
    } else {
        LOG(THREAD, LOG_PROFILE, 1, 
            "Creating local profile from "PFX" to "PFX" with shift %d "
            "(buffer size "SZFMT" bytes)\n", start, end, bucket_shift, buffer_size);
        profile = (profile_t *) heap_alloc(dcontext, sizeof(*profile) 
                                         HEAPACCT(ACCT_STATS));
        profile->buffer = (uint *) UNPROTECTED_LOCAL_ALLOC(dcontext, buffer_size 
                                                           HEAPACCT(ACCT_STATS));
    }
    memset(profile->buffer, 0, buffer_size);
    profile->start = start;
    profile->end = end;
    profile->bucket_shift = bucket_shift;
    profile->buffer_size = buffer_size;
    profile->enabled = false;
    profile->dcontext = dcontext;
    IF_X64(ASSERT(CHECK_TRUNCATE_TYPE_uint((byte *)end - (byte *)start)));
    IF_X64(ASSERT(CHECK_TRUNCATE_TYPE_uint(buffer_size)));
    profile->handle = nt_create_profile(NT_CURRENT_PROCESS, start, 
                                        (uint)((byte *)end-(byte *)start),
                                        profile->buffer,
                                        (uint)buffer_size, bucket_shift);
    return profile;
}

void
free_profile(profile_t *profile) 
{
    ASSERT(!profile->enabled);
    close_handle(profile->handle);
    if (profile->dcontext == NULL) {
        LOG(GLOBAL, LOG_PROFILE, 2, 
            "Freeing global profile from "PFX" to "PFX" with shift %d "
            "(buffer size "SZFMT" bytes)\n", 
            profile->start, profile->end, profile->bucket_shift, 
            profile->buffer_size); 
        UNPROTECTED_GLOBAL_FREE(profile->buffer, profile->buffer_size 
                                HEAPACCT(ACCT_STATS));
        global_heap_free(profile, sizeof(*profile) 
                         HEAPACCT(ACCT_STATS));
    } else {
        dcontext_t *dcontext = profile->dcontext;
        LOG(THREAD, LOG_PROFILE, 2, 
            "Freeing local profile from "PFX" to "PFX" with shift %d "
            "(buffer size "SZFMT" bytes)\n",
            profile->start, profile->end, profile->bucket_shift, 
            profile->buffer_size); 
        UNPROTECTED_LOCAL_FREE(dcontext, profile->buffer, profile->buffer_size 
                               HEAPACCT(ACCT_STATS));
        heap_free(dcontext, profile, sizeof(*profile) HEAPACCT(ACCT_STATS));
    }
}

void
start_profile(profile_t *profile)
{
    ASSERT(!profile->enabled);
    nt_start_profile(profile->handle);
    profile->enabled = true;
}

void
stop_profile(profile_t *profile)
{
    ASSERT(profile->enabled);
    nt_stop_profile(profile->handle);
    profile->enabled = false;
}

void
dump_profile_range(file_t file, profile_t *profile, byte *start, byte *end)
{
    uint i, step = (1 << profile->bucket_shift);
    uint start_i = (uint) (start - (byte *)profile->start) / step;
    uint end_i = (uint) (end - (byte *)profile->start) / step;
    IF_X64(ASSERT_TRUNCATE(start_i, uint, (start - (byte *)profile->start) / step));
    IF_X64(ASSERT_TRUNCATE(start_i, uint, (end - (byte *)profile->start) / step));

    print_file(file, "Profile Dump\nRange "PFX"-"PFX"\nStep "PFX
               " (%u-%u)\n", start, end, step, start_i, end_i);
    ASSERT(start_i < profile->buffer_size / sizeof(uint) &&
           end_i < profile->buffer_size / sizeof(uint));
    for (i = start_i; i <= end_i; i++) {
        if (profile->buffer[i] > 0)
            print_file(file, PFX" %10d\n", (byte *)profile->start + i * step, 
                       profile->buffer[i]);
    }
    print_file(file, "Finished Profile Dump\n");
}

void
dump_profile(file_t file, profile_t *profile)
{
    dump_profile_range(file, profile, (byte *) profile->start,
                       (byte *) profile->end);
}

uint
sum_profile_range(profile_t *profile, byte *start, byte *end)
{
    uint i, ret = 0, step = (1 << profile->bucket_shift);
    uint start_i = (uint) (start - (byte *)profile->start) / step;
    uint end_i = (uint) (end - (byte *)profile->start) / step;
    IF_X64(ASSERT_TRUNCATE(start_i, uint, (start - (byte *)profile->start) / step));
    IF_X64(ASSERT_TRUNCATE(start_i, uint, (end - (byte *)profile->start) / step));

    ASSERT(start_i < profile->buffer_size / sizeof(uint) &&
           end_i < profile->buffer_size / sizeof(uint));
    for (i = start_i; i <= end_i; i++) {
        if (profile->buffer[i] > 0)
            ret += profile->buffer[i];
    }
    return ret;
}

uint
sum_profile(profile_t *profile)
{
    return sum_profile_range(profile, (byte *) profile->start,
                             (byte *) profile->end);
}

void
reset_profile(profile_t *profile)
{
    memset(profile->buffer, 0, profile->buffer_size);
}
#endif

/* caller is required to handle thread synchronization */
/* see inject.c, this must be able to free an nt_allocate_virtual_memory
 * pointer */
void
os_heap_free(void *p, size_t size, heap_error_code_t *error_code)
{
    ASSERT(error_code != NULL);
    DOSTATS({
        if (!dynamo_exited_log_and_stats)
            LOG(GLOBAL, LOG_HEAP, 4, "os_heap_free: "SZFMT" bytes @ "PFX"\n", size, p);
    });
    *error_code = nt_free_virtual_memory(p);
    ASSERT(NT_SUCCESS(*error_code));
}

/* reserve virtual address space without committing swap space for it, 
   and of course no physical pages since it will never be touched */
void *
os_heap_reserve(void *preferred, size_t size, heap_error_code_t *error_code)
{
    void *p = preferred;
    ASSERT(error_code != NULL);
    /* should only be used on aligned pieces */
    ASSERT(size > 0 && ALIGNED(size, PAGE_SIZE));

    *error_code = nt_allocate_virtual_memory(&p, size, 
                                             PAGE_NOACCESS, MEMORY_RESERVE_ONLY);
    if (!NT_SUCCESS(*error_code))
        return NULL;
    LOG(GLOBAL, LOG_HEAP, 2, "os_heap_reserve: "SZFMT" bytes @ "PFX"\n", size, p);
    ASSERT(preferred == NULL || p == preferred); /* verify correct location */
    return p;
}

void *
os_heap_reserve_in_region(void *start, void *end, size_t size,
                          heap_error_code_t *error_code)
{
    byte *cur, *p = NULL;
    MEMORY_BASIC_INFORMATION mbi;

    ASSERT(start < end);

    LOG(GLOBAL, LOG_HEAP, 3,
        "os_heap_reserve_in_region: "SZFMT" bytes in "PFX"-"PFX"\n", size, start, end);

    /* if no restriction on location use regular os_heap_reserve() */
    if (start == (void *)PTR_UINT_0 && end == (void *)POINTER_MAX)
        return os_heap_reserve(NULL, size, error_code);

    /* walk bounds to find a suitable location */
    cur = (byte *)ALIGN_FORWARD(start, VM_ALLOCATION_BOUNDARY);
    *error_code = HEAP_ERROR_CANT_RESERVE_IN_REGION;
    while (p == NULL && cur + size <= (byte *)end &&
           query_virtual_memory(cur, &mbi, sizeof(mbi)) == sizeof(mbi)) { 
        if (mbi.State == MEM_FREE &&
            mbi.RegionSize - (cur - (byte *)mbi.BaseAddress) >= size) {
            /* we have a slot */
            p = (byte *)os_heap_reserve(cur, size, error_code);
            /* note - p could be NULL if someone grabbed some of the memory first */
            LOG(GLOBAL, LOG_HEAP, (p == NULL) ? 1U : 3U,
                "os_heap_reserve_in_region: got "PFX" trying to reserve "SZFMT" byte @ "
                PFX" in free region "PFX"-"PFX"\n",
                p, size, cur, mbi.BaseAddress, (byte *)mbi.BaseAddress + mbi.RegionSize);
        }
        cur = (byte *)ALIGN_FORWARD((byte *)mbi.BaseAddress + mbi.RegionSize,
                                    VM_ALLOCATION_BOUNDARY);
        /* check for overflow or 0 region size to prevent infinite loop */
        if (cur <= (byte *)mbi.BaseAddress)
            break; /* give up */
    }

    LOG(GLOBAL, LOG_HEAP, 2,
        "os_heap_reserve_in_region: reserved "SZFMT" bytes @ "PFX" in "PFX"-"PFX"\n",
        size, p, start, end);
    return p;
}

/* commit previously reserved with os_heap_reserve pages */
/* returns false when out of memory */
/* A replacement of os_heap_alloc can be constructed by using os_heap_reserve 
   and os_heap_commit on a subset of the reserved pages. */
/* caller is required to handle thread synchronization */
bool
os_heap_commit(void *p, size_t size, uint prot, heap_error_code_t *error_code)
{
    uint os_prot = memprot_to_osprot(prot);
    ASSERT(error_code != NULL);
    /* should only be used on aligned pieces */
    ASSERT(size > 0 && ALIGNED(size, PAGE_SIZE));
    ASSERT(p);

    LOG(GLOBAL, LOG_HEAP, 4, "os_heap_commit attempt: "SZFMT" bytes @ "PFX"\n", size, p);

    *error_code = nt_commit_virtual_memory(p, size, os_prot);
    if (!NT_SUCCESS(*error_code))
        return false;           /* out of memory */

    LOG(GLOBAL, LOG_HEAP, 3, "os_heap_commit: "SZFMT" bytes @ "PFX"\n", size, p);
    return true;
}

/* caller is required to handle thread synchronization and to update dynamo vm areas */
void
os_heap_decommit(void *p, size_t size, heap_error_code_t *error_code)
{
    ASSERT(error_code != NULL);
    if (!dynamo_exited)
        LOG(GLOBAL, LOG_HEAP, 3, "os_heap_decommit: "SZFMT" bytes @ "PFX"\n", size, p);

    *error_code = nt_decommit_virtual_memory(p, size);
    ASSERT(NT_SUCCESS(*error_code));
}

bool
os_heap_systemwide_overcommit(heap_error_code_t last_error_code)
{
    /* some error_codes may be worth retrying,
     * e.g. for win32/ STATUS_COMMITMENT_MINIMUM may be a good one
     * to retry, and maybe worth trying if systemwide memory
     * pressure has brought us to the limit 
     *
     * FIXME: case 7032 covers detecting this.  In fact a pagefile resize,
     *   will also cause an allocation failure, and TotalCommitLimit seems to be
     *   the current pagefile size + physical memory not used by the OS.
     *
     *     PeakCommitment should be close to TotalCommitLimit, unless
     *     the pagefile has been resized, or if the OS has trimmed the
     *     system cache and has made it available in the
     *     TotalCommitLimit
     */

    /* FIXME: conservative answer yes */
    return true;
}

bool
os_heap_get_commit_limit(size_t *commit_used, size_t *commit_limit)
{
    NTSTATUS res;
    SYSTEM_PERFORMANCE_INFORMATION sperf_info;

    STATS_INC(commit_limit_queries);
    res = query_system_info(SystemPerformanceInformation,
                            sizeof(sperf_info), &sperf_info);
    if (NT_SUCCESS(res)) {
        *commit_used = sperf_info.TotalCommittedPages;
        *commit_limit = sperf_info.TotalCommitLimit;
        return true;
    } else {
        ASSERT_NOT_REACHED();
        return false;
    }
}

/* yield the current thread */
void
thread_yield()
{
    /* main use in the busy path in mutex_lock  */
    nt_yield();
}

void
thread_sleep(uint64 milliseconds)
{
    LARGE_INTEGER liDueTime;
    /* negative == relative */
    liDueTime.QuadPart= - (int64)milliseconds * TIMER_UNITS_PER_MILLISECOND;
    nt_sleep(&liDueTime);
}

/* probably should have an option to stop all threads and then nt_sleep()
*/
int
os_timeout(int time_in_milliseconds)
{
    int res;
    LARGE_INTEGER liDueTime;
    liDueTime.QuadPart= - time_in_milliseconds * TIMER_UNITS_PER_MILLISECOND;
    LOG(THREAD_GET, LOG_ALL, 2, "os_timeout(%d)\n", time_in_milliseconds);

    res = nt_sleep(&liDueTime);
    LOG(THREAD_GET, LOG_ALL, 2, "Timeout expired res=%d.\n", res);
    return res;
}

bool
thread_suspend(thread_record_t *tr)
{
    return nt_thread_suspend(tr->handle, NULL);
}

bool
thread_resume(thread_record_t *tr)
{
    return nt_thread_resume(tr->handle, NULL);
}

bool
thread_terminate(thread_record_t *tr)
{
    return nt_terminate_thread(tr->handle, 0);
}

bool
thread_get_mcontext(thread_record_t *tr, dr_mcontext_t *mc)
{
    CONTEXT cxt;
    cxt.ContextFlags = CONTEXT_DR_STATE;
    if (thread_get_context(tr, &cxt)) {
        context_to_mcontext(mc, &cxt);
        return true;
    }
    return false;
}

bool
thread_set_mcontext(thread_record_t *tr, dr_mcontext_t *mc)
{
    CONTEXT cxt;
    cxt.ContextFlags = CONTEXT_DR_STATE;
    mcontext_to_context(&cxt, mc);
    return thread_set_context(tr->handle, &cxt);
}

bool
thread_get_context(thread_record_t *tr, CONTEXT *context)
{
    return NT_SUCCESS(nt_get_context(tr->handle, context));
}

bool
thread_set_context(thread_record_t *tr, CONTEXT *context)
{
    return NT_SUCCESS(nt_set_context(tr->handle, context));
}

/* Takes an os-specific context */
void
thread_set_self_context(void *cxt)
{
    /* We use NtContinue to avoid privilege issues with NtSetContext */
    nt_continue((CONTEXT *)cxt);
    ASSERT_NOT_REACHED();
}

/* Takes a dr_mcontext_t */
void
thread_set_self_mcontext(dr_mcontext_t *mc)
{
    CONTEXT cxt;
    cxt.ContextFlags = CONTEXT_DR_STATE;
    mcontext_to_context(&cxt, mc);
    thread_set_self_context(&cxt);
    ASSERT_NOT_REACHED();
}

int
get_num_processors()
{
    static uint num_cpu = 0;         /* cached value */
    if (!num_cpu) {
        SYSTEM_BASIC_INFORMATION sbasic_info;
        NTSTATUS result = query_system_info(SystemBasicInformation, 
                                            sizeof(SYSTEM_BASIC_INFORMATION), 
                                            &sbasic_info);
        if (!NT_SUCCESS(result))
            num_cpu = 1;               /* assume single CPU */
        else 
            num_cpu = sbasic_info.NumberProcessors;
        ASSERT(num_cpu);
    }
    return num_cpu;
}

/* Static to save stack space, is initialized at first call to debugbox or
 * at os_init (whichever is earlier), we are guaranteed to be single threaded
 * at os_init so no race conditions even though there shouldn't be any anyways
 * unless snwprintf does something funny with the buffer. This also ensures
 * that the static buffers in get_application_name and get_application_pid
 * get initialized while we are still single threaded. */
static wchar_t debugbox_title_buf[MAXIMUM_PATH+64];
static void init_debugbox_title_buf()
{
    snwprintf(debugbox_title_buf, BUFFER_SIZE_ELEMENTS(debugbox_title_buf), 
              L_PRODUCT_NAME L" Notice: %hs(%hs)", 
              get_application_name(), get_application_pid());
    NULL_TERMINATE_BUFFER(debugbox_title_buf);
}

/* Static buffer for debugbox.  If stack-allocated, debugbox is one of
 * the big space hogs when reporting a crash and we risk exhausting
 * the stack.
 */
DECLARE_NEVERPROT_VAR(static wchar_t debugbox_msg_buf[MAX_LOG_LENGTH], {0});

/* draw a message box on the screen with msg */
int
debugbox(char *msg)
{
    int res;

    if (debugbox_title_buf[0] == 0)
        init_debugbox_title_buf();

    /* FIXME: If we hit an assert in nt_messagebox, we'll deadlock when
     * we come back here.
     */
    mutex_lock(&debugbox_lock);

    snwprintf(debugbox_msg_buf, BUFFER_SIZE_ELEMENTS(debugbox_msg_buf), L"%hs", msg);
    NULL_TERMINATE_BUFFER(debugbox_msg_buf);
    res = nt_messagebox(debugbox_msg_buf, debugbox_title_buf);
    
    mutex_unlock(&debugbox_lock);

    return res;
}

#ifdef FANCY_COUNTDOWN          /* NOT IMPLEMENTED */
/* Fancy countdown box for a message with timeout */

// This is STATIC window control ID for a message box
#define ID_MSGBOX_STATIC_TEXT    0x0000ffff

typedef struct {
    char *message;
    char *title;
    HANDLE timer;
    int seconds_left;
    bool done;
} timeout_context_t;

#define print_timeout_message(buf, context) \
   snprintf(buf, sizeof(buf), "%s\n""You have %d seconds to respond", \
            ((timeout_context_t*)context)->message, ((timeout_context_t*)context)->seconds_left);

/* FIXME: Be careful about creating a thread -- make sure we 
don't intercept its asynch events.  Not clear how to do that -- you can
turn off interception once it's created, but to not intercept its init
APC, currently all you can do is globally turn off event interception,
or else try to identify it when we see the init APC. */

/* based on Richter's 11-TimedMsgBox */
DWORD WINAPI 
message_box_timeout_thread(void *context) {
    timeout_context_t *tcontext = (timeout_context_t*)context;
    return 0;

    LOG(GLOBAL, LOG_ALL, 2, "message_box_timeout_thread(%d)\n", tcontext->seconds_left);
    do {
        WaitForSingleObject(tcontext->timer, tcontext->seconds_left * 1000);
        {
            HWND hwnd = FindWindow(NULL, tcontext->title);

            LOG(THREAD_GET, LOG_ALL, 2, "message_box_timeout_thread(%d) hwnd="PIFX"\n", 
                tcontext->seconds_left, hwnd);
            if (hwnd != NULL) {
                char countdown[MAX_LOG_LENGTH];
                tcontext->seconds_left--;
                print_timeout_message(countdown, context);
                SetDlgItemText(hwnd, ID_MSGBOX_STATIC_TEXT, countdown);

                if (tcontext->seconds_left == 0) {
                    /* timeout */
                    EndDialog(hwnd, IDOK);
                    return 1;
                }
            }
        }
    } while(!tcontext->done);
    return 0;
}

int
os_countdown_messagebox(char *message, int time_in_milliseconds)
{
    char title[MAXIMUM_PATH+64];
    char buf[MAX_LOG_LENGTH];

    LONG update_period = 1000; /* milliseconds = 1s */
    uint seconds_left = time_in_milliseconds / update_period;
    LARGE_INTEGER liDueTime;
    HANDLE htimer;
    HANDLE hthread;
    timeout_context_t context = {message, title, 0, seconds_left, false};
    int res;

    LOG(THREAD_GET, LOG_ALL, 2, 
        "os_countdown_messagebox(%s, %d)\n", message, time_in_milliseconds); 
    ASSERT_NOT_IMPLEMENTED(false);

    get_debugbox_title(title, sizeof(title));
    print_timeout_message(buf, &context);

    liDueTime.QuadPart= - update_period * TIMER_UNITS_PER_MILLISECOND;

    /* create a waitable timer to get signaled periodically */
    htimer = nt_create_and_set_timer(&liDueTime, update_period);
    context.timer = htimer;
    
    hthread = CreateThread(NULL, 0, &message_box_timeout_thread, NULL, 0, NULL);
    LOG(THREAD_GET, LOG_ALL, 2, 
        "os_countdown_messagebox(%s, %d)\n", message, time_in_milliseconds); 

    debugbox(buf);
    context.done = true;

    WaitForSingleObject(hthread, INFINITE);

    close_handle(htimer);
    close_handle(hthread);

    return 0;
}
#else
int
os_countdown_messagebox(char *message, int time_in_milliseconds) {
    char buf[MAX_LOG_LENGTH];
    snprintf(buf, sizeof(buf), "%sTimeout ignored", message);
    NULL_TERMINATE_BUFFER(buf);
    debugbox(buf);
    return 0;
}
#endif /* FANCY_COUNTDOWN */

#if defined(CLIENT_INTERFACE) || defined(HOT_PATCHING_INTERFACE)
shlib_handle_t 
load_shared_library(char *name)
{
    wchar_t buf[MAX_PATH];
    snwprintf(buf, BUFFER_SIZE_ELEMENTS(buf), L"%hs", name);
    NULL_TERMINATE_BUFFER(buf);
    return load_library(buf);
}
#endif

#if defined(CLIENT_INTERFACE)
shlib_routine_ptr_t
lookup_library_routine(shlib_handle_t lib, char *name)
{
    return (shlib_routine_ptr_t)get_proc_address(lib, name);
}

void
unload_shared_library(shlib_handle_t lib)
{
   free_library(lib);
}

void
shared_library_error(char *buf, int maxlen)
{
    /* FIXME : this routine does nothing. It used to use kernel32 FormatMessage
     * to report errors, but now that we are kernel32 independant that will no
     * longer work. Would be nice if we could do something with the nt status
     * codes, but unclear how to propagate them to here. */
    buf[0] = '\0';
}

bool
shared_library_bounds(IN shlib_handle_t lib, IN byte *addr,
                      OUT byte **start, OUT byte **end)
{
    size_t sz = get_allocation_size(lib, start);
    ASSERT(start != NULL && end != NULL);
    *end = *start + sz;
    ASSERT(addr == NULL || (addr >= *start && addr < *end));
    return true;
}
#endif /* defined(CLIENT_INTERFACE) */

/* Returns base of the "allocation region" containing pc for allocated memory, 
 * Note the current protection settings may not be uniform in the whole region.
 * Returns NULL for free memory or invalid user mode addresses.
 * Use get_allocation_size() when size is also needed.
 */
byte *
get_allocation_base(byte *pc)
{
    MEMORY_BASIC_INFORMATION mbi;
    size_t res = query_virtual_memory(pc, &mbi, sizeof(mbi));
    if (res != sizeof(mbi)) {
        /* invalid address given, e.g. POINTER_MAX */
        return NULL;
    }
    if (mbi.State == MEM_FREE) {
        ASSERT_CURIOSITY(mbi.BaseAddress == (byte*)ALIGN_BACKWARD(pc, PAGE_SIZE));
        return NULL;
    }

    return mbi.AllocationBase;
}

/* See comments below -- this max will go away once we're sure
 * we won't infinite loop.  Until then we keep it very large
 * (we've seen 128MB with a ton of single-page regions inside in case 4502)
 * such that should never hit it (@ 1 block per page will hit 4GB first)
 */
enum { MAX_QUERY_VM_BLOCKS = 512*1024 };

/* Returns size of the "allocation region" containing pc
 * Note that this may include several pieces of memory with different
 * protection and state attributes.
 * If base_pc != NULL returns base pc as well 

 * If memory is free we set base_pc to NULL, but return free region
 * size - note that we can't efficiently go backwards to find the
 * maximum possible allocation size in a free hole.
 */
size_t
get_allocation_size(byte *pc, byte **base_pc)
{
    PBYTE pb = (PBYTE) pc;
    MEMORY_BASIC_INFORMATION mbi;
    PVOID region_base;
    PVOID pb_base;
    size_t pb_size;
    size_t res;
    int num_blocks = 0;
    size_t size;

    res = query_virtual_memory(pb, &mbi, sizeof(mbi));
    if (res != sizeof(mbi) /* invalid address given, e.g. POINTER_MAX */) {
        if (base_pc != NULL)
            *base_pc = NULL;
        return 0;
    }

    if (mbi.State == MEM_FREE /* free memory doesn't have AllocationBase */) {
        if (base_pc != NULL)
            *base_pc = NULL;
        /* note free region from requested ALIGN_BACKWARD(pc base */
        return mbi.RegionSize;
    }

    pb_base = mbi.BaseAddress;
    pb_size = mbi.RegionSize;
    region_base = mbi.AllocationBase;
    /* start beyond queried region */
    pb = (byte *) pb_base + mbi.RegionSize;
    size = (app_pc)pb - (app_pc)region_base;

    /* must keep querying contiguous blocks until reach next region
     * to find this region's size
     */
    LOG(GLOBAL, LOG_VMAREAS, 3,
        "get_allocation_size pc="PFX" base="PFX" region="PFX" size="PIFX"\n",
        pc, pb_base, region_base, mbi.RegionSize);
    do {
        res = query_virtual_memory(pb, &mbi, sizeof(mbi));
        if (res != sizeof(mbi) || mbi.State == MEM_FREE || mbi.AllocationBase != region_base)
            break;
        ASSERT(mbi.RegionSize > 0); /* if > 0, we will NOT infinite loop */
        size += mbi.RegionSize;
        if (pb + mbi.RegionSize < pb)
            break;
        pb += mbi.RegionSize;
        /* WARNING: if app is changing memory at same time as we're examining
         * it, we could have problems: but, if region becomes free, we'll break,
         * and so long as RegionSize > 0, we should make progress and hit
         * end of address space in worst case -- so we shouldn't need this
         * num_blocks max, but we'll keep it for now.  FIXME.
         */
        num_blocks++;
    } while (num_blocks < MAX_QUERY_VM_BLOCKS);
    ASSERT_CURIOSITY(num_blocks < MAX_QUERY_VM_BLOCKS);
    /* size may push to overflow to 0 if at end of address space */
    ASSERT((app_pc)region_base + size > pc || (app_pc)region_base + size == NULL);
    if (base_pc != NULL)
        *base_pc = (byte *) region_base;
    return size;
}

/* Returns size and writability of the memory area (not allocation region)
 * containing pc.  This is a single memory area all from the same allocation
 * region and all with the same protection and state attributes.
 * If base_pc != NULL returns base pc of the area.
 */
bool
get_memory_info(const byte *pc, byte **base_pc, size_t *size, uint *prot)
{
    MEMORY_BASIC_INFORMATION mbi;
    size_t res = query_virtual_memory(pc, &mbi, sizeof(mbi));
    if (res != sizeof(mbi) || mbi.State == MEM_FREE)
        return false;
    if (base_pc != NULL)
        *base_pc = mbi.BaseAddress;
    if (size != NULL)
        *size = mbi.RegionSize;
    if (prot != NULL) {
        *prot = osprot_to_memprot(mbi.Protect);
    }
    return true;
}

/* It is ok to pass NULL for dcontext */
bool
get_stack_bounds(dcontext_t *dcontext, byte **base, byte **top)
{
    os_thread_data_t *ostd = NULL;
    if (dcontext != NULL) {
        ostd = (os_thread_data_t *) dcontext->os_field;
        if (ostd->teb_stack_no_longer_valid) {
            /* Typically this means we are on NT or 2k and the TEB is being used
             * as the stack for ExitThread. Xref fixme in check_for_stack_free()
             * about possibly handling this differently. */
            return false;
        }
    }
    if (dcontext == NULL || ostd->stack_base == NULL) {
        byte * stack_base = NULL;
        byte * stack_top = NULL;
        /* This only works if the dcontext is for the current thread. */
        ASSERT(dcontext == NULL || dcontext == get_thread_private_dcontext());
        /* use the TIB fields:
         *   PVOID   pvStackUserTop;     // 04h Top of user stack
         *   PVOID   pvStackUserBase;    // 08h Base of user stack
         * and assume fs is always a valid TIB pointer when called here
         */
        stack_top = (byte *) get_tls(TOP_STACK_TIB_OFFSET);
        stack_base = (byte *) get_tls(BASE_STACK_TIB_OFFSET);
        LOG(THREAD, LOG_THREADS, 1, "app stack now is "PFX"-"PFX"\n",
            stack_base, stack_top); /* NULL dcontext => nop */
        /* we only have current base, we need to find reserved base */
        stack_base = get_allocation_base(stack_base);
        LOG(THREAD, LOG_THREADS, 1, "app stack region is "PFX"-"PFX"\n",
            stack_base, stack_top); /* NULL dcontext => nop */
        /* FIXME - make curiosity? prob. could create a thread with no official
         * stack and we would largely be fine with that. */
        ASSERT(stack_base != NULL);
        ASSERT(stack_base < stack_top);
        ASSERT(get_allocation_base(stack_top - 1) == stack_base &&
               (get_allocation_base(stack_top) != stack_base ||
                /* PR 252008: for WOW64 nudges we allocate an extra page.
                 * We would test dcontext->nudge_thread but that's not set yet. */
                is_wow64_process(NT_CURRENT_PROCESS)));
        if (dcontext == NULL) {
            if (base != NULL)
                *base = stack_base;
            if (top != NULL)
                *top = stack_top;
            return true;
        }
        ostd->stack_base = stack_base;
        ostd->stack_top = stack_top;
    }
    if (base != NULL)
        *base = ostd->stack_base;
    if (top != NULL)
        *top = ostd->stack_top;
    return true;
}

/*
winnt.h:#define PAGE_READONLY 2
winnt.h:#define PAGE_READWRITE 4
winnt.h:#define PAGE_WRITECOPY 8
winnt.h:#define PAGE_EXECUTE 16
winnt.h:#define PAGE_EXECUTE_READ 32
winnt.h:#define PAGE_EXECUTE_READWRITE 64
winnt.h:#define PAGE_GUARD 256
winnt.h:#define PAGE_NOACCESS 1
winnt.h:#define PAGE_NOCACHE 512
winnt.h:#define PAGE_EXECUTE_WRITECOPY 128
*/

/* is_readable_without_exception checks to see that all bytes with addresses
 * from pc to pc+size-1 are readable and that reading from there won't
 * generate an exception.  this is a stronger check than
 * !not_readable() below.
 * FIXME : beware of multi-thread races, just because this returns true, 
 * doesn't mean another thread can't make the region unreadable between the 
 * check here and the actual read later.  See safe_read() as an alt.
 */
static bool 
query_is_readable_without_exception(byte *pc, size_t size)
{
    MEMORY_BASIC_INFORMATION mbi;
    byte *check_pc = (byte *) ALIGN_BACKWARD(pc, PAGE_SIZE);
    size_t res;
    if (size > (size_t)((byte *)POINTER_MAX - pc))
        size = (byte *)POINTER_MAX - pc;
    do {
        res = query_virtual_memory(check_pc, &mbi, sizeof(mbi));
        if (res != sizeof(mbi)) {
            return false;
        } else {
            if (mbi.State != MEM_COMMIT ||
                TEST(PAGE_GUARD, mbi.Protect) ||
                !prot_is_readable(mbi.Protect))
                return false;
        }
        /* FIXME: this routine can walk by mbi.RegionSize instead of pages */
        check_pc += PAGE_SIZE;
    } while (check_pc != 0/*overflow*/ && check_pc < pc+size);
    return true;
}

/* On Windows, same as is_readable_without_exception */
bool 
is_readable_without_exception_query_os(byte *pc, size_t size)
{
    return is_readable_without_exception(pc, size);
}

/* Reads size bytes starting at base and puts them in out_buf, this is safe
 * to call even if the memory at base is unreadable, returns true if the 
 * read succeeded */
/* FIXME : This avoids the races with an is_readable_without_exception followed
 * by a read. We get the os to do the read for us via ReadVirtualMemory, 
 * however this is still much slower then a structured exception handling 
 * solution since we expect this to succeed most of the time.  Ref PR 206278 
 * and 208562 on using the faster TRY/EXCEPT. */
bool
safe_read_ex(const void *base, size_t size, void *out_buf, size_t *bytes_read)
{
    if (bytes_read != NULL)
        *bytes_read = 0;
    STATS_INC(num_safe_reads);
    return nt_read_virtual_memory(NT_CURRENT_PROCESS, base, out_buf, size, bytes_read);
}

/* FIXME - fold this together with safe_read_ex() (is a lot of places to update) */
bool
safe_read(const void *base, size_t size, void *out_buf)
{
    size_t bytes_read = 0;
    return (safe_read_ex(base, size, out_buf, &bytes_read) && bytes_read == size);
}

/* Writes size bytes starting at base from in_buf, this is safe
 * to call even if the memory at base is unreadable, returns true if the 
 * write succeeded.  See safe_read_ex() on using more performant TRY/EXCEPT. */
bool
safe_write_ex(void *base, size_t size, const void *in_buf, size_t *bytes_written)
{
    if (bytes_written != NULL)
        *bytes_written = 0;
    STATS_INC(num_safe_writes);
    return nt_write_virtual_memory(NT_CURRENT_PROCESS, base, in_buf,
                                   size, bytes_written);
}

/* FIXME - fold this together with safe_write_ex() (is a lot of places to update) */
bool
safe_write(void *base, size_t size, const void *in_buf)
{
    size_t written_bytes = 0;
    return (safe_write_ex(base, size, in_buf, &written_bytes) && written_bytes == size);
}

/* unlike get_memory_info() we return osprot preserving complete
 * protection info.  Note errors or bad addresses are ignored and
 * return PAGE_NOACCESS instead.  If the difference between invalid
 * address or PAGE_NOACCESS is essential users must use
 * query_virtual_memory()
 */
uint
get_current_protection(byte *pc)
{
    PBYTE pb = (PBYTE) pc;
    MEMORY_BASIC_INFORMATION mbi;
    size_t res = query_virtual_memory(pb, &mbi, sizeof(mbi));
    ASSERT(res == sizeof(mbi));
    ASSERT(mbi.State != MEM_FREE); /* caller assumes this is a valid page */
    if (res != sizeof(mbi) || mbi.State == MEM_FREE) {
        /* note we could also return 0 since PAGE_NOACCESS is 1 */
        ASSERT_CURIOSITY(false && "in get_memory_osprot");
        return PAGE_NOACCESS;
    }
    return mbi.Protect;
}

/* see note on is_readable_without_exception for differences between the two
 * returns true if any byte with address from pc to pc+size-1 is not_readable
 * FIXME: reverse the logic to make this is_readable
 * Also CHECK that we actually need this routine
 */
bool
not_readable(byte *pc, size_t size)
{
    MEMORY_BASIC_INFORMATION mbi;
    byte *check_pc = (byte *) ALIGN_BACKWARD(pc, PAGE_SIZE);
    size_t res;
    if (size > (size_t)((byte *)POINTER_MAX - pc))
        size = (byte *)POINTER_MAX - pc;
    while (check_pc != 0/*overflow*/ && check_pc < pc+size) {
        res = query_virtual_memory(check_pc, &mbi, sizeof(mbi));
        if (res != sizeof(mbi) || mbi.State == MEM_FREE)
            return true;
        else if (!prot_is_readable(mbi.Protect))
            return true;
        check_pc += PAGE_SIZE;
    }
    return false;
}

void
mark_page_as_guard(byte *pc)
{
    uint old_prot;
    int res;
    /* NOACCESS combined w/ GUARD is invalid -- apparently you specify what you want
     * after the guard triggers
     */
    uint flags = PAGE_READWRITE | PAGE_GUARD;
    ASSERT(ALIGNED(pc, PAGE_SIZE));
    res = protect_virtual_memory((void *) pc, PAGE_SIZE, flags, &old_prot);
    ASSERT(res);
}

/* Change page protection for pc:pc+size.
 * If set is false, makes [un]writable depending on add_writable argument,
 * preserving other flags; else, sets protection to new_prot.
 * If cow is true and set is false and writable is true, sets to
 * be not only writable but copy-on-write.
 * Requires pc and size are multiples of the
 * PAGE_SIZE.  
 *
 * Returns true if all protection requests succeeded, false if
 * protection on any subregion fails: all callers that make memory
 * writable should be able to handle the unrecoverable yet failure on
 * out of commit memory.  
 * changed_protection is set to true if changes were necessary, or
 * false if protection already meets requirements.  Note that any
 * reserved yet not committed subregion will be skipped (and change
 * protection is not needed).
 */
static bool
internal_change_protection(byte *start, size_t requested_size, bool set,
                           bool writable, bool cow, uint new_prot, 
                           bool *changed_protection /* OUT */)
{
    byte *pc = start;
    size_t remaining_size = requested_size;
    bool changed_permissions = false;
    bool subregions_failed = false;

    /* while this routine may allow crossing allocation bases 
     * it is supposed to be in error, a MEM_FREE block would terminate it */
    DEBUG_DECLARE(app_pc first_allocation_base = NULL;)

    /* we no longer allow you to pass in 0 */
    ASSERT(requested_size > 0);
    ASSERT(ALIGNED(start, PAGE_SIZE) && ALIGNED(requested_size, PAGE_SIZE));

    /* we can call protect_virtual_memory only on regions that have
     * the same attributes, we have to split the requested region into
     * multiple proper subregions
     */
    do {
        MEMORY_BASIC_INFORMATION mbi;
        uint old_prot;
        size_t res;
        uint flags, new_flags;
        size_t allow_size;        /* remaining size with same privileges */
        size_t subregion_size;    /* should be a subregion <= allow_size */

        ASSERT(remaining_size > 0);

        /* FIXME: note that a faster version of this routine when we
         * know the desired flags can do without the
         * query_virtual_memory() calls and only needs to process the
         * results of protect_virtual_memory() to decide whether needs
         * more iterations.
         */

        /* needed for current flags and region size */
        res = query_virtual_memory((PBYTE)pc, &mbi, sizeof(mbi));
        if (res != sizeof(mbi)) {
            /* can get here if executing from kernel address space - case 9022 */
            goto finish;
        }
        ASSERT(res == sizeof(mbi));
        ASSERT(mbi.State != MEM_FREE);
        ASSERT(mbi.State == MEM_COMMIT || mbi.State == MEM_RESERVE);
        ASSERT(ALIGNED(pc, PAGE_SIZE) && ALIGNED(remaining_size, PAGE_SIZE));
        ASSERT(first_allocation_base == NULL ||
               first_allocation_base == mbi.AllocationBase);
        DODEBUG({first_allocation_base = mbi.AllocationBase;});
        ASSERT(pc == mbi.BaseAddress); /* if pc is page aligned, but just in case */
        allow_size = mbi.RegionSize - (pc - (byte *)mbi.BaseAddress);

        /* to maintain old prot flags, 
         * we have to do each os region separately */
        if (remaining_size > allow_size) {
            LOG(THREAD_GET, LOG_VMAREAS, 2,
                "WARNING: make_%swritable "PFX": param size "PIFX" vs. "
                "mbi size "PIFX" base "PFX"\n",
                writable ? "" : "un", pc, remaining_size, mbi.RegionSize, mbi.BaseAddress);
            /* we see this on make_writable when we've merged regions that we made read-only
             * and we go to restore their permissions.
             * we can see it for the same region many times in a row (e.g., on javac in SPECJVM98),
             */
            /* flag in GLOBAL LOG */
            LOG(GLOBAL, LOG_VMAREAS, pc == start ? 1U : 2U, 
                "make_%swritable called with size "PFX
                "> region size "PFX" at pc "PFX"\n",
                writable ? "" : "un", remaining_size, allow_size, pc);
            /* needed most commonly when a PAGE_WRITECOPY breaks up a
             * region or when MEM_RESERVE subregion is processed,
             * for the time being adding a curiosity on any other use */

            /* for this invocation, just do region size */
            subregion_size = allow_size;
        } else {
            subregion_size = remaining_size;
        }

        ASSERT( subregion_size <= allow_size);

        LOG(THREAD_GET, LOG_VMAREAS, 3, "make_%swritable: pc "PFX"-"PFX
            ", currently %s %s\n",
            writable ? "" : "un", pc, pc+subregion_size, prot_string(mbi.Protect), 
            mbi.State == MEM_COMMIT ? "committed" : "reserved");
        /* mbi.Protect is defined only for mbi.State == MEM_COMMIT 
         * we use gratuitously in this LOG
         */

        if (mbi.State == MEM_RESERVE) {
            LOG(THREAD_GET, LOG_VMAREAS, 2, 
                "make_%swritable: WARNING skipping reserved region "PFX"-"PFX"\n",
                pc, pc+subregion_size);
            /* There is nothing we can do about reserved memory.
             * Assume nobody will really reference this uncomitted
             * memory, and in case it is caller error, that we'll find
             * out on write.
             */
            goto skip;
        }
        if (mbi.State == MEM_FREE) {
            /* now this is always supposed to be an error */
            ASSERT_NOT_REACHED();
            subregions_failed = true;
            goto finish;
        }

        flags = mbi.Protect & ~PAGE_PROTECTION_QUALIFIERS;

        if (set) {
            new_flags = new_prot;
        } else if (writable) {
            switch (flags) {
            case PAGE_NOACCESS:          new_flags = PAGE_READWRITE; break;
            case PAGE_READONLY:          new_flags = PAGE_READWRITE; break;
            case PAGE_READWRITE:         goto skip;
            case PAGE_WRITECOPY:         goto skip;
            case PAGE_EXECUTE:           new_flags = PAGE_EXECUTE_READWRITE; break;
            case PAGE_EXECUTE_READ:      new_flags = PAGE_EXECUTE_READWRITE; break;
            case PAGE_EXECUTE_READWRITE: goto skip;
            case PAGE_EXECUTE_WRITECOPY: goto skip;
            default:
                ASSERT_NOT_REACHED();
                /* not possible since we handle MEM_RESERVE earlier */
                /* do not attempt changing permissions to be robust */
                goto skip;
            }
            if (cow)
                new_flags = osprot_add_writecopy(new_flags);
        } else {
            switch (flags) {
            case PAGE_NOACCESS:          goto skip;
            case PAGE_READONLY:          goto skip;
            case PAGE_READWRITE:         new_flags = PAGE_READONLY; break;
            case PAGE_WRITECOPY:         new_flags = PAGE_READONLY; break;
            case PAGE_EXECUTE:           goto skip;
            case PAGE_EXECUTE_READ:      goto skip;
            case PAGE_EXECUTE_READWRITE: new_flags = PAGE_EXECUTE_READ; break;
            case PAGE_EXECUTE_WRITECOPY: new_flags = PAGE_EXECUTE_READ; break;
            default:
                ASSERT_NOT_REACHED();
                goto skip;
            }
        }
        /* preserve other flags */
        new_flags = (mbi.Protect & ~flags) | new_flags;

        DOSTATS({
            /* once on each side of prot, to get on right side of writability */
            if (!writable) {
                STATS_INC(protection_change_calls);
                STATS_ADD(protection_change_pages, subregion_size / PAGE_SIZE);
            }
        });
        res = protect_virtual_memory((void *)pc, subregion_size, new_flags, &old_prot);
        if (!res) {
            /* FIXME: we may want to really make sure that we are out
             * of commit memory, if we are marking this up as failure
             * here
             */
            subregions_failed = true;
            /* FIXME: case 10551 we may want to use the techniques in
             * vmm_heap_commit to wait a little for someone else to
             * free up memory, or free any of our own.
             */
        }
        /* we ignore any failures due to TOCTOU races on subregion protection */
        ASSERT_CURIOSITY(res && "protect_virtual_memory failed");
        DOSTATS({
            /* once on each side of prot, to get on right side of writability */
            if (writable) {
                STATS_INC(protection_change_calls);
                STATS_ADD(protection_change_pages, subregion_size / PAGE_SIZE);
            }
        });
        changed_permissions = true;
    skip:
        pc += subregion_size;
        remaining_size -= subregion_size;
    } while ( remaining_size > 0);

 finish:
    if (changed_protection != NULL)
        *changed_protection = changed_permissions;
    return !subregions_failed;
}

/* Set protections on memory region starting at pc of length size 
 * (padded to page boundaries).
 * returns false on failure, e.g. out of commit memory
 */
bool
set_protection(byte *pc, size_t size, uint prot)
{
    byte *start_page = (byte *)ALIGN_BACKWARD(pc, PAGE_SIZE);
    size_t num_bytes = ALIGN_FORWARD(size + (pc - start_page), PAGE_SIZE);
    return internal_change_protection(start_page, num_bytes, true/*set*/,
                                      false/*ignored*/, false/*ignored*/,
                                      memprot_to_osprot(prot), NULL);
}

/* Change protections on memory region starting at pc of length size 
 * (padded to page boundaries).  This method is meant to be used on DR memory 
 * as part of protect from app and is safe with respect to stats and the data
 * segment.
 */
bool
change_protection(byte *pc, size_t size, bool writable)
{
    byte *start_page = (byte *)ALIGN_BACKWARD(pc, PAGE_SIZE);
    size_t num_bytes = ALIGN_FORWARD(size + (pc - start_page), PAGE_SIZE);
    return internal_change_protection(start_page, num_bytes, false/*relative*/, 
                                      writable, false/*not cow*/, 0, NULL);
}

/* makes pc-:c+size (page_padded) writable preserving other flags */
bool
make_hookable(byte *pc, size_t size, bool *changed_prot)
{
    byte *start_pc = (byte *)ALIGN_BACKWARD(pc, PAGE_SIZE);
    size_t num_bytes = ALIGN_FORWARD(size + (pc - start_pc), PAGE_SIZE);
    ASSERT(changed_prot != NULL);
    return internal_change_protection(start_pc, num_bytes,
                                      false/*relative*/, true, false/*not cow*/, 0, 
                                      changed_prot);
}

/* if changed_prot makes pc:pc+size (page padded) unwritable preserving 
 * other flags */
void
make_unhookable(byte *pc, size_t size, bool changed_prot)
{
    if (changed_prot) {
        byte *start_pc = (byte *)ALIGN_BACKWARD(pc, PAGE_SIZE);
        size_t num_bytes = ALIGN_FORWARD(size + (pc - start_pc), PAGE_SIZE);
        internal_change_protection(start_pc, num_bytes, false/*relative*/, 
                                   false, false/*ignored*/, 0, NULL);
    }
}

/* requires that pc is page aligned and size is multiple of the page size
 * and marks that memory writable, preserves other flags */
/* returns false if out of commit memory! */
bool
make_writable(byte *pc, size_t size)
{
    return internal_change_protection(pc, size, false/*relative*/, true,
                                      false/*not cow*/, 0, NULL);
}

/* requires that pc is page aligned and size is multiple of the page size
 * and marks that memory writable and copy-on-write, preserving other flags.
 * note: only usable if allocated COW
 */
bool
make_copy_on_writable(byte *pc, size_t size)
{
    return internal_change_protection(pc, size, false/*relative*/, true,
                                      true/*cow*/, 0, NULL);
}

/* requires that pc is page aligned and size is multiple of the page size
 * and marks that memory NOT writable, preserves other flags */
void
make_unwritable(byte *pc, size_t size)
{
    internal_change_protection(pc, size, false/*relative*/, false, false/*ignored*/,
                               0, NULL);
}

static bool
convert_to_NT_file_path(OUT wchar_t *buf, IN const char *fname,
                        IN size_t buf_len/*# elements*/)
{
    bool is_UNC = false;
    const char *name = fname;
    uint i;
    /* need nt file path, prepend \??\ so is \??\c:\.... make sure everyone
     * gives us a fullly qualified absolute path, no . .. relative etc.
     * For UNC names(//server/name), the path should be \??\UNC\server\name. */
    /* NOTE - for process control we use an app path (image location) with this routine
     * so we should handle all possible file name prefixes, we've seen - 
     * c:\ \??\c:\ \\?\c:\ \\server \??\UNC\server \\?\UNC\server */
    /* FIXME - could we ever get any other path formats here (xref case 9146 and the
     * reactos src.  See DEVICE_PATH \\.\foo, UNC_DOT_PATH \\., etc. */ 
    /* CHECK - at the api level, paths longer then MAX_PATH require \\?\ prefix, unclear
     * if we would need to use that at this level instead of \??\ for long paths (not
     * that it matters since our buffer in this routine limits us to MAX_PATH anyways).
    /* FIXME - handle . and .. */
    /* FIMXE : there is also ntdll!RtlDosPathNameToNtPathName_U that does the
     * translation for us, used by CreateDirectory CreateFile etc. but looking
     * at the dissasembly it grabs the loader lock! why does it need
     * to do that? is it to translate . or ..?, better just to do the 
     * translation here where we know what's going on */
    if (name[0] == '\\') {
        name += 1; /* eat the first \ */
        if (name[0] == '\\') {
            if (name[1] == '?') {
                /* is \\?\UNC\server or \\?\c:\ type,
                 * chop off the \\?\ and we'll check for the UNC later */
                ASSERT_CURIOSITY(name[2] == '\\' && "create file invalid name");
                /* safety check, don't go beyond end of string */
                if (name[2] != '\0') {
                    name += 3;
                } else {
                    return false;
                } 
            } else {
                /* is \\server type */
                is_UNC = true;
            }
        } else {
            /* is \??\UNC\server for \??\c:\ type
             * chop off the \??\ and we'll check for the UNC later */
            ASSERT_CURIOSITY(name[0] == '?' && name[1] == '?' && name[2] == '\\' &&
                             "create file invalid name");
            /* safety check, don't go beyond end of string */
            if (name[0] != '\0' && name[1] != '\0' && name[2] != '\0') {
                name += 3;
            } else {
                return false;
            } 
        }
        if (!is_UNC) {
            /* we've eaten the initial \\?\ or \??\ check for UNC */
            if ((name[0] == 'U' || name[0] == 'u') &&
                (name[1] == 'N' || name[1] == 'n') &&
                (name[2] == 'C' || name[2] == 'c')) {
                /* is \??\UNC\server or \\?\UNC\server type, chop of the UNC 
                 * (we'll re-add below)
                 * NOTE '/' is not a legal separator for a \??\ or \\?\ path */
                ASSERT_CURIOSITY(name[3] == '\\' && "create file invalid name");
                is_UNC = true;
                name += 3;
            } else {
                /* is \??\c:\ or \\?\c:\ type,
                 * NOTE '/' is not a legal separator for a \??\ or \\?\ path */
                ASSERT_CURIOSITY(name[1] == ':' && name[2] == '\\' &&
                                 "create file invalid name");
            }
        }
    } else {
        /* is c:\ type, NOTE case 9329 c:/ is also legal */
        ASSERT_CURIOSITY(name[1] == ':' && (name[2] == '/' || name[2] == '\\') &&
                         "create file invalid name");
    }

    /* should now have either ("c:\" and !is_UNC) or ("\server" and is_UNC) */ 
    snwprintf(buf, buf_len, L"\\??\\%ls%hs",
              is_UNC ? L"UNC" : L"", name);
    buf[buf_len-1] = L'\0';
    /* change / to \ */
    for (i = 0; i < buf_len; i++) { 
        if (L'/' == buf[i]) 
            buf[i] = L'\\';
    }
    return true;
}

static file_t
os_internal_create_file(const char *fname, bool is_dir, ACCESS_MASK rights, uint sharing,
                        uint create_disposition)
{
    wchar_t buf[MAX_PATH];
    if (!convert_to_NT_file_path(buf, fname, BUFFER_SIZE_ELEMENTS(buf)))
        return INVALID_FILE;
    NULL_TERMINATE_BUFFER(buf); /* be paranoid */
    return create_file(buf, is_dir,
                       rights, sharing, create_disposition, true);
}

static bool
os_internal_create_file_test(const char *fname, bool is_dir, ACCESS_MASK rights,
                             uint sharing, uint create_disposition)
{
    HANDLE file = os_internal_create_file(fname, is_dir, rights, sharing,
                                          create_disposition);
    if (INVALID_FILE == file) {
        return false;
    }
    os_close(file);
    return true;
}

bool
os_file_exists(const char *fname, bool is_dir)
{
    return os_internal_create_file_test(fname, is_dir, 0, FILE_SHARE_READ, FILE_OPEN);
}

/* Returns true and sets 'size' of file on success; returns false on failure.
 * Note: This size is different from the allocation size of the file, which can
 * be larger or smaller (if file compression is turned on - case 8272).
 */
bool
os_get_file_size(const char *file, uint64 *size)
{
    wchar_t filename[MAXIMUM_PATH + 1];
    FILE_NETWORK_OPEN_INFORMATION file_info;

    ASSERT(file != NULL && size != NULL); 
    if (file == NULL || size == NULL)
        return false;

    /* See FIXME in os_internal_create_file() about prepending \??\ to the path
     * directly. 
     */
     /* FIXME: case 9182 this won't work for remote files */
     _snwprintf(filename, BUFFER_SIZE_ELEMENTS(filename), L"\\??\\%hs", file);
     NULL_TERMINATE_BUFFER(filename);
     if (query_full_attributes_file(filename, &file_info)) {
        ASSERT(sizeof(*size) == sizeof(file_info.EndOfFile.QuadPart));
        *size = file_info.EndOfFile.QuadPart;
        return true;
     }
     return false;
}

bool
os_get_file_size_by_handle(IN HANDLE file_handle,
                           uint64 *end_of_file /* OUT */)
{
    FILE_STANDARD_INFORMATION standard_info;
    NTSTATUS res = nt_query_file_info(file_handle,
                                      &standard_info,
                                      sizeof(standard_info),
                                      FileStandardInformation);
    /* should always be able to get this */
    ASSERT(NT_SUCCESS(res) && "bad file handle?");
    if (!NT_SUCCESS(res)) {
        return false;
    }
 
    *end_of_file = standard_info.EndOfFile.QuadPart;
    return true;
}

bool
os_set_file_size(IN HANDLE file_handle,
                 uint64 end_of_file)
{
    NTSTATUS res;
    FILE_END_OF_FILE_INFORMATION file_end_info;
    ASSERT_CURIOSITY(end_of_file != 0);
    file_end_info.EndOfFile.QuadPart = end_of_file;
    res = nt_set_file_info(file_handle,
                           &file_end_info,
                           sizeof(file_end_info),
                           FileEndOfFileInformation);
    ASSERT(NT_SUCCESS(res) && "can't set size: bad handle?");
    return NT_SUCCESS(res);
}

/* returns available and total quota for the current thread's user (if
 * impersonated), as well as total available on the volume 
 * Note that any valid handle on the volume can be used.
 */
bool
os_get_disk_free_space(IN HANDLE file_handle,
                       OUT uint64 *AvailableQuotaBytes OPTIONAL,
                       OUT uint64 *TotalQuotaBytes OPTIONAL,
                       OUT uint64 *TotalVolumeBytes OPTIONAL)
{
    /* FIXME: considering that we don't usually care about the actual
     * bytes available on the volume, we may use just
     * FILE_FS_SIZE_INFORMATION instead of FILE_FS_FULL_SIZE_INFORMATION
     * case 9000: need to check if both are available on NT
     */

    /* Windows Driver Kit: Installable File System Drivers ::
     * FILE_FS_FULL_SIZE_INFORMATION
     *
     * "The size of the buffer passed ... must be at least sizeof
     * (FILE_FS_FULL_SIZE_INFORMATION).  This structure must be
     * aligned on a LONGLONG (8-byte) boundary.  "
     *
     * Although on XP SP2 this call succeeds even on a non-aligned
     * value, to be sure we'll follow the recommendation.
     */

    FILE_FS_FULL_SIZE_INFORMATION unaligned_fs_full_size[2];
    FILE_FS_FULL_SIZE_INFORMATION *FileFsFullSize = 
        (FILE_FS_FULL_SIZE_INFORMATION *)
        ALIGN_FORWARD(unaligned_fs_full_size, sizeof(LONGLONG));
    uint64 BytesPerUnit;
    NTSTATUS res;

    ASSERT(sizeof(LONGLONG) < sizeof(FILE_FS_FULL_SIZE_INFORMATION));
    ASSERT(ALIGNED(FileFsFullSize, sizeof(LONGLONG)));
    res = nt_query_volume_info(file_handle,
                               FileFsFullSize,
                               sizeof(*FileFsFullSize),
                               FileFsFullSizeInformation);
    if (!NT_SUCCESS(res))
        return false;

    BytesPerUnit = FileFsFullSize->SectorsPerAllocationUnit *
        FileFsFullSize->BytesPerSector;
    if (AvailableQuotaBytes != NULL) {
        *AvailableQuotaBytes = FileFsFullSize->
            CallerAvailableAllocationUnits.QuadPart * BytesPerUnit;
    }
    if (TotalQuotaBytes != NULL) {
        *TotalQuotaBytes = FileFsFullSize->
            TotalAllocationUnits.QuadPart * BytesPerUnit;
    }
    if (TotalVolumeBytes != NULL) {
        *TotalVolumeBytes = FileFsFullSize->
            ActualAvailableAllocationUnits.QuadPart * BytesPerUnit;
    }

    return true;
}

/* NYI: os_copy_file - copies a portion of a file onto another.  Note
 * that if new_file is non-empty we are overwriting only the
 * appropriate subregion. os_copy_file() can be used as a full file
 * copy (with offset 0 in both files).  With an offset os_copy_file()
 * can be used to overwrite the portions of a file that are not mapped
 * in memory or are suffixes not at all covered by the PE format.
 */
/* NOTE: cf CopyFileEx which also claims to be doing something special
 * to preserve OLE structured storage?
 *
 * NOTE: we do don't support NTFS alternate data streams,
 * e.g. downloaded.dll:Zone.Identifier since we would expect that any
 * checks by say Software Restriction Policies are done on the
 * original file, not on what we really open.
 *
 * NOTE we don't preserve extended attributes, file attributes.  If we
 * care to have these should see see
 * kernel32!CreateFile(,hTemplateFile) which supplies file attributes
 * and extended attributes for the new file.
 *
 * Note we don't preserve security attributes - see
 * shell32!SHFileOperation if we need this.
 *
 * We don't deal in any way with encrypted files - they are opened
 * raw.  FIXME: may want to at least make sure that encrypted files
 * aren't shared.
 *
 * FIXME: testing: doublecheck compressed file offsets are properly
 * used - test both encrypted and compressed folders.
 */
bool
os_copy_file(HANDLE new_file, HANDLE original_file,
             uint64 new_file_offset, uint64 original_file_offset)
{
    /* we don't care to have the fastest filecopy implementation
     * current uses are rare enough.  See p.64 and 02 FileCopy from
     * Richter&Clark if a fast one is needed. */

    /* Note that NTFS will make the calls synchronously */
    /* FIXME: it may be useful to set the expected total file size
     * right away with os_set_file_size(), but that should be done
     * only in case the current size is smaller (e.g. we shouldn't
     * truncate if trying to overwrite a subsection ) */
    ASSERT_NOT_IMPLEMENTED(false);
    return false;
}

bool
os_create_dir(const char *fname, create_directory_flags_t create_dir_flags)
{
    bool require_new = TEST(CREATE_DIR_REQUIRE_NEW, create_dir_flags);
    bool force_owner = TEST(CREATE_DIR_FORCE_OWNER, create_dir_flags);

    /* case 9057 note that hard links are only between files but not directories */
    /* upcoming symlinks can be between either, for consistency should
     * always require_new,
     * FIXME: not all current users do this properly
     */
    return os_internal_create_file_test(fname, true, 0, FILE_SHARE_READ,
                                        (require_new ? FILE_CREATE : 
                                                      FILE_OPEN_IF) |
                                        (force_owner ? FILE_DISPOSITION_SET_OWNER : 0));
}

file_t
os_open_directory(const char *fname, int os_open_flags)
{
    uint sharing = FILE_SHARE_READ
        /* case 10255: allow persisted cache file renaming in
         * directory */
        | FILE_SHARE_WRITE;
    uint access = READ_CONTROL;

    /* FIXME: only 0 is allowed by create_file for now */
    if (TEST(OS_OPEN_READ, os_open_flags))
        access |= FILE_GENERIC_READ;

    return os_internal_create_file(fname, true, access, sharing,
                                   FILE_OPEN);
}

/* FIXME : investigate difference between GENERIC_* and FILE_GENERIC_*
 * both seem to work as expected (and CreateFile uses the GENERIC_* while
 * the ddk uses FILE_GENERIC_*) but they resolve differently, some confusion.
 * ntddk.h has GENERIC_* as a single bit flag while FILE_GENERIC_* is
 * a combination including FILE_{READ,WRITE}_DATA, so going with the latter.
 */
file_t
os_open(const char *fname, int os_open_flags)
{
    uint access = 0;
    /* FIXME case 8865: should default be no sharing? */
    uint sharing = FILE_SHARE_READ;

    if (TEST(OS_EXECUTE, os_open_flags))
        access |= FILE_GENERIC_EXECUTE;
    if (TEST(OS_OPEN_READ, os_open_flags))
        access |= FILE_GENERIC_READ;

    if (TEST(OS_SHARE_DELETE, os_open_flags))
        sharing |= FILE_SHARE_DELETE;

    if (!TEST(OS_OPEN_WRITE, os_open_flags))
        return os_internal_create_file(fname, false, access, sharing, FILE_OPEN);

    /* clients are allowed to open the file however they want, xref PR 227737 */
    ASSERT_CURIOSITY_ONCE((TEST(OS_OPEN_REQUIRE_NEW, os_open_flags)
                           IF_CLIENT_INTERFACE( ||
                             !IS_INTERNAL_STRING_OPTION_EMPTY(client_lib))) &&
                          "symlink risk PR 213492");
    
    return os_internal_create_file(fname, false,
                                   access |
                                   (TEST(OS_OPEN_APPEND, os_open_flags) ? 
                                    /* FILE_GENERIC_WRITE minus
                                     * FILE_WRITE_DATA, so we get auto-append
                                     */
                                    (STANDARD_RIGHTS_WRITE | FILE_APPEND_DATA |
                                     FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA) :
                                    FILE_GENERIC_WRITE),
                                   sharing,
                                   (TEST(OS_OPEN_REQUIRE_NEW, os_open_flags) ? 
                                       FILE_CREATE : 
                                       (TEST(OS_OPEN_APPEND, os_open_flags) ? 
                                           FILE_OPEN_IF : 
                                           FILE_OVERWRITE_IF)) |
                                   (TEST(OS_OPEN_FORCE_OWNER, os_open_flags) ? 
                                    FILE_DISPOSITION_SET_OWNER : 0));
}

void
os_close(file_t f)
{
    close_handle(f);
}

/* We take in size_t count to match linux, but Nt{Read,Write}File only
 * takes in a ULONG (==uint), though they return a ULONG_PTR (size_t)
 */
ssize_t
os_write(file_t f, const void *buf, size_t count)
{
    /* file_t is HANDLE opened with CreateFile */
    size_t written = 0;
    ssize_t out = -1;
    bool ok;
    if (f == INVALID_FILE)
        return out;
    IF_X64(ASSERT(CHECK_TRUNCATE_TYPE_uint(count)));
    ok = write_file(f, buf, (uint) count, NULL, &written);
    if (ok) {
        ASSERT(written <= INT_MAX && written <= count);
        out = (ssize_t)written;
    } else {
        ASSERT(written == 0);
    }
    return out;
}

/* We take in size_t count to match linux, but Nt{Read,Write}File only
 * takes in a ULONG (==uint), though they return a ULONG_PTR (size_t)
 */
ssize_t
os_read(file_t f, void *buf, size_t count)
{
    /* file_t is HANDLE opened with CreateFile */
    size_t read = 0;
    ssize_t out = -1;
    bool ok;
    if (f == INVALID_FILE)
        return out;
    IF_X64(ASSERT(CHECK_TRUNCATE_TYPE_uint(count)));
    ok = read_file(f, buf, (uint) count, NULL, &read);
    if (ok) {
        ASSERT(read <= INT_MAX && read <= count);
        out = (ssize_t)read;
    } else {
        ASSERT(read == 0);
    }
    return out;
}

void
os_flush(file_t f)
{
    bool ok = flush_file_buffers(f);
}

/* seek the current file position to offset bytes from origin, return true if succesful */
bool
os_seek(file_t f, int64 offset, int origin)
{
    FILE_POSITION_INFORMATION info;
    NTSTATUS res;
    int64 abs_offset = offset;

    switch (origin) {
    case OS_SEEK_SET:
        break;
    case OS_SEEK_CUR:
        {
            int64 cur_pos = os_tell(f);
            ASSERT(cur_pos != -1 && "bad file handle?"); /* shouldn't fail */
            abs_offset += cur_pos;
        }
        break;
    case OS_SEEK_END:
        {
            uint64 file_size = 0;
            bool res = os_get_file_size_by_handle(f, &file_size);
            ASSERT(res && "bad file handle?"); /* shouldn't fail */
            abs_offset += file_size;
        }
        break;
    default:
        ASSERT(false && "os_seek: invalid origin");
        return false;
    }

    info.CurrentByteOffset.QuadPart = abs_offset;
    res = nt_set_file_info(f, &info, sizeof(info), FilePositionInformation);

    /* can fail if invalid seek (past end of read only file for ex.) */
    return NT_SUCCESS(res);
}

/* return the current file position, -1 on failure */
int64
os_tell(file_t f)
{
    FILE_POSITION_INFORMATION info;
    NTSTATUS res = nt_query_file_info(f, &info, sizeof(info), FilePositionInformation);

    /* should always be able to get this */
    ASSERT(NT_SUCCESS(res) && "bad file handle?");
    if (!NT_SUCCESS(res)) {
        return -1;
    }
 
    return info.CurrentByteOffset.QuadPart;
}

/* Tries to delete a file that may be mapped in by this or another process.
 * We use FILE_DELETE_ON_CLOSE, which works only on SEC_COMMIT, not on SEC_IMAGE.
 * There is no known way to immediately delete a mapped-in SEC_IMAGE file.
 * Xref case 9964.
 */
bool
os_delete_mapped_file(const char *filename)
{
    NTSTATUS res;
    HANDLE hf;
    FILE_DISPOSITION_INFORMATION file_dispose_info;
    bool deleted = false;
    wchar_t wname[MAX_FILE_NAME_LENGTH];

    if (!convert_to_NT_file_path(wname, filename,
                                 BUFFER_SIZE_ELEMENTS(wname)))
        return false;
    NULL_TERMINATE_BUFFER(wname); /* be paranoid */

    res = nt_create_file(&hf, wname, NULL, 0, SYNCHRONIZE | DELETE,
                         FILE_ATTRIBUTE_NORMAL,
                         FILE_SHARE_DELETE | /* if already deleted */
                         FILE_SHARE_READ,
                         FILE_OPEN,
                         FILE_SYNCHRONOUS_IO_NONALERT 
                         | FILE_DELETE_ON_CLOSE
                         /* This should open a handle on a symlink rather
                          * than its target, and avoid other reparse code.
                          * Otherwise the FILE_DELETE_ON_CLOSE would cause
                          * us to delete the target of a symlink!
                          * FIXME: fully test this: case 10067
                          */
                         | FILE_OPEN_REPARSE_POINT);
    if (!NT_SUCCESS(res)) {
        LOG(GLOBAL, LOG_NT, 2,
            "os_delete_mapped_file: unable to open handle to %s: "PFX"\n",
            filename, res);
        return false;
    }

    /* Try to delete immediately.  If the file is mapped in, this will fail
     * with STATUS_CANNOT_DELETE 0xc0000121.
     */
    file_dispose_info.DeleteFile = TRUE;
    res = nt_set_file_info(hf, &file_dispose_info, sizeof(file_dispose_info),
                           FileDispositionInformation);
    if (NT_SUCCESS(res))
        deleted = true;
    else {
        LOG(GLOBAL, LOG_NT, 2,
            "os_delete_mapped_file: unable to mark for deletion %s: "PFX"\n",
            filename, res);
        /* continue on */
    }
    close_handle(hf);
    if (!deleted) {
        /* We can't accurately tell if FILE_DELETE_ON_CLOSE worked but we can try to
         * open and assume nobody created a new file of the same name.
         */
        res = nt_create_file(&hf, wname, NULL, 0, SYNCHRONIZE,
                             FILE_ATTRIBUTE_NORMAL,
                             FILE_SHARE_DELETE | FILE_SHARE_READ, FILE_OPEN,
                             FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT);
        LOG(GLOBAL, LOG_NT, 2,
            "os_delete_mapped_file: opening after close %s: "PFX"\n", filename, res);
        if (NT_SUCCESS(res))
            close_handle(hf);
        else if (res == STATUS_DELETE_PENDING ||
                 res == STATUS_OBJECT_NAME_NOT_FOUND) {
            deleted = true;
        } else
            ASSERT_CURIOSITY(false && "unable to confirm close-on-delete");
    }
    /* FIXME case 10048: if failure here, schedule for smss-on-boot deletion */
    return deleted;
}

bool
os_delete_file(const wchar_t *file_name,
               HANDLE directory_handle)
{
    NTSTATUS res;
    HANDLE hf;
    FILE_DISPOSITION_INFORMATION file_dispose_info;

    res = nt_create_module_file(&hf, file_name, 
                                directory_handle,
                                DELETE,
                                FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_DELETE | /* if already deleted */
                                FILE_SHARE_READ |
                                FILE_SHARE_WRITE,
                                FILE_OPEN,
                                0);
    /* note that FILE_DELETE_ON_CLOSE will act on the target of a
     * symbolic link (in Longhorn), while we want to act on the link
     * itself
     */

    /* this is expected to be called only when a file is in the way */
    ASSERT_CURIOSITY(NT_SUCCESS(res) && "can't open for deletion");
    if (!NT_SUCCESS(res))
        return false;

    file_dispose_info.DeleteFile = TRUE;
    res = nt_set_file_info(hf,
                           &file_dispose_info,
                           sizeof(file_dispose_info),
                           FileDispositionInformation);
    /* close regardless of success */
    close_handle(hf);
    ASSERT_CURIOSITY(NT_SUCCESS(res) && "couldn't mark for deletion");
    /* file may have sections mapped (the usual case for DLLs in ASLR cache) */
    /* we don't expect to be deleting files that are in use by others */

    /* if we had the only handle, the file should be deleted by now */
    return NT_SUCCESS(res);
}

/* We take in orig_name instead of a file handle so that we can abstract
 * away the privileges required to rename a file when opening the handle.
 * We also do not take in a rootdir handle to be parallel to the linux
 * system call, so caller must specify full path.
 * This will not rename a file across volumes.
 *
 * see os_rename_file_in_directory() for a Win32-specific interface
 */
bool
os_rename_file(const char *orig_name, const char *new_name, bool replace)
{
    file_t fd = INVALID_FILE;
    NTSTATUS res;
    FILE_RENAME_INFORMATION info;
    wchar_t worig[MAX_FILE_NAME_LENGTH];

    if (!convert_to_NT_file_path(info.FileName, new_name,
                                 BUFFER_SIZE_ELEMENTS(info.FileName)))
        return false;
    NULL_TERMINATE_BUFFER(info.FileName); /* be paranoid */

    /* We could use os_open if we added OS_DELETE => DELETE+FILE_OPEN,
     * but then we couldn't rename directories; ditto for create_file,
     * so we directly call nt_create_file.
     */
    if (!convert_to_NT_file_path(worig, orig_name,
                                 BUFFER_SIZE_ELEMENTS(worig)))
        return false;
    NULL_TERMINATE_BUFFER(worig); /* be paranoid */
    res = nt_create_file(&fd, worig, NULL, 0, DELETE | SYNCHRONIZE,
                         FILE_ATTRIBUTE_NORMAL,
                         /* need F_S_READ if currently open w/ F_S_READ */
                         FILE_SHARE_READ | FILE_SHARE_DELETE,
                         FILE_OPEN, /* FILE_SUPERSEDE fails */
                         /* no FILE_{NON_,}DIRECTORY_FILE */
                         FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(res) || fd == INVALID_FILE) {
        LOG(GLOBAL, LOG_NT, 2,
            "os_rename_file: unable to open handle to %s: "PFX"\n", orig_name, res);
        return false;
    }

    /* I tried three rename options with NtSetFileInformation:
     * 1) set FileRenameInformation: works on FAT, NTFS, all platforms
     * 2) set FileNameInformation: not allowed; only for get
     * 3) set FileShortNameInformation: I couldn't get this to work, but
     *    was probably missing some privilege; but, only available on NTFS XP+
     */
    info.ReplaceIfExists = (BOOLEAN) replace;
    info.RootDirectory = NULL;
    IF_X64(ASSERT_TRUNCATE(info.FileNameLength, uint,
                           wcslen(info.FileName) * sizeof(wchar_t)));
    info.FileNameLength = (uint) (wcslen(info.FileName) * sizeof(wchar_t));
    res = nt_set_file_info(fd, &info, sizeof(info), FileRenameInformation);
    /* Renaming will fail if a file handle (other than this one) is open */
    if (!NT_SUCCESS(res)) {
        LOG(GLOBAL, LOG_NT, 2,
            "os_rename_file: NtSetFileInformation error "PFX"\n", res);
    }
    close_handle(fd);
    return NT_SUCCESS(res);
}

/* similar to os_rename_file(), but more geared to Windows users
 * We take in orig_name instead of a file handle, so that we can abstract
 * away the privileges required to rename a file when opening the handle.
 * Note however, that any other handle must be closed before calling.
 * Both names are relative to rootdir handle, since renaming files in
 * same directory is our primary use.
 */
bool
os_rename_file_in_directory(IN HANDLE rootdir,
                            const wchar_t *orig_name,
                            const wchar_t *new_name,
                            bool replace)
{
    file_t fd = INVALID_FILE;
    NTSTATUS res;
    FILE_RENAME_INFORMATION info;

    res = nt_create_file(&fd, orig_name, rootdir, 
                         0, DELETE | SYNCHRONIZE,
                         FILE_ATTRIBUTE_NORMAL,
                         /* need F_S_READ if currently open w/ F_S_READ */
                         FILE_SHARE_READ | FILE_SHARE_DELETE,
                         FILE_OPEN, /* FILE_SUPERSEDE fails */
                         /* no FILE_{NON_,}DIRECTORY_FILE */
                         FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(res) || fd == INVALID_FILE) {
        LOG(GLOBAL, LOG_NT, 2,
            "os_rename_file: unable to open handle to %s: "PFX"\n", orig_name, res);
        return false;
    }

    info.ReplaceIfExists = (BOOLEAN) replace;
    info.RootDirectory = rootdir;
    wcsncpy(info.FileName, new_name,
            BUFFER_SIZE_ELEMENTS(info.FileName));
    NULL_TERMINATE_BUFFER(info.FileName); /* be paranoid */
    IF_X64(ASSERT_TRUNCATE(info.FileNameLength, uint,
                           wcslen(info.FileName) * sizeof(wchar_t)));
    info.FileNameLength = (uint) (wcslen(info.FileName) * sizeof(wchar_t));
    res = nt_set_file_info(fd, &info, sizeof(info), FileRenameInformation);
    /* Renaming will fail if a file handle (other than this one) is open */
    if (!NT_SUCCESS(res)) {
        LOG(GLOBAL, LOG_NT, 2,
            "os_rename_file_in_directory: NtSetFileInformation error "PFX"\n", res);
    }
    close_handle(fd);
    return NT_SUCCESS(res);
}

byte *
os_map_file(file_t f, size_t size, uint64 offs, app_pc addr, uint prot,
            bool copy_on_write)
{
    NTSTATUS res;
    HANDLE section;
    byte *map = addr;
    size_t view_size = size;
    uint osprot = memprot_to_osprot(prot);
    LARGE_INTEGER li_offs;
    li_offs.QuadPart = offs;

    if (copy_on_write) {
        /* Ask for COW for both the section and the view, though we should only
         * need it for the view (except on win98, according to Richter p604)
         */
        osprot = osprot_add_writecopy(osprot);
    }
    res = nt_create_section(&section,
                            SECTION_ALL_ACCESS, /* FIXME: maybe less privileges needed */
                            NULL, /* full file size, even if partial view map */
                            osprot,
                            /* can only be SEC_IMAGE if a PE file */
                            /* FIXME: SEC_RESERVE shouldn't work w/ COW yet
                             * it did in my test */
                            SEC_COMMIT,
                            f,
                            /* process private - no security needed */
                            /* object name attributes */
                            NULL /* unnamed */, 0, NULL, NULL);
    if (!NT_SUCCESS(res)) {
        LOG(GLOBAL, LOG_NT, 2, "os_map_file: NtCreateSection error "PFX"\n", res);
        return NULL;
    }
    /* FIXME case 9642: support requesting a particular base address so
     * we can randomize, make adjacent to vmheap, etc.
     */
    res = nt_map_view_of_section(section, /* 0 */
                                 NT_CURRENT_PROCESS, /* 1 */
                                 &map, /* 2 */
                                 0, /* 3 */
                                 0 /* not page-file-backed */, /* 4 */
                                 &li_offs, /* 5 */
                                 (PSIZE_T) &view_size, /* 6 */
                                 ViewUnmap /* FIXME: expose? */, /* 7 */
                                 0 /* no special top-down or anything */, /* 8 */
                                 osprot); /* 9 */
    /* We do not need to keep the section handle open */
    close_handle(section);
    if (!NT_SUCCESS(res)) {
        LOG(GLOBAL, LOG_NT, 2, "os_map_file: NtMapViewOfSection error "PFX"\n", res);
        return NULL;
    }
    return map;
}

bool
os_unmap_file(byte *map, size_t size/*unused*/)
{
    int res = nt_unmap_view_of_section(NT_CURRENT_PROCESS, map);
    return NT_SUCCESS(res);
}

/* FIXME : should check context flags, what if only integer or only control! */
/* Translates the context cxt for the given thread trec
 * Like any instance where a thread_record_t is used by a thread other than its
 * owner, the caller must hold the thread_initexit_lock to ensure that it
 * remains valid.
 * Requires thread trec is at_safe_spot().
 */
bool
translate_context(thread_record_t *trec, CONTEXT *cxt, bool restore_memory)
{
    dr_mcontext_t mc;
    bool res;
    /* ensure we have eip and esp */
    ASSERT(TESTALL(CONTEXT_CONTROL/*2 bits so ALL*/, cxt->ContextFlags));
    /* really we should have the full state */
    ASSERT(TESTALL(CONTEXT_DR_STATE, cxt->ContextFlags));
    context_to_mcontext(&mc, cxt);
    res = translate_mcontext(trec, &mc, restore_memory);
    if (res)
        mcontext_to_context(cxt, &mc);
    return res;
}

/* be careful about args: for windows different versions have different offsets
 * see SYSCALL_PARAM_OFFSET in win32/os.c
 */
static void
set_mcontext_for_syscall(dcontext_t *dcontext, int sys_enum,
#ifdef X64
                         reg_t arg1, reg_t arg2, reg_t arg3
#else
                         reg_t sys_arg
#endif
                         )
{
    dr_mcontext_t *mc = get_mcontext(dcontext);
#ifdef X64
    LOG(THREAD, LOG_SYSCALLS, 2,
        "issue_last_system_call_from_app(0x%x, "PFX" "PFX" "PFX")\n",
        syscalls[sys_enum], arg1, arg2, arg3);
#else
    LOG(THREAD, LOG_SYSCALLS, 2,
        "issue_last_system_call_from_app(0x%x, "PFX")\n", syscalls[sys_enum], sys_arg);
#endif

    mc->xax = syscalls[sys_enum];
    if (get_syscall_method() == SYSCALL_METHOD_WOW64) {
        mc->xcx = wow64_index[sys_enum];
    }
#ifdef X64
    mc->xcx = arg1;
    mc->xdx = arg2;
    mc->r8 = arg3;
#else
    mc->xdx = sys_arg;
#endif
}

/* raise an exception in the application context */
/* FIXME : see os_forge_exception's call of this function for issues */
void
os_raise_exception(dcontext_t *dcontext, 
                   EXCEPTION_RECORD* pexcrec, CONTEXT* pcontext)
{
#ifdef X64
    set_mcontext_for_syscall(dcontext, SYS_RaiseException,
                             (reg_t)pexcrec, (reg_t)pcontext, (reg_t)true);
#else
    /* ZwRaiseException arguments */
    struct _raise_exception_arguments_t {
        PEXCEPTION_RECORD ExceptionRecord;
        PCONTEXT Context;
        DWORD SearchFrames;
    } raise_exception_arguments = {pexcrec, pcontext, true};
    /* NOTE this struct stays on dstack when the syscall is executed! */

    /* args are on our stack so offset bytes are valid, we won't return
     * here so is ok if os clobbers them, though it won't since natively
     * they hold return addresses */
    reg_t arg_pointer = (reg_t)
        ((ptr_uint_t)&raise_exception_arguments) - SYSCALL_PARAM_OFFSET();

    set_mcontext_for_syscall(dcontext, SYS_RaiseException, arg_pointer),
#endif
    issue_last_system_call_from_app(dcontext);
    ASSERT_NOT_REACHED();
}

/***************************************************************************
 * CORE DUMPS
 */
/* all static vars here are not persistent across cache execution, so unprot */
START_DATA_SECTION(NEVER_PROTECTED_SECTION, "w");

static char dump_core_buf[256] VAR_IN_SECTION(NEVER_PROTECTED_SECTION)
     = {0,}; /* protected by dumpcore_lock */
static char dump_core_file_name[MAXIMUM_PATH] VAR_IN_SECTION(NEVER_PROTECTED_SECTION)
     = {0,}; /* protected by dumpcore_lock */

static void
os_dump_core_segment_info(file_t file, HANDLE h, ULONG selector, const char *name)
{
    NTSTATUS res;
    DESCRIPTOR_TABLE_ENTRY entry = {0,};

    entry.Selector = selector;
    res = query_seg_descriptor(h, &entry);
    /* This feature from PR 212905 does not work on x64 b/c there is no
     * support for the underlying system call: we get STATUS_NOT_IMPLEMENTED.
     */
    if (NT_SUCCESS(res)) {
        snprintf(dump_core_buf, BUFFER_SIZE_ELEMENTS(dump_core_buf),
                 "%s=0x%04x (0x%08x 0x%08x)\n", name, entry.Selector,
                 /* print the raw bits in the descriptor */
                 *((PULONG)&entry.Descriptor), *(((PULONG)&entry.Descriptor)+1));
    } else {
        snprintf(dump_core_buf, BUFFER_SIZE_ELEMENTS(dump_core_buf),
                 "%s=0x%04x\n", name, entry.Selector);
    }        

    NULL_TERMINATE_BUFFER(dump_core_buf);
    os_write(file, dump_core_buf, strlen(dump_core_buf));
}

static void
os_dump_core_dump_thread(file_t file, thread_id_t tid, TEB *teb, HANDLE h,
                         int handle_rights, CONTEXT *cxt,
                         dcontext_t *dcontext)
{
    app_pc win32_start_addr = 0;

    /* for x64, FIXME PR 249988: need to coordinate w/ ldmp.c */

    snprintf(dump_core_buf, BUFFER_SIZE_ELEMENTS(dump_core_buf),
             "Thread="PFX"\nTEB="PFX"\n"
             "HandleRights=0x%08x\n"
             "Eax="PFX", Ebx="PFX", Ecx="PFX", Edx="PFX"\n"
             "Esi="PFX", Edi="PFX", Esp="PFX", Ebp="PFX"\n"
             "EFlags="PFX", Eip="PFX"\n",
             tid, teb, handle_rights,
             cxt->CXT_XAX, cxt->CXT_XBX, cxt->CXT_XCX, cxt->CXT_XDX, cxt->CXT_XSI, 
             cxt->CXT_XDI, cxt->CXT_XSP, cxt->CXT_XBP, cxt->CXT_XFLAGS, cxt->CXT_XIP);
    NULL_TERMINATE_BUFFER(dump_core_buf);
    os_write(file, dump_core_buf, strlen(dump_core_buf));

    /* print segment selectors and associated descriptors */
    os_dump_core_segment_info(file, h, cxt->SegCs, "Cs");
    os_dump_core_segment_info(file, h, cxt->SegSs, "Ss");
    os_dump_core_segment_info(file, h, cxt->SegDs, "Ds");
    os_dump_core_segment_info(file, h, cxt->SegEs, "Es");
    os_dump_core_segment_info(file, h, cxt->SegFs, "Fs");
    os_dump_core_segment_info(file, h, cxt->SegGs, "Gs");

    /* Print the win32 start address.  This is saved away in the
     * dcontext when the thread is created.
     */
    if (dcontext != NULL) {
        win32_start_addr = dcontext->win32_start_addr;
    }
    /* if the dcontext is unavailable, use the syscall */
    else {
        NTSTATUS res = query_win32_start_addr(h, &win32_start_addr);
        ASSERT(NT_SUCCESS(res) && "failed to obtain win32 start address");
    }
    snprintf(dump_core_buf, BUFFER_SIZE_ELEMENTS(dump_core_buf),
             "Win32StartAddr="PFX"\n", win32_start_addr);
    NULL_TERMINATE_BUFFER(dump_core_buf);
    os_write(file, dump_core_buf, strlen(dump_core_buf));
}

#pragma warning( push )
/* warning is from GET_OWN_CONTEXT: flow in/out of asm code suppresses global opt */
#pragma warning( disable : 4740)
static void
os_dump_core_live_dump(const char *msg)
{
    /* like the dump_core_buf, all the locals are protected by the
     * dumpcore_lock and are static to save stack space (CONTEXT is quite
     * sizable) */
    static file_t dmp_file VAR_IN_SECTION(NEVER_PROTECTED_SECTION) = INVALID_FILE;
    static thread_record_t *tr VAR_IN_SECTION(NEVER_PROTECTED_SECTION) = NULL;
    static thread_record_t *my_tr VAR_IN_SECTION(NEVER_PROTECTED_SECTION) = NULL;
    static int i VAR_IN_SECTION(NEVER_PROTECTED_SECTION) = 0;
    static thread_id_t my_id VAR_IN_SECTION(NEVER_PROTECTED_SECTION) = 0;
    static bool have_all_threads_lock VAR_IN_SECTION(NEVER_PROTECTED_SECTION) = false;
    static PBYTE pb VAR_IN_SECTION(NEVER_PROTECTED_SECTION) = NULL;
    static MEMORY_BASIC_INFORMATION mbi VAR_IN_SECTION(NEVER_PROTECTED_SECTION) = {0,};
    static CONTEXT cxt VAR_IN_SECTION(NEVER_PROTECTED_SECTION) = {0,};
    DEBUG_DECLARE(static bool suspend_failures VAR_IN_SECTION(NEVER_PROTECTED_SECTION)
                  = false;)

    /* initialize */
    pb = NULL;
    have_all_threads_lock = false;
    my_id = get_thread_id();
    my_tr = NULL;
    /* We should eventually add xmm regs to ldmp and use CONTEXT_DR_STATE here
     * (xref PR 264138) */
    cxt.ContextFlags = CONTEXT_CONTROL|CONTEXT_INTEGER|CONTEXT_SEGMENTS;
    
    /* get logfile */
    /* use no option synch for syslogs to avoid grabbing locks and risking
     * deadlock, caller should have synchronized already anyways */
    if (!get_unique_logfile(".ldmp", dump_core_file_name, 
                            sizeof(dump_core_file_name), false, &dmp_file) || 
        dmp_file == INVALID_FILE) {
        SYSLOG_INTERNAL_NO_OPTION_SYNCH(SYSLOG_WARNING, 
                                        "Unable to open core dump file");
        return;
    }

    /* Write message */
    if (msg != NULL) {
        size_t length = strlen(msg);
        /* we start with length of message to make parsing easier */
        snprintf(dump_core_buf, BUFFER_SIZE_ELEMENTS(dump_core_buf),
                 PFX"\n", length+1 /* +1 for the \n */);
        NULL_TERMINATE_BUFFER(dump_core_buf);
        os_write(dmp_file, dump_core_buf, strlen(dump_core_buf));
        os_write(dmp_file, msg, length);
        os_write(dmp_file, "\n", 1);
    }
    
    /* synch with all threads */
    /* Don't use get_list_of_threads, it grabs a lock and allocates memory
     * both of which might be dangerous on this path, instead walk table
     * by hand (we try to grab the neccesary locks, but we will go ahead 
     * and walk the table if we can't FIXME)
     * FIXME : share with dynamo.c */
    /* Try to grab locks,
     * NOTE os_dump_core, already turned off deadlock_avoidance for us */
#ifdef DEADLOCK_AVOIDANCE
    /* ref case 4174, deadlock avoidance will assert if we try to grab a lock
     * we already own, even if its only a trylock and even if the option is
     * turned off! We hack around it here */
    if (all_threads_lock.owner == get_thread_id()) {
        LOG(GLOBAL, LOG_ALL, 1, 
            "WARNING : live dump, faulting thread already owns the all_threads lock, let's hope things are consistent\n");
    } else {
#endif
        for(i=0; i < 100 /* arbitrary num */; i++) {
            if (mutex_trylock(&all_threads_lock)) {
                have_all_threads_lock = true;
                break;
            } else {
                thread_yield();
            }
        }
        DODEBUG({
            if (!have_all_threads_lock) {
                LOG(GLOBAL, LOG_ALL, 1, 
                    "WARNING : live dump unable to grab all_threads lock, "
                    "continuing without it\n");
            }
        });
#ifdef DEADLOCK_AVOIDANCE
    }
#endif
    
    /* print out peb address */
    snprintf(dump_core_buf, BUFFER_SIZE_ELEMENTS(dump_core_buf),
             "PEB="PFX"\n", get_own_peb());
    NULL_TERMINATE_BUFFER(dump_core_buf);
    os_write(dmp_file, dump_core_buf, strlen(dump_core_buf));
    
    /* print out DR address */
    snprintf(dump_core_buf, BUFFER_SIZE_ELEMENTS(dump_core_buf),
             "dynamorio.dll="PFX"\n", get_dynamorio_dll_start());
    NULL_TERMINATE_BUFFER(dump_core_buf);
    os_write(dmp_file, dump_core_buf, strlen(dump_core_buf));

    /* for all threads, suspend and dump context */
    /* FIXME : do we care about segment, sse, float, or debug registers? */
    /* Do current thread first, first get thread record */
    if (all_threads != NULL) {
        for (i = 0; i < HASHTABLE_SIZE(ALL_THREADS_HASH_BITS); i++) {
            for (tr = all_threads[i]; tr != NULL; tr = tr->next) {
                if (tr->id == my_id)
                    my_tr = tr;
            }
        }
    }
    GET_OWN_CONTEXT(&cxt);
    os_dump_core_dump_thread(dmp_file, my_id, get_own_teb(), NT_CURRENT_THREAD,
                             my_tr != NULL ? 
                              nt_get_handle_access_rights(my_tr->handle) : 0, 
                             &cxt,
                             my_tr != NULL ? my_tr->dcontext : NULL);

    /* now walk all threads, skipping current thread */
    if (all_threads != NULL) {
        for (i = 0; i < HASHTABLE_SIZE(ALL_THREADS_HASH_BITS); i++) {
            for (tr = all_threads[i]; tr != NULL; tr = tr->next) {
                if (tr->id != my_id) {
                    ACCESS_MASK handle_rights = 
                        nt_get_handle_access_rights(tr->handle);
                    TEB *teb_addr = get_teb(tr->handle);
                    DEBUG_DECLARE(bool res = )
                        thread_suspend(tr);
                    /* we can't assert here (could infinite loop) */
                    DODEBUG({ suspend_failures = suspend_failures || !res; });
                    if (thread_get_context(tr, &cxt)) {
                        os_dump_core_dump_thread(dmp_file, tr->id, teb_addr, 
                                                 tr->handle, handle_rights, &cxt,
                                                 tr->dcontext);
                    } else {
                        snprintf(dump_core_buf, 
                                 BUFFER_SIZE_ELEMENTS(dump_core_buf),
                                 "Thread=0x%08x\nTEB="PFX"\n"
                                 "HandleRights=0x%08x\n"
                                 "<error state not available>\n\n",
                                 tr->id, teb_addr, handle_rights);
                        NULL_TERMINATE_BUFFER(dump_core_buf);
                        os_write(dmp_file, dump_core_buf, strlen(dump_core_buf));
                    }
                }
            }
        }
    } else {
        char *msg = "<error all threads list is already freed>";
        os_write(dmp_file, msg, strlen(msg));
        /* FIXME : if other threads are active (say in the case of detaching)
         * walking the memory below could be racy, what if another thread
         * frees some chunk of memory while we are copying it! Just live with
         * the race for now. */
    }

    /* dump memory */
    /* FIXME : print_ldr_data() ? */
    while (query_virtual_memory(pb, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        snprintf(dump_core_buf, BUFFER_SIZE_ELEMENTS(dump_core_buf), "\n"
                 "BaseAddress="PFX"\n"
                 "AllocationBase="PFX"\n"
                 "AllocationProtect=0x%08x %s\n"
                 "RegionSize=0x%08x\n"
                 "State=0x%08x %s\n"
                 "Protect=0x%08x %s\n"
                 "Type=0x%08x %s\n",
                 mbi.BaseAddress,
                 mbi.AllocationBase,
                 mbi.AllocationProtect, prot_string(mbi.AllocationProtect),
                 mbi.RegionSize,
                 mbi.State, mem_state_string(mbi.State),
                 mbi.Protect, prot_string(mbi.Protect),
                 mbi.Type, mem_type_string(mbi.Type));
        NULL_TERMINATE_BUFFER(dump_core_buf);
        os_write(dmp_file, dump_core_buf, strlen(dump_core_buf));
        
        if (mbi.State == MEM_COMMIT && !TEST(PAGE_GUARD, mbi.Protect) &&
            prot_is_readable(mbi.Protect)) {
            os_write(dmp_file, mbi.BaseAddress, mbi.RegionSize);
        }
            
        if (pb + mbi.RegionSize < pb)
            break;
        pb += mbi.RegionSize;
    }

    /* dump handles */
    {
        /* see Nebbett examples 1.2 and 2.1, may not be able to do this 
         * in the general case one methodolgy requires the debug privilege 
         * the other requires that a global flag is set at boot time 
         * FIXME */
    }
    
    /* end dump, forensics file will have call stacks and module list */
    /* unsynch with threads */
    if (all_threads != NULL) {
        for (i = 0; i < HASHTABLE_SIZE(ALL_THREADS_HASH_BITS); i++) {
            for (tr = all_threads[i]; tr != NULL; tr = tr->next) {
                if (tr->id != my_id) {
                    /* we assume that if a suspend failed, the corresponding
                     * resume will also fail -- o/w we could end up resuming
                     * a thread that a caller suspended!
                     */
                    DEBUG_DECLARE(bool res = )
                        thread_resume(tr);
                    /* we can't assert here (could infinite loop) */
                    DODEBUG({ suspend_failures = suspend_failures || !res; });
                }
            }
        }
    }
    
    /* cleanup */
    if (have_all_threads_lock)
        mutex_unlock(&all_threads_lock);
    close_file(dmp_file);

    /* write an event indicating the file was created */
    SYSLOG_NO_OPTION_SYNCH(SYSLOG_INFORMATION, LDMP, 
                           3, get_application_name(), get_application_pid(), 
                           dump_core_file_name);

    DODEBUG({
        if (suspend_failures) {
            SYSLOG_INTERNAL_NO_OPTION_SYNCH(SYSLOG_ERROR,
                "suspend/resume failures during ldmp creation");
        }
    });
}
#pragma warning( pop )

#ifdef INTERNAL
static void
os_dump_core_external_dump()
{
    /* static buffers save stack space, this is do-once anyway, protected by
     * dumpcore_lock from os_dump_core() */
    static char oncrash_var[MAXIMUM_PATH] VAR_IN_SECTION(NEVER_PROTECTED_SECTION) = {0,};
    static wchar_t oncrash_cmdline[MAXIMUM_PATH] VAR_IN_SECTION(NEVER_PROTECTED_SECTION)
        = {0,};
    static wchar_t oncrash_exe[MAXIMUM_PATH] VAR_IN_SECTION(NEVER_PROTECTED_SECTION)
        = {0,};
    
    /* the ONCRASH key tells us exactly what to launch, with our pid appended */
    int retval = get_parameter(L_IF_WIN(DYNAMORIO_VAR_ONCRASH), oncrash_var,
                               sizeof(oncrash_var));
    if (IS_GET_PARAMETER_SUCCESS(retval)) {
        HANDLE child;
        /* ASSUMPTION: no spaces in exe name, should be ok since only developers will
         * specify a name for this key, everyone else will use tools
         */
        char *c = strchr(oncrash_var, ' ');
        if (c == NULL)
            c = oncrash_var + strlen(oncrash_var);
        ASSERT(c - oncrash_var < sizeof(oncrash_exe)/sizeof(wchar_t));
        snwprintf(oncrash_exe, c - oncrash_var, L"%hs", oncrash_var);
        oncrash_exe[c - oncrash_var] = L'\0';
        
        snwprintf(oncrash_cmdline, sizeof(oncrash_cmdline)/sizeof(wchar_t),
                  L"%hs %hs", oncrash_var, get_application_pid());
        NULL_TERMINATE_BUFFER(oncrash_cmdline);
        
        SYSLOG_INTERNAL_INFO("Thread %d dumping core via \"%ls\"", get_thread_id(),
                             oncrash_cmdline);
        
        child = create_process(oncrash_exe, oncrash_cmdline);
        
        if (child != INVALID_HANDLE_VALUE) {
            /* wait for child to exit
             * FIXME: this makes ntsd have to do a 30-second wait to break in!
             * plus it causes drwtsn32 to hang, then timeout and kill us
             * w/o producing a dump file -- and only the header on the log file
             * BUT, if we don't do this, we only get dumps for -kill_thread!
             */
            nt_wait_event_with_timeout(child, INFINITE_WAIT);
            close_handle(child);
        } else
            SYSLOG_INTERNAL_WARNING("Unable to dump core via \"%ls\"", oncrash_cmdline);
    } else {
        SYSLOG_INTERNAL_WARNING("Unable to dump core due to missing parameter");
    }
}
#endif /* INTERNAL */

void
os_dump_core(const char *msg)
{
    static thread_id_t current_dumping_thread_id VAR_IN_SECTION(NEVER_PROTECTED_SECTION)
        = 0;
    thread_id_t current_id = get_thread_id();
#ifdef DEADLOCK_AVOIDANCE
    dcontext_t *dcontext = get_thread_private_dcontext();
    thread_locks_t * old_thread_owned_locks = NULL;
#endif

    if (current_id == current_dumping_thread_id)
        return; /* avoid infinite loop */

    /* FIXME : A failure in the mutex_lock or mutex_unlock of the 
     * dump_core_lock could lead to an infinite recursion, also a failure while
     * holding the eventlog_lock would lead to a deadlock at the syslog in
     * livedump (but we would likely deadlock later anyways), all other 
     * recursion/deadlock cases should be handled by the above check */

#ifdef DEADLOCK_AVOIDANCE
    /* first turn off deadlock avoidance for this thread (needed for live dump
     * to try to grab all_threads and thread_initexit locks) */ 
    if (dcontext != NULL) {
        old_thread_owned_locks = dcontext->thread_owned_locks;
        dcontext->thread_owned_locks = NULL;
    }
#endif

    /* only allow one thread to dumpcore at a time, also protects static 
     * buffers and current_dumping_thread_id */
    mutex_lock(&dump_core_lock);
    current_dumping_thread_id = current_id;

    if (DYNAMO_OPTION(live_dump)) {
        os_dump_core_live_dump(msg);
    }
    
#ifdef INTERNAL
    /* not else-if, allow to be composable */
    if (DYNAMO_OPTION(external_dump)) {
        os_dump_core_external_dump();
    }
#endif
    
    current_dumping_thread_id = 0;
    mutex_unlock(&dump_core_lock);

#ifdef DEADLOCK_AVOIDANCE
    /* restore deadlock avoidance for this thread */
    if (dcontext != NULL) {
        dcontext->thread_owned_locks = old_thread_owned_locks;
    }
#endif
}

/* back to normal section */
END_DATA_SECTION()
/***************************************************************************/

/***************************************************************************/
/* detaching routines */

/* not static only for a few asserts in other files */
bool doing_detach = false;

static bool internal_detach = false;

/* Handle any outstanding callbacks.
 *
 * For sysenter system calls the kernel callback return returns to a known fixed
 * location that does a ret.  To regain control we have overwritten the return
 * address on the stack to point back to the after syscall location and need to restore
 * the original target here.
 *
 * For all other types of system calls the kernel will return the instruction after
 * the system call which is in our generated code.  We allocate a piece of thread
 * shared code here followed by an array of thread private detach_callback_stack_ts and
 * an array of the callback return addresses.  We redirect all after syscall locations
 * to that shared code which then dispatches on thread_id to find the proper
 * detach_callback_stack_t, get the right return address from it and then jmp to it.
 *
 * Returns true if there are outstanding non-sysenter callbacks.
 */
static bool
detach_helper_handle_callbacks(int num_threads, thread_record_t **threads,
                               bool *cleanup_tpc /* array of size num_threads */)
{
    int i, num_threads_with_callbacks = 0, num_stacked_callbacks = 0;

    /* First walk counts the number of threads with outstanding callbacks and the number
     * of stacked callbacks (and also fixes the stack for sysenter system calls) so we
     * now how much memory to allocate for non-sysenter system calls. */
    for (i = 0; i < num_threads; i++) {
        dcontext_t *dcontext = threads[i]->dcontext;
        cleanup_tpc[i] = true; /* default to clean up */
        if (dcontext->prev_unused != NULL && dcontext->prev_unused->valid) {
            dcontext_t *tmp_dc = dcontext->prev_unused;
            int count = 0;
            LOG(GLOBAL, LOG_ALL, 1, 
                "Detach : thread %d has stacked callbacks\n", threads[i]->id);
            do {
                count++;
                LOG(GLOBAL, LOG_ALL, 1, "callback %d has ret pc "PFX"\n", 
                    count, POST_SYSCALL_PC(tmp_dc));
                ASSERT(POST_SYSCALL_PC(tmp_dc) != NULL &&
                       !is_dynamo_address(POST_SYSCALL_PC(tmp_dc)));
                if (get_syscall_method() == SYSCALL_METHOD_SYSENTER && 
                    INTERNAL_OPTION(detach_fix_sysenter_on_stack)) {
                    /* Fix up our stack modifications. Since the kernel returns to a
                     * fixed location this is all we need to do to restore app state.
                     * Note that shared syscall saves xsp for us, so xsp should be
                     * correct. */
                    ASSERT(*((app_pc *)get_mcontext(tmp_dc)->xsp) == 
                           after_do_syscall_code(dcontext) ||
                           *((app_pc *)get_mcontext(tmp_dc)->xsp) ==
                           after_shared_syscall_code(dcontext));
                    /* fix return address */
                    LOG(GLOBAL, LOG_ALL, 1,
                        "callback %d patching stack address "PFX" from "PFX" to "PFX"\n",
                        get_mcontext(tmp_dc)->xsp, *((app_pc *)get_mcontext(tmp_dc)->xsp),
                        POST_SYSCALL_PC(tmp_dc));
                    *((app_pc *)get_mcontext(tmp_dc)->xsp) = POST_SYSCALL_PC(tmp_dc);
                    if (DYNAMO_OPTION(sygate_sysenter)) {
                        *((app_pc *)(get_mcontext(tmp_dc)->xsp+XSP_SZ)) =
                            dcontext->sysenter_storage;
                    }
                }
                tmp_dc = tmp_dc->prev_unused;
            } while (tmp_dc != NULL && tmp_dc->valid);
            num_threads_with_callbacks++;
            num_stacked_callbacks += count;
            /* can't free thread private syscall code if not SYSENTER since kernel
             * will return to there */
            cleanup_tpc[i] = (get_syscall_method() == SYSCALL_METHOD_SYSENTER && 
                              INTERNAL_OPTION(detach_fix_sysenter_on_stack));
            LOG(GLOBAL, LOG_ALL, 1, 
                "Detach : thread %d had %d stacked callbacks\n", threads[i]->id, count);
        } else {
            /* no saved callback state, done with this thread */
            LOG(GLOBAL, LOG_ALL, 1, 
                "Detach : thread %d has no stacked callbacks\n", threads[i]->id);
        }
    }
    
    /* Second walk (only needed for non-sysenter systemcalls).  Allocate and populate
     * the callback dispatch code and data structures. */
    if (num_stacked_callbacks > 0 &&
        (get_syscall_method() != SYSCALL_METHOD_SYSENTER ||
         !INTERNAL_OPTION(detach_fix_sysenter_on_stack))) {
        /* callback handling buf layout
         * {
         *   byte dispatch_code[DETACH_CALLBACK_CODE_SIZE];
         *   detach_callback_stack_t per_thread[num_threads_with_callbacks]
         *   app_pc callback_addrs[num_stacked_callbacks]
         * }
         * Not a real struct since variable size arrays. Note that nothing requires the
         * above elements to be in that order (or even in the same allocation).  We
         * allocate them together to save memory since we must leak this. FIXME - find
         * a way to free the allocation once we are finished with it. */
        int callback_buf_size = DETACH_CALLBACK_CODE_SIZE +
            num_threads_with_callbacks * sizeof(detach_callback_stack_t) +
            num_stacked_callbacks * sizeof(app_pc);
        /* FIXME - this should (along with any do/shared syscall containing gencode) be
         * allocated outside of our vmmheap so that we can free the vmmheap reservation
         * on detach. */
        byte *callback_buf = (byte *)heap_mmap(callback_buf_size);
        detach_callback_stack_t *per_thread =
            (detach_callback_stack_t *)(callback_buf + DETACH_CALLBACK_CODE_SIZE);
        app_pc *callback_addrs = (app_pc *)(&per_thread[num_threads_with_callbacks]);
        int j = 0; /* per_thread index */

        emit_detach_callback_code(GLOBAL_DCONTEXT, callback_buf, per_thread);
#ifdef X64 /* we only emit shared/do_syscall in shared_code on 64-bit */
        arch_patch_syscall(GLOBAL_DCONTEXT, callback_buf); /* patch the shared syscalls */
#endif

        for (i = 0; i < num_threads; i++) {
            dcontext_t *dcontext = threads[i]->dcontext;
            if (dcontext->prev_unused != NULL && dcontext->prev_unused->valid) {
                dcontext_t *tmp_dc = dcontext->prev_unused;
                
                arch_patch_syscall(dcontext, callback_buf);
                emit_detach_callback_final_jmp(dcontext, &(per_thread[j]));
                per_thread[j].callback_addrs = callback_addrs;
                per_thread[j].tid = dcontext->owning_thread;
                per_thread[j].count = 0;
                
                /* NOTE - we are walking the stacked dcontexts in reverse order
                 * (see callback.c, the last dcontext is considered the top of the
                 * stack).  This is ok since our emitted code expects this. */
                do {
                    *callback_addrs++ = POST_SYSCALL_PC(tmp_dc);
                    ASSERT((byte *)callback_addrs - (byte *)per_thread <=
                           callback_buf_size);
                    per_thread[j].count++;
                    tmp_dc = tmp_dc->prev_unused;
                } while (tmp_dc != NULL && tmp_dc->valid);
                
                j++;
            }
        }
        ASSERT(j == num_threads_with_callbacks);
        return true;
    }
    return false;
}

/* note not transparent while suspending since suspend count 
 * will be different, (and number of threads) */
/* FIXME : ? right now give each thread private code its own top heap_mmap so 
 * that can be left behind, is this too much of a hit, otherwise ok? */
void
detach_helper(int detach_type)
{
    thread_record_t **threads;
    dcontext_t *my_dcontext = get_thread_private_dcontext();
    int i, num_threads, my_thread_index = -1;
    thread_id_t my_id;
    bool res;
    bool *cleanup_tpc;
    bool translate_cxt;
    bool detach_stacked_callbacks;
    CONTEXT cxt;
    DEBUG_DECLARE(bool ok;)

    /* Caller (generic_nudge_handler) should have already checked these and
     * verified the nudge is valid. */
    ASSERT(dynamo_initialized && !dynamo_exited && my_dcontext != NULL);
    if (!dynamo_initialized || dynamo_exited || my_dcontext == NULL) {
        return;
    }
    
    /* enter DR after we have the detach lock for DEADLOCK_AVOIDANCE LIFO 
     * FIXME: for self-protection, though, we'll need this earlier so we
     * can write to the detach lock!
     */
    ENTERING_DR();
    
    /* dynamo_detaching_flag is not really a lock, 
       and since no one ever waits on it we can't deadlock on it either */
    if (!atomic_compare_exchange(&dynamo_detaching_flag,
                                 LOCK_FREE_STATE, LOCK_SET_STATE)) {
        EXITING_DR();
        return;
    }
    /* we'll need to unprotect for exit cleanup
     * FIXME: more secure to not do this until we've synched, but then need
     * alternative prot for doing_detach and init_apc_go_native*
     */
    SELF_UNPROTECT_DATASEC(DATASEC_RARELY_PROT);

    doing_detach = true;
    
    if (!internal_detach && !SYNCHRONIZE_DYNAMIC_OPTION(allow_detach)) {
        doing_detach = false;
        SELF_PROTECT_DATASEC(DATASEC_RARELY_PROT);
        dynamo_detaching_flag = LOCK_FREE_STATE;
        SYSLOG_INTERNAL_ERROR("Detach called without the allow_detach option set");
        EXITING_DR();
        return;
    }

    ASSERT(dynamo_initialized);
    ASSERT(!dynamo_exited);
    cxt.ContextFlags = CONTEXT_DR_STATE;
    my_id = get_thread_id();

    ASSERT(detach_type < DETACH_NORMAL_TYPE || 
           ((my_dcontext != NULL && my_dcontext->whereami == WHERE_FCACHE) ||
            /* If detaching in thin_client/hotp_only mode, must only be WHERE_APP!  */
            (RUNNING_WITHOUT_CODE_CACHE() && my_dcontext->whereami == WHERE_APP)));

    LOG(GLOBAL, LOG_ALL, 1, "Detach : thread %d starting\n", my_id);
    SYSLOG(SYSLOG_INFORMATION, INFO_DETACHING, 2, get_application_name(), 
           get_application_pid());

    /* synch with flush */
    if (my_dcontext != NULL)
        enter_threadexit(my_dcontext);

    /* signal to go native at APC init here, set pause first so that threads
     * will wait till we are ready for them to go native 
     * (after ntdll unpatching)*/
    /* note to avoid races these must be set in this order! */
    init_apc_go_native_pause = true;
    init_apc_go_native = true;
    /* see FIXME below about threads caught between the lock and initialization,
     * this just reduces the risk */
    thread_yield();

#ifdef CLIENT_INTERFACE
    /* make sure client nudges are finished */
    wait_for_outstanding_nudges();
#endif

    /* suspend all dynamo controlled threads at safe locations */
    DEBUG_DECLARE(ok =)
        synch_with_all_threads(THREAD_SYNCH_SUSPENDED_VALID_MCONTEXT, &threads, 
                               /* Case 6821: allow other synch-all-thread uses that beat
                                * us to not wait on us.  We still have a problem
                                * if we go first since we must xfer other threads.
                                */
                               &num_threads, THREAD_SYNCH_NO_LOCKS_NO_XFER,
                               /* if we fail to suspend a thread (e.g., privilege
                                * problems) ignore it. FIXME: retry instead? */
                               THREAD_SYNCH_SUSPEND_FAILURE_IGNORE);
    ASSERT(ok);
    /* now we own the thread_initexit_lock */
    /* NOTE : we will release the locks grabbed in synch_with_all_threads
     * below after cleaning up all the threads in case we will need to grab
     * it during process exit cleanup */
    ASSERT(mutex_testlock(&all_threads_synch_lock) && 
           mutex_testlock(&thread_initexit_lock));

#ifdef HOT_PATCHING_INTERFACE
    /* In hotp_only mode, we must remove patches when detaching; we don't want 
     * to leave in all our hooks and detach; that will definitely crash the app.
     */
    if (DYNAMO_OPTION(hotp_only))
        hotp_only_detach_helper();
#endif
    /* FIXME : NYI now all we know about are suspended, should do safety check for 
     * additional threads here, race condition may be threads that were passed the 
     * init_apc lock, but not yet initialized and so didn't show up on list */
    
    /* FIXME : if we hooked the image entry point and haven't unhooked it yet
     * need to do so now, can tell from callback hack since see thread with
     * LOST_CONTROL_AT_CALLBACK in the under_dynamo_control bool */
    {
        bool did_unhook = false;
        for (i = 0; i < num_threads; i++) {
            if (threads[i]->under_dynamo_control == UNDER_DYN_HACK) {
                LOG(GLOBAL, LOG_ALL, 1, 
                    "Detach : unpatching image entry point (from thread %d)\n",
                    threads[i]->id);
                ASSERT(!did_unhook); /* should only happen once, at most! */
                did_unhook = true;
                remove_image_entry_trampoline();
            }
        }
        if (!did_unhook) {
            /* case 9347/9475 if detaching before we have taken over the primary thread */
            if (dr_injected_secondary_thread && !dr_late_injected_primary_thread) {
                LOG(GLOBAL, LOG_ALL, 1, 
                    "Detach : unpatching image entry point (from primary)\n");
                did_unhook = true;
                /* note that primary thread is unknown and therefore not suspended */
                remove_image_entry_trampoline();
            }
        }
    }

    /* unpatch ntdll.dll, revert memory protections */
    LOG(GLOBAL, LOG_ALL, 1, 
        "Detach : about to unpatch ntdll.dll and fix memory permissions\n");
    /* FIXME : will go ahead and check option, though detach probably won't work with
     * noasynch anyways */
    if (!INTERNAL_OPTION(noasynch)) {
        callback_interception_unintercept();
    }
    if (!DYNAMO_OPTION(thin_client))
        revert_memory_regions();
    LOG(GLOBAL, LOG_ALL, 1, 
        "Detach : unpatched ntdll.dll and fixed memory permissions\n");

    /* release APC init lock, let any threads waiting there go native */
    LOG(GLOBAL, LOG_ALL, 1, "Detach : Releasing init_apc_go_native_pause\n");
    init_apc_go_native_pause = false;

    /* perform exit tasks that require full thread data structs */
    dynamo_process_exit_with_thread_info();

    /* note that no dynamorio code will be run by any other thread than this 
     * one from now on once APC's blocked at lock clear the method and go 
     * native */
    LOG(GLOBAL, LOG_ALL, 1, "Detach : starting to translate contexts\n");

    /* prefer not to do thread cleanup here as can be slow and we are blocking
     * the whole process, cleanup after resumed, need shadow list to know
     * if should free thread private code or not
     */
    cleanup_tpc = (bool *)global_heap_alloc(num_threads*sizeof(bool)
                                            HEAPACCT(ACCT_OTHER));

    /* Handle any outstanding callbacks */
    detach_stacked_callbacks =
        detach_helper_handle_callbacks(num_threads, threads, cleanup_tpc);

    /* translate current context */
    for (i = 0; i < num_threads; i++) {
        dcontext_t *dcontext = threads[i]->dcontext;
        translate_cxt = true;
        /* note is safe to check via id since no new thread records have been
         * created since threads was grabbed */
        if (threads[i]->id == my_id) {
            my_thread_index = i;
            continue;
        }
        res = thread_get_context(threads[i], &cxt);
        ASSERT(res);
        /* FIXME : callback UNDER_DYN_HACK hack again */
        if (threads[i]->under_dynamo_control == UNDER_DYN_HACK) {
            LOG(GLOBAL, LOG_ALL, 1, 
                "Detach : thread %d running natively since lost control at callback "
                "return and have not regained it, no need to translate context\n", 
                threads[i]->id);
            /* We don't expect to be at do_syscall (and therefore require translation
             * even though native) since we should've re-taken over by then. */
            ASSERT(!is_at_do_syscall(dcontext, (app_pc)cxt.CXT_XIP,
                                     (byte *)cxt.CXT_XSP));
            translate_cxt = false;
        }
        if (translate_cxt) {
            LOG(GLOBAL, LOG_ALL, 1, 
                "Detach : recreating address for "PFX"\n", cxt.CXT_XIP);
            /* fine to call this for native thread, will return cxt right back */
            res = translate_context(threads[i], &cxt, true/*restore memory*/);
            ASSERT(res);
            if (!threads[i]->under_dynamo_control) {
                LOG(GLOBAL, LOG_ALL, 1, 
                    "Detach : thread %d already running natively\n", 
                    threads[i]->id);
                /* we do need to restore the app ret addr, for native_exec */
                if (!DYNAMO_OPTION(thin_client) && DYNAMO_OPTION(native_exec) &&
                    !vmvector_empty(native_exec_areas)) {
                    /* We store the retaddr location that we clobbered in
                     * native_exec_retloc.  We don't currently follow callbacks
                     * so we don't have to do this while walking the callback
                     * stack below, just for this dcontext.  We also only have a
                     * single location since we don't re-takeover while native on
                     * an APC or an exception.
                     */
                    app_pc *esp = (app_pc *) threads[i]->dcontext->native_exec_retloc;
                    app_pc real_retaddr = threads[i]->dcontext->native_exec_retval;

                    /* In hotp_only mode, a thread can be !under_dynamo_control 
                     * and have no native_exec_retloc.  For hotp_only, there 
                     * should be no need to restore a return value on the stack 
                     * as the thread has been native from the start and not 
                     * half-way through as it would in the regular hot patching 
                     * mode, i.e., with the code cache.  See case 7681. 
                     */
#ifdef HOT_PATCHING_INTERFACE
                    if (esp == NULL) {
                        ASSERT(DYNAMO_OPTION(hotp_only));
                        ASSERT(real_retaddr == NULL);
                    } else {
                        ASSERT(!DYNAMO_OPTION(hotp_only));
#endif
                        ASSERT(esp != NULL && 
                               *esp == (app_pc)back_from_native);
                        ASSERT(real_retaddr != NULL);
                        *esp = real_retaddr;
#ifdef HOT_PATCHING_INTERFACE
                    }
#endif
                }
            }
            /* handle special case of vsyscall, need to hack the return address
             * on the stack as part of the translation */
            if (get_syscall_method() == SYSCALL_METHOD_SYSENTER &&
                cxt.CXT_XIP == (ptr_uint_t) vsyscall_after_syscall) {
                ASSERT(get_os_version() >= WINDOWS_VERSION_XP);
                /* handle special case of vsyscall */
                /* case 5441 Sygate hack means after_syscall will be at
                 * esp+4 (esp will point to sysenter_ret_address in ntdll) */
                if (*(cache_pc *)(cxt.CXT_XSP+
                                  (DYNAMO_OPTION(sygate_sysenter) ? XSP_SZ : 0)) ==
                    after_do_syscall_code(dcontext) ||
                    *(cache_pc *)(cxt.CXT_XSP+
                                  (DYNAMO_OPTION(sygate_sysenter) ? XSP_SZ : 0)) ==
                    after_shared_syscall_code(dcontext)) {
                    LOG(GLOBAL, LOG_ALL, 1, 
                        "Detach : thread %d suspended at vsysall with ret to after "
                        "shared syscall, fixing up by changing ret to "PFX"\n", 
                        threads[i]->id, POST_SYSCALL_PC(dcontext));
                    /* need to restore sysenter_storage for Sygate hack */
                    if (DYNAMO_OPTION(sygate_sysenter))
                        *(app_pc *)(cxt.CXT_XSP+XSP_SZ) = dcontext->sysenter_storage;
                    *(app_pc *)cxt.CXT_XSP = POST_SYSCALL_PC(dcontext);
                } else {
                    LOG(GLOBAL, LOG_ALL, 1,
                        "Detach, thread %d suspended at vsyscall with ret to "
                        "unknown addr, must be running native!\n",
                        threads[i]->id);
                }
            }
            LOG(GLOBAL, LOG_ALL, 1, 
                "Detach : pc = "PFX" for thread %d\n", cxt.CXT_XIP, threads[i]->id);
            ASSERT(!is_dynamo_address((app_pc)cxt.CXT_XIP)&&
                   !in_fcache((app_pc)cxt.CXT_XIP));
            /* FIXME case 7457: if the thread is suspended after it
             * received a fault but before the kernel copied the faulting
             * context to the user mode structures for the handler, it could
             * result in a codemod exception that wouldn't happen natively!
             */
            /* FIXME: switch to using set_synched_thread_context() once we address
             * the context storage issue
             */
            res = thread_set_context(threads[i], &cxt);
            ASSERT(res);
        }

#ifdef CLIENT_INTERFACE
        /* If this is a client-owned thread then there is no app state to return it to
         * so we kill it here.  Note - we won't kill threads that have returned from
         * the client nudge routine, but the exit path there doesn't do anything that
         * would interfere with rest of the detach cleanup. */
        if (IS_CLIENT_THREAD(dcontext)) {
            bool terminated = nt_terminate_thread(dcontext->thread_record->handle, 0);
            ASSERT(terminated); /* Should always be able to terminate. */
        }
#endif
        /* resume thread */
        LOG(GLOBAL, LOG_ALL, 1, 
            "Detach : thread %d is being resumed in native context\n", 
            threads[i]->id);
        res = thread_resume(threads[i]);
        ASSERT(res);
    }

    if (detach_type == DETACH_BAD_STATE_NO_CLEANUP) {
        SYSLOG_INTERNAL_WARNING("finished detaching, skipping cleanup");
        /* do a quick exit, skipping all cleanup except eventlog */
        eventlog_fast_exit();
        /* We don't even unload our dll since it's no longer required to unload
         * our dll for proper tools function. */
        /* FIXME : since we reached detach_helper via a clean call out of the
         * cache, if we return we will return back into the cache! It would be
         * cleaner for the thread to die by returning from its start function,
         * but to avoid complications we just kill it here. */
        /* NOTE - ref case 4923 (2k3sp1 doesn't free the LdrLock when the owning
         * thread dies unlike earlier versions).  With the fix for that case we
         * should no longer be holding any application locks at this point. */
        nt_terminate_thread(NT_CURRENT_THREAD, 0);
        ASSERT_NOT_REACHED();
        return;
    }
    
    /* assert that we found the index of the detaching thread in the 
     * threads thread_record_t array */
    ASSERT(detach_type < DETACH_NORMAL_TYPE || my_thread_index != -1);
    for (i = 0; i < num_threads; i++) {
        /* clean up threads, including us, but do us last in case cleanup
         * routines call is_self_* style routines */
        if (i != my_thread_index) {
            if (cleanup_tpc[i]) {
                LOG(GLOBAL, LOG_ALL, 1, 
                    "Detach : cleaning up thread %d, including its TPC\n", threads[i]->id);
                dynamo_other_thread_exit(threads[i], false);
            } else {
                LOG(GLOBAL, LOG_ALL, 1,
                    "Detach : cleaning up thread %d, but not its TPC\n",
                    threads[i]->id);
                dynamo_other_thread_exit(threads[i], true);
            }
        }
    }
    /* now free the detaching thread's dcontext */
    if (my_thread_index != -1)
        dynamo_other_thread_exit(threads[my_thread_index], false);

    /* free list of threads and cleanup_tpc */
    global_heap_free(cleanup_tpc, num_threads*sizeof(bool) HEAPACCT(ACCT_OTHER));
    end_synch_with_all_threads(threads, num_threads, false/*no resume*/);

    /* FIXME : NYI check that any threads waiting at APC have left dynamo code 
     * and interception code (will be cleaned up in shared_exit), 
     * potential race condition with unloading the dll, what if is suspended? 
     */
    thread_yield();

    LOG(GLOBAL, LOG_ALL, 1, 
        "Detach :  Last message from detach, about to clean up some more memory and unload\n");
    SYSLOG_INTERNAL_INFO("Detaching from process, entering final cleanup");
    /* call dynamo exit routines */
    res = dynamo_shared_exit(detach_stacked_callbacks); 
    ASSERT(res == SUCCESS);

    /* we can free the initstack, it can't be our stack, we are specially created thread */
    stack_free(initstack, DYNAMORIO_STACK_SIZE);
    dynamo_initialized = false;

    /* FIXME : ? have we freed all space, released all handles */
    
    /* CHECK: by now EXITING_DR should have silently happened */

    /* FIXME : unload dll, be able to have thread continue etc. */

    /* FIXME : since we reached detach_helper via a clean call out of the
     * cache, if we return we will return back into the cache! It would be
     * cleaner for the thread to die by returning from its start function,
     * but to avoid complications we just kill it here. */
    /* NOTE - ref case 4923 (2k3sp1 doesn't free the LdrLock when the owning
     * thread dies unlike earlier versions).  With the fix for that case we
     * should no longer be holding any application locks at this point. */
    nt_terminate_thread(NT_CURRENT_THREAD, 0);
    ASSERT_NOT_REACHED();
    return;
}

/* FIXME : we create a thread to do the detaching, and all other dlls will
 * be notifed of its creation by dll_thread_attach, is a transparency issue. */
/* sets detach in motion and then returns */
void
detach_internal()
{
    SELF_UNPROTECT_DATASEC(DATASEC_RARELY_PROT);
    internal_detach = true;
    /* we go ahead and re-protect though detach thread will soon un-prot */
    SELF_PROTECT_DATASEC(DATASEC_RARELY_PROT);
    LOG(GLOBAL, LOG_ALL, 1, "Starting detach\n");
    nudge_internal(NUDGE_GENERIC(detach), NULL, 0 /* ignored */);
    LOG(GLOBAL, LOG_ALL, 1, "Created detach thread\n");
}

/* mcontext must be valid, including the pc field (native) and app_errno
 * must not be holding any locks */
/* sets detach in motion and never returns */
void
detach_internal_synch()
{
    dcontext_t *dcontext = get_thread_private_dcontext();
    detach_internal();
    /* to be safe with flush */
    enter_threadexit(dcontext);
    /* make sure we spin forever */
    adjust_wait_at_safe_spot(dcontext, 1);
    check_wait_at_safe_spot(dcontext, THREAD_SYNCH_VALID_MCONTEXT);
}

bool
is_thread_currently_native(thread_record_t *tr)
{
    return (!tr->under_dynamo_control ||
            tr->under_dynamo_control == UNDER_DYN_HACK);
}

/* contended path of mutex operations */

static contention_event_t
mutex_get_contended_event(contention_event_t *contended_event, EVENT_TYPE event_type)
{
    contention_event_t ret = *contended_event;
    if (ret == CONTENTION_EVENT_NOT_CREATED)
    {
        contention_event_t new_event;
        bool not_yet_created;
        /* not signaled */
        /* EVENT_ALL_ACCESS, although observed access mask of 0x100003 (SYNCHRONIZE|0x3) */
        new_event = nt_create_event(event_type); 

        not_yet_created =
            atomic_compare_exchange_ptr((ptr_uint_t*)contended_event, 
                                        (ptr_uint_t)CONTENTION_EVENT_NOT_CREATED,
                                        (ptr_uint_t)new_event);
        if (not_yet_created) {
            /* we were first to create it */
            ret = new_event;
        } else {
            /* already created by someone else */
            ret = *contended_event;
            close_handle(new_event);
        }
    }
    ASSERT(ret != CONTENTION_EVENT_NOT_CREATED);
    return ret;
}

/* common wrapper that also attempts to detect deadlocks */
static void
os_wait_event(event_t e _IF_CLIENT_INTERFACE(bool set_safe_for_synch)
              _IF_CLIENT_INTERFACE(dcontext_t *dcontext))
{
    wait_status_t res;
    bool reported_timeout = false;

    KSTART(wait_event);
    /* we allow using this in release builds as well */
    if (DYNAMO_OPTION(deadlock_timeout) > 0) {
        LARGE_INTEGER timeout;
        timeout.QuadPart= - ((int)DYNAMO_OPTION(deadlock_timeout)) * 
            TIMER_UNITS_PER_MILLISECOND;
#ifdef CLIENT_INTERFACE
        /* if set_safe_for_synch dcontext must be non-NULL */
        ASSERT(!set_safe_for_synch || dcontext != NULL);
        if (set_safe_for_synch)
            dcontext->client_data->client_thread_safe_for_synch = true;
#endif
        res = nt_wait_event_with_timeout(e, &timeout /* debug timeout */);
#ifdef CLIENT_INTERFACE
        if (set_safe_for_synch)
            dcontext->client_data->client_thread_safe_for_synch = false;
#endif
        if (res == WAIT_SIGNALED) {
            KSTOP(wait_event);
            return;             /* all went well */
        }
        ASSERT(res == WAIT_TIMEDOUT);
        /* We could use get_own_peb()->BeingDebugged to determine whether
         * there was a debugger, but we can't just ignore this. It's better
         * to explicitly overwrite the hidden DO_ONCE variable from a debugging
         * session if this is getting in the way.
         */
        /* FIXME - instead of DO_ONCE we may want a named static variable that
         * we can access easily from the debugger. */
        DO_ONCE({
            reported_timeout = true;
            report_dynamorio_problem(NULL, DUMPCORE_TIMEOUT, NULL, NULL,
                                     "Timeout expired - 1st wait, possible deadlock (or you were debugging)");
            /* do a 2nd wait so we can get two dumps to compare for progress */
            /* FIXME - use shorter timeout for the 2nd wait? */
            res = nt_wait_event_with_timeout(e, &timeout /* debug timeout */);
            if (res == WAIT_SIGNALED) {
                /* 2nd wait succeeded! We must not have been really deadlocked.
                 * Syslog a warning to ignore the first ldmp and continue. */
                /* FIXME - should we reset the DO_ONCE now? */
                /* FIXME - should this be a report_dynamorio_problem or some
                 * such so is more useful in release builds? */
                SYSLOG_INTERNAL_WARNING("WARNING - 2nd wait after deadlock timeout expired succeeded! Not really deadlocked.");
                KSTOP(wait_event);
                return;
            }
            ASSERT(res == WAIT_TIMEDOUT);
            report_dynamorio_problem(NULL, DUMPCORE_TIMEOUT, NULL, NULL,
                                     "Timeout expired - 2nd wait, possible deadlock (or you were debugging)");
        });
    }
    /* fallback to waiting forever */
#ifdef CLIENT_INTERFACE
    if (set_safe_for_synch)
        dcontext->client_data->client_thread_safe_for_synch = true;
#endif
    res = nt_wait_event_with_timeout(e, INFINITE_WAIT);
#ifdef CLIENT_INTERFACE
    if (set_safe_for_synch)
        dcontext->client_data->client_thread_safe_for_synch = false;
#endif
    ASSERT(res == WAIT_SIGNALED);
    if (reported_timeout) {
        /* Our wait eventually succeeded so not truely a deadlock.  Syslog a
         * warning to that effect. */
        /* FIXME - should we reset the DO_ONCE now? */
        /* FIXME - should this be a report_dynamorio_problem or some
         * such so is more useful in release builds? */
        SYSLOG_INTERNAL_WARNING("WARNING - Final wait after reporting deadlock timeout expired succeeded! Not really deadlocked.");
    }
    KSTOP(wait_event);
}

void
mutex_wait_contended_lock(mutex_t *lock)
{
    contention_event_t event = 
        mutex_get_contended_event(&lock->contended_event, SynchronizationEvent);
#ifdef CLIENT_INTERFACE
    dcontext_t *dcontext = get_thread_private_dcontext();
    bool set_safe_for_sync =
        (dcontext != NULL && IS_CLIENT_THREAD(dcontext) &&
         (mutex_t *)dcontext->client_data->client_grab_mutex == lock);
    ASSERT(!set_safe_for_sync || dcontext != NULL);
#endif
    os_wait_event(event _IF_CLIENT_INTERFACE(set_safe_for_sync)
                  _IF_CLIENT_INTERFACE(dcontext));
    /* the event was signaled, and this thread was released,
       the auto-reset event is again nonsignaled for all other threads to wait on
    */
}

void
mutex_notify_released_lock(mutex_t *lock)
{
    contention_event_t event = 
        mutex_get_contended_event(&lock->contended_event, SynchronizationEvent);
    nt_set_event(event);
}

void
rwlock_wait_contended_writer(read_write_lock_t *rwlock)
{
    contention_event_t event = 
        mutex_get_contended_event(&rwlock->writer_waiting_readers, SynchronizationEvent);
    os_wait_event(event _IF_CLIENT_INTERFACE(false) _IF_CLIENT_INTERFACE(NULL));
    /* the event was signaled, and this thread was released,
       the auto-reset event is again nonsignaled for all other threads to wait on
    */
}

void
rwlock_notify_writer(read_write_lock_t *rwlock)
{
    contention_event_t event = 
        mutex_get_contended_event(&rwlock->writer_waiting_readers, SynchronizationEvent);
    nt_set_event(event);
}

/* The current implementation uses auto events and will wake up only a
   single reader.  We then expect each of them to wake up any other ones
   by properly counting.
*/
void
rwlock_wait_contended_reader(read_write_lock_t *rwlock)
{
    contention_event_t notify_readers = 
        mutex_get_contended_event(&rwlock->readers_waiting_writer, SynchronizationEvent);
    os_wait_event(notify_readers _IF_CLIENT_INTERFACE(false) _IF_CLIENT_INTERFACE(NULL));
    /* the event was signaled, and only a single threads waiting on this event are released,
       if this was indeed the last reader
    */
}

void
rwlock_notify_readers(read_write_lock_t *rwlock)
{
    contention_event_t notify_readers = 
        mutex_get_contended_event(&rwlock->readers_waiting_writer, SynchronizationEvent);
    /* this will wake up only one since we're using an auto event */
    nt_set_event(notify_readers);
}

/***************************************************************************/

event_t
create_event()
{
    return nt_create_event(SynchronizationEvent);
}

void
destroy_event(event_t e)
{
    nt_close_event(e);
}

void
signal_event(event_t e)
{
    nt_set_event(e);
}

void
reset_event(event_t e)
{
    /* should be used only for manual events (NotificationEvent) */
    nt_clear_event(e);
}

void
wait_for_event(event_t e)
{
    os_wait_event(e _IF_CLIENT_INTERFACE(false) _IF_CLIENT_INTERFACE(NULL));
}

timestamp_t
get_timer_frequency()
{
    LARGE_INTEGER ignore_tsc;
    LARGE_INTEGER freq;
    timestamp_t processor_speed;

    nt_query_performance_counter(&ignore_tsc, /* not optional */
                                 &freq);
    DOLOG(2, LOG_ALL, {
        timestamp_t tsc;
        RDTSC_LL(tsc);

        LOG(GLOBAL, LOG_ALL, 2, 
            "Starting RDTSC: "UINT64_FORMAT_STRING
            " nt_query_performance_counter: "UINT64_FORMAT_STRING
            " freq:" UINT64_FORMAT_STRING "\n",
            tsc, ignore_tsc.QuadPart, freq.QuadPart);
    });

    processor_speed = freq.QuadPart / 1000; /* convert to KHz */
    /* case 2937 - windows sometimes is using RTC */
    if (processor_speed < 500*1000 /* considering 500 MHz too low for a modern machine */) {
        processor_speed = 2937*1000;
        LOG(GLOBAL, LOG_ALL, 1, "get_timer_frequency: OS is using RTC!  Reported speed is bogus.\n");
    }
    return processor_speed;
}

uint
os_random_seed()
{
    LARGE_INTEGER tsc_or_rtc;   
    uint seed = (uint) get_thread_id();
    seed ^= query_time_millis();

    /* safer to use than RDTSC, since it defaults to real time clock
     * if TSC is not available, either one is good enough for randomness
     */
    nt_query_performance_counter(&tsc_or_rtc, NULL);
    seed ^= tsc_or_rtc.LowPart;
    seed ^= tsc_or_rtc.HighPart;

    LOG(GLOBAL, LOG_ALL, 1, "os_random_seed: %d\n", seed);
    return seed;
}

void
early_inject_init()
{
    dcontext_t *dcontext = get_thread_private_dcontext();
    module_handle_t mod;
    bool under_dr_save;
    where_am_i_t whereami_save;
    wchar_t buf[MAX_PATH];
    int os_version = get_os_version();
    GET_NTDLL(LdrLoadDll, (IN PCWSTR PathToFile OPTIONAL,
                           IN PULONG Flags OPTIONAL,
                           IN PUNICODE_STRING ModuleFileName,
                           OUT PHANDLE ModuleHandle));
    ASSERT(dcontext != NULL);

    early_inject_location = DYNAMO_OPTION(early_inject_location);

    /* check for option override of the address */
    if (DYNAMO_OPTION(early_inject_location) == INJECT_LOCATION_LdrCustom) {
        early_inject_address = (app_pc)DYNAMO_OPTION(early_inject_address);
        ASSERT(early_inject_address != NULL);
        LOG(GLOBAL, LOG_TOP, 1,
            "early_inject using option provided address "PFX" at location %d\n",
            early_inject_address, early_inject_location);
        return;
    }

    /* We only need to figure out the address for Ldr* locations */
    if (!INJECT_LOCATION_IS_LDR(early_inject_location)) {
        LOG(GLOBAL, LOG_TOP, 1,
            "early_inject is using location %d, no need to find address\n",
            early_inject_location);
        return;
    }

    /* Figure out which location we're using, keep in synch with
     * LdrpLoadImportModule check in options.c */
    if (DYNAMO_OPTION(early_inject_location) == INJECT_LOCATION_LdrDefault) {
        LOG(GLOBAL, LOG_TOP, 2,
            "early_inject using default ldr location for this os_ver\n");
        switch (os_version) {
        case WINDOWS_VERSION_NT:
            /* LdrpImportModule is best but we can't find that address
             * automatically since one of the stack frames we need to walk
             * for it doesn't use frame ptrs (we can get LdrpLoadDll though),
             * LdrpLoadDll seems to work fairly well, but won't get us in til
             * after some of the static dlls are loaded. */
            /* if someone provided a location for us go ahead and use that on
             * the presumption they're providing LdrpLoadImportModule for us.
             */
            if (DYNAMO_OPTION(early_inject_address != 0)) {
                early_inject_address =
                    (app_pc)DYNAMO_OPTION(early_inject_address);
                LOG(GLOBAL, LOG_TOP, 1,
                    "early_inject using option provided address "PFX" at location %d\n",
                    early_inject_address, early_inject_location);
                return;
            }
            /* Case 7806, on some NT machines LdrpLoadDll causes problems
             * while on others it doesn't.  Just turn off early injection
             * on NT for now (LdrpLoadDll wasn't giving very good aslr support
             * anyways and isn't a desktop target).  FIXME - we could just
             * hardcode a table of LdrpLoadImportModule addresses for NT since
             * we don't expect Microsoft to release any more patches for it.
             */
            options_make_writable();
            dynamo_options.early_inject = false;
            options_restore_readonly();
            return;
        case WINDOWS_VERSION_2000:
            /* LdrpImportModule is best, LdrpLoadDll kind of works but won't
             * get us in til after most of the static dlls are loaded */ 
            early_inject_location = INJECT_LOCATION_LdrpLoadImportModule;;
            break;
        case WINDOWS_VERSION_XP:
            /* LdrpLoadDll is best, LdrpLoadImportModule also works but it
             * misses the load of kernel32 */
            early_inject_location = INJECT_LOCATION_LdrpLoadDll;
            break;
        case WINDOWS_VERSION_2003:
        case WINDOWS_VERSION_VISTA:
            /* LdrLoadDll is best but LdrpLoadDll seems to work just as well
             * (FIXME would it be better just to use that so matches XP?),
             * LdrpLoadImportModule also works but it misses the load of
             * kernel32. */
            early_inject_location = INJECT_LOCATION_LdrLoadDll;
            break;
        default:
            /* is prob. a newer windows version so the 2003 location is the
             * most likely to work */
            early_inject_location = INJECT_LOCATION_LdrLoadDll;
            ASSERT_NOT_REACHED();
        }
    }
    ASSERT(early_inject_location != INJECT_LOCATION_LdrDefault);
    LOG(GLOBAL, LOG_TOP, 1,
        "early_inject is using location %d, finding address\n",
        early_inject_location);

    /* check if we already have the right address */
    if (dr_early_injected &&
        INJECT_LOCATION_IS_LDR_NON_DEFAULT(early_inject_location) &&
        early_inject_location == dr_early_injected_location
        /* don't use parent's address if stress option set */
        && !(INTERNAL_OPTION(early_inject_stress_helpers) && 
             early_inject_location == INJECT_LOCATION_LdrpLoadImportModule)) {
        /* We've got the right address to use already (from parent) */
        early_inject_address = parent_early_inject_address;
        ASSERT(early_inject_address != NULL);
        ASSERT(early_inject_location != INJECT_LOCATION_LdrLoadDll ||
               early_inject_address == (app_pc)LdrLoadDll);
        LOG(GLOBAL, LOG_TOP, 1,
            "early_inject using parent supplied address "PFX"\n",
            early_inject_address);
        return;
    }

    switch (early_inject_location) {
    case INJECT_LOCATION_LdrLoadDll:
        early_inject_address = (app_pc)LdrLoadDll;
        break;
    case INJECT_LOCATION_LdrpLoadDll:
        /* If we were early injected have to have already gotten this address
         * from parent as our DllMain stack walk will have gotten the wrong
         * locations (during process init the Ldr delays calling DllMains
         * until all static dlls are loaded unless GetProcAddress is called
         * on the dll first, in that case its DllMain is called from there
         * not LdrpLoadDll as we expect). */
        /* FIXME - we could use a helper dll to get this, but it won't work
         * when early_injected for the same reason dr's DllMain walk doesn't.
         * Maybe there's some flag we can pass to the Ldr to tell it to call
         * the DllMain right away (could then use it when trampoline loads
         * dr dll).  Other option is we could wait and use the helper dll
         * once the Ldr is in a state where it will do what we expect
         * (the image entry point would qualify, though we could prob. find
         * somewhere earlier then that, say when we see the execution of the
         * DllMain of one of the non ntdll system dlls or something).  That
         * said in the product I expect any given platform (let alone machine)
         * to always use the same inject location. */
        ASSERT_NOT_IMPLEMENTED(!dr_early_injected && "process early injected"
                               "at non LdrpLoadDll location is configured to"
                               "use LdrpLoadDll location which is NYI");
        if (os_version == WINDOWS_VERSION_NT)
            early_inject_address = ldrpLoadDll_address_NT;
        else
            early_inject_address = ldrpLoadDll_address_not_NT;
        break;
    case INJECT_LOCATION_LdrpLoadImportModule:
        /* We use helper dlls to determine this address at runtime.  We pretend
         * to be a native_exec thread and load drearlyhelper1.dll which
         * statically links to drearlyhelper2.dll.  We watch for the
         * NtMapViewOfSection call that loads drearlyhelper2.dll in
         * syscall_while_native. At that point we expect the stack to look
         * like this:
         *   (in NtMapViewOfSection)
         *   ntdll!LdrpMapDll
         *   ntdll!LdrpLoadImportModule (what we want)
         * After that don't really care (is one of the Ldrp*ImportDescriptor*
         * routines. So we walk the stack back and get the desired address.
         */
        ASSERT(DYNAMO_OPTION(native_exec_syscalls));
        LOG(GLOBAL, LOG_ALL, 1,
            "early_inject using helper dlls to find LdrpLoadImportModule\n");

        /* Pretend to be native, so Ki & Ldr hooks don't bother us. NOTE that
         * since we're still pre dynamo_initialized no other threads can be
         * running in dr code (so we're ok with the synch routines which could
         * otherwise be a problem since we're still on the appstack at this
         * point so could pass at_safe_spot while we were native). Hotpatch
         * nudge dll loading does the same trick. This does assume that,
         * like hotpatch nudge, we aren't running on the dstack as that
         * will be clobbered. Alt. we could remove the KSTATS issue and
         * the stack restriction by special casing this thread in
         * syscall_while_native (just let all system calls run natively except
         * MapViewOfSection which we do there so we can check the result). */
        ASSERT(!is_currently_on_dstack(dcontext));
        under_dr_save = dcontext->thread_record->under_dynamo_control;
        dcontext->thread_record->under_dynamo_control = false;
        whereami_save = dcontext->whereami;
        /* FIXME - this is an ugly hack to get the kstack in a form compatible
         * with dispatch for processing the native exec syscalls we'll hit
         * while loading the helper dll (hotpatch has a similar issue but
         * lucks out with having a compatible stack).  Shouldn't mess things
         * up too much though.  We do have to use non-matching stops so not
         * sure how accurate these times will be (should be tiny anyways)
         * should poke around dispatch sometime and figure out some way to
         * do this nicer. */
        KSTART(dispatch_num_exits);
        KSTART(dispatch_num_exits);
        
        string_option_read_lock();
        snwprintf(buf, BUFFER_SIZE_ELEMENTS(buf), L"%hs",
                  DYNAMO_OPTION(early_inject_helper_dll));
        NULL_TERMINATE_BUFFER(buf);
        string_option_read_unlock();
        /* load the helper library, post syscall hook will fill in
         * ldrpLoadImportModule_address for us */
        early_inject_load_helper_dcontext = dcontext;
        /* FIXME : if we are early_injected and the load fails because either
         * of the helper dlls don't exist/can't be found the Ldr treats
         * that as a process init failure and aborts the process.  Wonder if
         * there's a flag we can pass to the Ldr to tell it not to do that.
         * Anyways, in normal usage we expect to use the parent's address when
         * early_injected (would only fail to do so if the parent was using
         * a different inject_location which would be unexpected in a product
         * configuration). */
        EXITING_DR();
        /* FIXME - we are making the assumption (currently true) that our
         * load_library() & free_library() routines themselves don't write to
         * any self protected regions, if changes we may need special versions
         * here. */
        mod = load_library(buf);
        if (mod != NULL) {
            free_library(mod);
        }
        ENTERING_DR();

        /* clean up & restore state */
        dcontext->whereami = whereami_save;
        early_inject_load_helper_dcontext = NULL;
        dcontext->thread_record->under_dynamo_control = under_dr_save;
        /* Undo the kstack hack (see comment above) */
        KSTOP_NOT_MATCHING_NOT_PROPAGATED(dispatch_num_exits);
        KSTOP_NOT_PROPAGATED(dispatch_num_exits);

        ASSERT(mod != NULL && ldrpLoadImportModule_address != NULL &&
               "check that drearlyhelp*.dlls are installed");

        /* FIXME - should we do anything if the address isn't found for some
         * reason (most likely would be the helper dlls didn't exist/couldn't
         * be found)?  Could choose to fall back to another os version
         * appropriate location. As is, in release build we'd just fail to
         * follow children when we couldn't find the address (see FIXME in
         * inject_into_process()).  I expect QA is going to run into this
         * occasionally (esp. till nodemgr etc. handle the helper dlls), so
         * can we do anything to make things easier/more apparent for them? */
        early_inject_address = ldrpLoadImportModule_address;
        break;
    default:
        ASSERT_NOT_REACHED();
    }
    
    /* FIXME - if failed to get address for any reason and we were early
     * injected, we could fall back to parent's address. */
    ASSERT(early_inject_address != NULL);
    /* Since we are using a non-overriden Ldr* location can assert that
     * early_inject_address is in ntdll */
    ASSERT(get_allocation_base(early_inject_address) == get_ntdll_base());
    LOG(GLOBAL, LOG_TOP, 1, "early_inject found address "PFX" to use\n",
        early_inject_location);
}

#define SECURITY_MAX_SID_STRING_SIZE                            \
    (2 + MAX_DWORD_STRING_LENGTH + 1 + MAX_DWORD_STRING_LENGTH  \
     + (MAX_DWORD_STRING_LENGTH * SID_MAX_SUB_AUTHORITIES) + 1)
    /* S-SID_REVISION- + IdentifierAuthority- + subauthorities- + NULL */

static
const char*
get_process_SID_string()
{
    static char process_SID[SECURITY_MAX_SID_STRING_SIZE];
    if (!process_SID[0]) {
        wchar_t sid_string[SECURITY_MAX_SID_STRING_SIZE];
        /* FIXME: we only need to query NtOpenProcessToken, but we'll
         * assume that this function is called early enough before any
         * impersonation could have taken place and NtOpenThreadToken
         */
        get_current_user_SID(sid_string, sizeof(sid_string));

        snprintf(process_SID, BUFFER_SIZE_ELEMENTS(process_SID), "%ls",
                 sid_string);
        NULL_TERMINATE_BUFFER(process_SID);
    }
    return process_SID;
}

static
const PSID
get_Everyone_SID()
{
    static PSID everyone_SID = NULL;
    static UCHAR everyone_buf[LengthRequiredSID(1)];

    if (everyone_SID == NULL) {
        SID_IDENTIFIER_AUTHORITY world = SECURITY_WORLD_SID_AUTHORITY;
        everyone_SID = (PSID)everyone_buf;
        initialize_known_SID(&world, SECURITY_WORLD_RID,
                             everyone_SID);
    }
    return everyone_SID;
}

/* default owner SID for created objects */
static
const PSID
get_process_owner_SID()
{
    static PSID owner_SID = NULL;
    /* owner SID will be self-referenced in TOKEN_OWNER */
    static UCHAR owner_buf[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_OWNER)];

    if (owner_SID == NULL) {
        PTOKEN_OWNER powner = (PTOKEN_OWNER)owner_buf;
        NTSTATUS res;
        ASSERT(!dynamo_initialized); /* .data still writable */
        /* initialization expected with os_user_directory() */
        res = get_primary_owner_token(powner, sizeof(owner_buf));
        ASSERT(NT_SUCCESS(res));
           
        if (!NT_SUCCESS(res)) {
            /* while we don't expect to fail even once, we better fail
             * all the time, otherwise we'll crash later when writing
             * to owner_buf */
            return NULL;
        }
        owner_SID = powner->Owner;
    }
    /* static buffer, no need to deallocate */
    return owner_SID;
}

static bool
os_validate_owner_equals(HANDLE file_or_directory_handle, PSID expected_owner)
{
    /* see comments in os_current_user_directory() */
    /* when this scheme would work.
     *
     * Note that we only allow files used by initial process, so we
     * must memoize initial SID.
     */

    /* Note on Unix this scheme doesn't work - anyone can chown(2)
     * a directory or file to pretend to be created by the victim
     * - we can only ask a trusted component to create a directory
     * writable only by the corresponding user.  On Windows,
     * however, chown() requries restore or TCB privileges.
     * therefore it doesn't present a privilege escalation route.
     */

    /* FIXME: If we do allow anyone to create their own directory,
     * then we'd have to verify it wasn't created by somebody else -
     * after we open a file we should validate that we are its
     * rightful owner (and we'll assume we have maintained the correct
     * ACLs) to maintain that nobody else could have had write access
     * to the file.
     */

    /* Note that we assume that TokenUser == TokenOwner, so all
     * created files' owner will be the current user (in addition to
     * being readable by the current user).  We also assume that the
     * cache\ directory is on the local system.
     * FIXME: case 10884 we can't assume that, we have to create our files explicitly
     *
     * (FIXME: unclear whether Machine account will be available for
     * us on the network for services)
     */

    /* FIXME: having a open handle to the directory instead of
     * concatenating strings would allow us to do the check only on
     * the directory, and not on the files.  We only need to make sure
     * there are no TOCTOU races: no symbolic links allowed, and
     * that directories cannot be renamed or deleted.
     */

    /* just owner */
    UCHAR sd_buf[SECURITY_MAX_SID_SIZE + sizeof(SECURITY_DESCRIPTOR)];
    PSECURITY_DESCRIPTOR sd = (PSECURITY_DESCRIPTOR)sd_buf;
    /* it is really SECURITY_DESCRIPTOR_RELATIVE */
    PSID owner;
    DWORD actual_sd_length;
    NTSTATUS res;

    /* This buffer must be aligned on a 4-byte boundary. */
    ASSERT(ALIGNED(sd, sizeof(DWORD)));

    /* FIXME: unlike SIDs which we can bound, there is no good bound
     * for a complete SD.  We need to ensure that only one SID would
     * be returned to us here.
     */

    /* We need READ_CONTROL access to the file_or_directory_handle */
    res = nt_query_security_object(file_or_directory_handle,
                                   OWNER_SECURITY_INFORMATION,
                                   sd,
                                   sizeof(sd_buf),
                                   &actual_sd_length);
    if (!NT_SUCCESS(res)) {
        if (res == STATUS_ACCESS_DENIED) {
            ASSERT_CURIOSITY(false && "verify handle allows READ_CONTROL");
        }
        return false;
    }
    ASSERT(actual_sd_length < sizeof(sd_buf));

    if (get_owner_sd(sd, &owner)) {
        /* FIXME: on Vista services using restricted SIDs may require
         * obtaining the SID that we can use for creating files */

        if (!equal_sid(owner, expected_owner)) {
            /* !sid poi(owner) */
            LOG(GLOBAL, LOG_TOP, 1,
                "os_validate_owner_equals: owner not matching expected_owner\n");

            return false;
        }
        return true;
    }

    ASSERT_NOT_REACHED();
    return false;
}

/* Recommended that callers check ownership of a file that is
 * guaranteed to not be writable.
 */
bool
os_filesystem_supports_ownership(HANDLE file_or_directory_handle)
{
    /* Can we verify we are on FAT32 in a documented way to be certain? */

    /* Currently done by checking if cache\ directory is Owned by
     * Everyone - which certainly should only happen on FAT32.
     */

    /* FIXME: Alternatively we can test for support for file
     * ID/reference, since creation by file reference is only
     * supported on NTFS
     */

    /* either FAT32 or we have a proper owner */
    if (os_validate_owner_equals(file_or_directory_handle, get_Everyone_SID())) {
        /* On FAT32 :
         * 0:000> !sid poi(owner)
         * SID is: S-1-1-0 Everyone
         *
         * We assume that a malicious user cannot set the SID to
         * Everyone.  Although Everyone is not the same as Anonymous
         * Logon S-1-5-7, just in case malware can run as Everyone and
         * creates a file we cannot decide we're on FAT32 just based
         * on this for files that.
         */

        SYSLOG_INTERNAL_WARNING_ONCE("cache root directory is on FAT32, no security\n");
                
        return false;
    } else {
        /* we have a real owner - presumably NTFS */
        return true;
    }
    return false;
}

/* opens the cache\ directory that should be modifed only by trusted users */
/* and is used by both ASLR and persistent cache trusted producers */
HANDLE
open_trusted_cache_root_directory(void)
{
    char base_directory[MAXIMUM_PATH];
    wchar_t wbuf[MAXIMUM_PATH];
    HANDLE directory_handle;
    int retval;

    retval = get_parameter(L_IF_WIN(DYNAMORIO_VAR_CACHE_ROOT),
                           base_directory, sizeof(base_directory));
    if (IS_GET_PARAMETER_FAILURE(retval) || 
        (strchr(base_directory, DIRSEP) == NULL &&
         strchr(base_directory, ALT_DIRSEP) == NULL)) {
        SYSLOG_INTERNAL_ERROR(" %s not set!\n", DYNAMORIO_VAR_CACHE_ROOT);
        return INVALID_HANDLE_VALUE;
    }
    NULL_TERMINATE_BUFFER(base_directory);

    if (!convert_to_NT_file_path(wbuf, base_directory, BUFFER_SIZE_ELEMENTS(wbuf)))
        return INVALID_HANDLE_VALUE;

    /* the cache root directory is supposed to be created by nodemgr
     * and owned by Administrators, and the directory ACL should not
     * allow changes.  We should not create one if it doesn't exist,
     * even if we did we wouldn't have the correct ACLs for its
     * children.
     */
    directory_handle = create_file(wbuf, true /* is_dir */, 
                                   READ_CONTROL /* generic rights */,
                                   FILE_SHARE_READ
                                   /* case 10255: allow persisted cache files
                                    * in same directory */
                                   | FILE_SHARE_WRITE,
                                   FILE_OPEN, true);
    if (directory_handle == INVALID_HANDLE_VALUE) {
        SYSLOG_INTERNAL_ERROR("%s=%s is invalid!",
                              DYNAMORIO_VAR_CACHE_ROOT, base_directory);
    }

    return directory_handle;
}

bool
os_user_directory_supports_ownership()
{
    /* should evaluate early so no need for .data unprotection */
    static int user_directory_has_ownership = -1;  /* not evaluated yet */
    /* note using explicit int, to not rely on bool true values */
    if (user_directory_has_ownership < 0) {
        if (DYNAMO_OPTION(validate_owner_dir) || 
            DYNAMO_OPTION(validate_owner_file)) {
            HANDLE root_handle = open_trusted_cache_root_directory();
            /* Note that if root_handle is INVALID_HANDLE_VALUE we
             * don't care about user_directory_has_ownership, it is
             * undefined.  Since all users that verify ownership
             * construct paths based on this directory, they should
             * all fail and we don't really care.  We assume that this
             * directory is created with correct privileges, so if
             * anyone controls the registry key or can create the
             * directory we have lost already.  (Interestingly,
             * nt_query_security_object() returns current user for
             * owner of -1, and so os_filesystem_supports_ownership()
             * does return true instead.)
             */
            if (os_filesystem_supports_ownership(root_handle))
                user_directory_has_ownership = 1;
            else 
                user_directory_has_ownership = 0;
            close_handle(root_handle);
        } else {
            user_directory_has_ownership = 0; /* nobody cares whether it supports */
        }
    }
    return (user_directory_has_ownership == 1);
}

/* validate we are the rightful owner */
/* Note. we assume all calls to os_validate_owner_equals are on the same volume as
 * DYNAMORIO_VAR_CACHE_ROOT.
 * Handle needs to have READ_CONTROL access (FILE_GENERIC_READ provides that).
 */
bool
os_validate_user_owned(HANDLE file_or_directory_handle)
{
    /* note that Creator and Owner don't have to match, but we expect
     * that we'll be creating new files with current token as owner
     */
    PSID process_SID = get_process_primary_SID();
    /* Note we only trust the primary token!  If we are impersonating,
     * we also need ACLs allowing us to open other files created by
     * the primary token */

    if (os_validate_owner_equals(file_or_directory_handle, process_SID)) {
        return true;
    }
    if (!os_user_directory_supports_ownership()) {
        /* Although on FAT32 there is no owner (or any other ACLs), we
         * get as owner Everyone.  Since file ACLs are unsupported by
         * file system on the system drive (where we install), we can
         * assume that privilege escalation is irrelevant for this
         * host.
         */
        /* nobody really cares about this owner validation on FAT32 */
        ASSERT(os_validate_owner_equals(file_or_directory_handle, get_Everyone_SID()));
        return true;
    }

    ASSERT_CURIOSITY(false && "unauthorized user tried to forge our files");
    return false;
}

/* append per-user directory name to provided directory_prefix,
 * and optionally create a new one if possible
 *
 * Note 'current' is actually the primary process token: we currently
 * allow only read-only access for impersonated threads.
 */
bool
os_current_user_directory(char *directory_prefix /* INOUT */,
                          uint directory_len,
                          bool create)
{
    char *directory = directory_prefix;
    char *dirend = directory_prefix + strlen(directory_prefix);
    snprintf(dirend, directory_len - (dirend - directory_prefix), "%c%s",
             DIRSEP, get_process_SID_string());
    directory_prefix[directory_len - 1] = '\0';

    directory = directory_prefix;
    LOG(GLOBAL, LOG_CACHE, 2, "\tper-user dir is %s\n", directory);
    DODEBUG({
        if (!equal_sid(get_process_owner_SID(),
                       get_process_primary_SID())) {
            LOG(GLOBAL, LOG_CACHE, 1, 
                "Default owner is not current user, we must be an Administrator?\n");
            /* FIXME: we could try to really check */
        }
    });

    /* Note that if an application impersonates threads, data for a
     * single application will be spread across different users secure
     * storage locations.  This may be a vulnerability - if a secure
     * server loads a DLL while impersonated we may be erroneously
     * using (without validation) a DLL controlled by lower privilege.
     * Delay-loaded DLLs may provide such unexpected DLL loads.
     *
     * ACLs: We may want to leave files readable by Everyone - allows
     * any impersonated threads to read files in the directory of the
     * original process token.  (Note that Anonymous token belongs to
     * Everyone).  World readable files also allow us to share files
     * produced by TCB services.  Yet, for stronger security against
     * local privilege exploits, there is some value in not allowing
     * anyone else to read our persistent files - the layout may be
     * useful to attackers; and general need to know principle:
     * normally other process don't need to read these.
     */

    /* FIXME: Of course, at beginning we want be dealing with
     * impersonation at all, but we should try to detect it here if we
     * fail to open a directory due to impersonated thread.
     */

    /* create directory if it doesn't exist */
    /* check for existence first so we can require new during creation */
    if (!os_file_exists(directory, true/*is dir*/) && create) {
        /* CREATE_DIR_FORCE_OWNER case 10884 - NoDefaultAdminOwner -
         * the default owner doesn't have to be the current user, if
         * member of Administrators.  Therefore we specify our own
         * SecurityDescriptor.Owner when creating a file so that we
         * don't use SE_OWNER_DEFAULTED, but we still want a default
         * DACL and we don't care about group.
         */

        /* FIXME: we should ensure we do not follow symlinks! */
        if (!os_create_dir(directory, CREATE_DIR_REQUIRE_NEW | CREATE_DIR_FORCE_OWNER)) {
            LOG(GLOBAL, LOG_CACHE, 2, 
                "\terror creating per-user dir %s\n", directory);

            /* FIXME: currently this is expected for the 4.2 ACLs */
            /* Note SYSLOG can be just a Warning since we will still
             * run correctly without persistence. */
            SYSLOG_INTERNAL_ERROR_ONCE("Persistent cache per-user needed.\n"
                                       "mkdir \"%s\"\n"
                                       "cacls \"%s\" /E /G username:F",
                                       /* Note cacls needs a real user
                                        * name, while subinacl does take SIDs */
                                       directory, directory);
            return false;
        } else {
            LOG(GLOBAL, LOG_CACHE, 2, 
                "\tcreated per-user dir %s\n", directory);
        }
    }
    
    /* FIXME: case 8812 if the cache\ directory inheritable ACLs are
     * setup accordingly we should be able to automatically create a
     * our own per-user folder, without dealing with forging ACLs
     * here, and without asking a trusted components to create it for
     * us.
     
     * currently each user MUST call os_validate_user_owned()
     * before trusting a file, or if a directory handle is
     * guaranteed to be open at all times such that renaming is
     * disallowed, then only the directory needs to be validated
     */

    return true;
}

/* checks for compatibility OS specific options, returns true if
 * modified the value of any options to make them compatible
 */
bool
os_check_option_compatibility(void)
{
    bool changed_options = false;
    bool os_has_aslr = get_os_version() >= (int)INTERNAL_OPTION(os_aslr_version);
    /* ASLR introduced in Vista Beta2, but we support only RTM+ so
     * WINDOWS_VERSION_VISTA */

    if (!os_has_aslr)
        return false;

    if (TEST(OS_ASLR_DISABLE_PCACHE_ALL, DYNAMO_OPTION(os_aslr))) {
        /* completely disable pcache */

        /* enabled by -desktop, but can be enabled independently as well */
        if (DYNAMO_OPTION(coarse_enable_freeze)) {
            dynamo_options.coarse_enable_freeze = false;
            changed_options = true;
        }
        if (DYNAMO_OPTION(coarse_freeze_at_unload)) {
            dynamo_options.coarse_freeze_at_unload = false;
            changed_options = true;
        }
        if (DYNAMO_OPTION(use_persisted)) {
            dynamo_options.use_persisted = false;
            changed_options = true;
        }
        if (changed_options)
            SYSLOG_INTERNAL_WARNING_ONCE("pcache completely disabled, Vista+");
    }

    /* note dynamorio.dll is not marked as ASLR friendly so we keep
     * using our own -aslr_dr */
    if (TEST(OS_ASLR_DISABLE_PCACHE_ALL, DYNAMO_OPTION(os_aslr))) {
        /* completely disable ASLR */
        /* enabled by -client, but can be enabled independently as well */
        if (DYNAMO_OPTION(aslr) != 0) {
            dynamo_options.aslr = 0;
            changed_options = true;
            SYSLOG_INTERNAL_WARNING_ONCE("ASLR completely disabled, Vista+");
        }
        if (DYNAMO_OPTION(aslr_cache) != 0) {
            dynamo_options.aslr_cache = 0;
            changed_options = true;
        }
    }
    ASSERT(os_has_aslr);
    return changed_options;
}
