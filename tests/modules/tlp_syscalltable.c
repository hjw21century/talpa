/*
 * talpa_syscalltable.c
 *
 * TALPA Filesystem Interceptor
 *
 * Copyright(C) 2004-2011 Sophos Limited, Oxford, England.
 *
 * This program is free software; you can redistribute it and/or modify it under the terms of the
 * GNU General Public License Version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program; if not,
 * write to the Free Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/unistd.h>
#include <linux/linkage.h>
#include <linux/fs.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
  #if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,3)
    #include <linux/syscalls.h>
  #endif
  #include <linux/ptrace.h>
#endif
#ifdef TALPA_NEED_MANUAL_RODATA
#include <asm/page.h>
#include <asm/cacheflush.h>
#endif
#ifdef TALPA_HAS_PROBE_KERNEL_WRITE
#include <linux/vmalloc.h>
#endif

#define TALPA_SUBSYS "syscalltable"
#include "common/talpa.h"

#include "platforms/linux/glue.h"
#include "platforms/linux/locking.h"


static asmlinkage long (*orig_mount)(char* dev_name, char* dir_name, char* type, unsigned long flags, void* data);

#if defined(TALPA_HAS_RODATA)
static void *talpa_syscallhook_unro(void *addr, size_t len, int rw);
#endif

#ifdef TALPA_HIDDEN_SYSCALLS
static unsigned long syscall_table = TALPA_SYSCALL_TABLE;
#endif

#ifdef TALPA_NEED_MANUAL_RODATA
  #ifdef TALPA_RODATA_START
static unsigned long rodata_start = TALPA_RODATA_START;
  #else
static unsigned long rodata_start;
  #endif

  #ifdef TALPA_RODATA_END
static unsigned long rodata_end = TALPA_RODATA_END;
  #else
static unsigned long rodata_end;
  #endif

static long rwdata_offset;
#endif


#ifndef prevent_tail_call
# define prevent_tail_call(ret) do { } while (0)
#endif

#ifdef TALPA_HAS_MARK_RODATA_RW
extern void mark_rodata_rw(void);
extern void mark_rodata_ro(void);
#endif

int talpa_syscallhook_modify_start(void)
{
#ifdef TALPA_HAS_RODATA
  #ifdef TALPA_HAS_MARK_RODATA_RW
    mark_rodata_rw();
  #else
    unsigned long rwshadow;

    talpa_lock_kernel();
    rwshadow = (unsigned long)talpa_syscallhook_unro((void *)rodata_start, rodata_end - rodata_start, 1);
    talpa_unlock_kernel();
    if (!rwshadow)
    {
        return 1;
    }
    rwdata_offset = rwshadow - rodata_start;
  #endif
#endif

    return 0;
}

void talpa_syscallhook_modify_finish(void)
{
#ifdef TALPA_HAS_RODATA
  #ifdef TALPA_HAS_MARK_RODATA_RW
    mark_rodata_ro();
  #else
    talpa_lock_kernel();
    talpa_syscallhook_unro((void *)(rodata_start + rwdata_offset), rodata_end - rodata_start, 0);
    talpa_unlock_kernel();
  #endif
#endif
}

void *talpa_syscallhook_poke(void *addr, void *val)
{
    unsigned long target = (unsigned long)addr;

#if defined(TALPA_HAS_RODATA) && defined(TALPA_RODATA_MAP_WRITABLE)
    if (target >= rodata_start && target <= rodata_end)
    {
        target += rwdata_offset;
    }
#endif

    *(void **)target = val;

    return (void *)target;
}

static asmlinkage long talpa_mount(char* dev_name, char* dir_name, char* type, unsigned long flags, void* data)
{
    long err;
    err = orig_mount(dev_name, dir_name, type, flags, data);
    prevent_tail_call(err);
    return err;
}

/*
 * System call table helpers
 */
#ifdef TALPA_HIDDEN_SYSCALLS
static void **sys_call_table;

/* Code below, which finds the hidden system call table,
   is borrowed from the ARLA project and modified.
   It is confirmed to work on 2.6 (x86, x86_64) and
   patched 2.4 (x86) kernels. */

  #if LINUX_VERSION_CODE >= KERNEL_VERSION(3,7,0)
static void *lower_bound = 0;
  #elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
    #include <linux/kallsyms.h>
static void *lower_bound = &kernel_thread;
  #else
    #include <asm/pgtable.h>
static void *lower_bound = &empty_zero_page;
  #endif

const char * __attribute__((weak)) kallsyms_lookup(unsigned long addr, unsigned long *symbolsize, unsigned long *offset, char **modname, char *namebuf);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,39)

static void **get_start_addr(void)
{
    /* This isn't used very much as the address is compiled in by the configure script
     * so we won't deal with it for 2.6.39+ as I can't find a symbol to use
     */
#ifdef TALPA_HIDDEN_SYSCALLS
  #ifndef TALPA_SYSCALL_TABLE
    #error "Syscall table address not built in"
  #endif
#endif
    err("Syscall searching not available for 2.6.39+");
    return (void **)0;
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,16)
static void **get_start_addr(void)
{
    #ifdef CONFIG_SMP
      #if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,33)
    return (void **)&_lock_kernel;
      #else
    return (void **)&lock_kernel;
      #endif
    #else
      #include <linux/mutex.h>
      #ifdef CONFIG_DEBUG_LOCK_ALLOC
    return (void **)&mutex_lock_nested;
      #else
    return (void **)&mutex_lock;
      #endif
    #endif
}
#else /* LINUX_VERSION_CODE < KERNEL_VERSION(2,6,16) */

static void **get_start_addr(void)
{
    #ifdef CONFIG_X86_64
      return (void **)&tasklist_lock - 0x4000;
    #else
      return (void **)&init_mm;
    #endif
}

#endif

static int kallsym_is_equal(unsigned long addr, const char *name)
{
    char namebuf[128];
    const char *retname;
    unsigned long size, offset;
    char *modname;


    retname = kallsyms_lookup(addr, &size, &offset, &modname, namebuf);

    if ( (retname != NULL) && (strcmp(name, retname) == 0) && (offset == 0) )
    {
        return 1;
    }

    return 0;
}

static int verify(void **p, const unsigned int unique_syscalls[], const unsigned int num_unique_syscalls, const unsigned int zapped_syscalls[], const unsigned int num_zapped_syscalls, int symlookup)
{
    unsigned int i, s;


    /* Check that not implemented system calls all point to the same address.
        This is where heuristic usually immediately fails. */
    for ( i = 1; i < num_zapped_syscalls; i++ )
    {
        if ( p[zapped_syscalls[i]] != p[zapped_syscalls[0]] )
        {
                dbg("  [0x%p] not same %u", p, zapped_syscalls[i]);
            return 0;
        }
    }

    /* Check that all different sysmte calls are really different */
    for ( i = 0; i < num_unique_syscalls; i++ )
    {
        for ( s = 0; s < 223; s++ )
        {
            if ( (p[s] == p[unique_syscalls[i]]) && (s != unique_syscalls[i]) )
            {
                dbg("  [0x%p] not unique %u", p, unique_syscalls[i]);
                return 0;
            }
        }
    }

    /* Lookup symbols (if we can) as a final check */
    if (    symlookup
         && kallsyms_lookup
         && !IS_ERR(kallsyms_lookup)
         && (    !kallsym_is_equal((unsigned long)p[__NR_close], "sys_close")
              || !kallsym_is_equal((unsigned long)p[__NR_chdir], "sys_chdir")) )
    {
        dbg("  [0x%p] lookup mismatch", p);
        return 0;
    }

    return 1;
}

static int looks_good(void **p)
{
    if ( (*p <= (void*)lower_bound) || (*p >= (void*)p) )
    {
        return 0;
    }

    return 1;
}

static void **talpa_find_syscall_table(void **ptr, const unsigned int unique_syscalls[], const unsigned int num_unique_syscalls, const unsigned int zapped_syscalls[], const unsigned int num_zapped_syscalls, int symlookup)
{
    void **limit = ptr + 0xa000;
    void **table = NULL;


    for ( ; ptr < limit && table == NULL; ptr++ )
    {
        unsigned int ok = 1;
        unsigned int i;


        for ( i = 0; i < 222; i++ )
        {
            if ( !looks_good(ptr + i) )
            {
                ok = 0;
                ptr = ptr + i;
                break;
            }
        }

        if ( ok && verify(ptr, unique_syscalls, num_unique_syscalls, zapped_syscalls, num_zapped_syscalls, symlookup) )
        {
            table = ptr;
            break;
        }
    }

    return table;
}

#else
extern void *sys_call_table[];
#endif

#if defined(TALPA_HAS_RODATA) && defined(TALPA_RODATA_MAP_WRITABLE)

/*
 * map_writable creates a shadow page mapping of the range
 * [addr, addr + len) so that we can write to code mapped read-only.
 *
 * It is similar to a generalized version of x86's text_poke.  But
 * because one cannot use vmalloc/vfree() inside stop_machine, we use
 * map_writable to map the pages before stop_machine, then use the
 * mapping inside stop_machine, and unmap the pages afterwards.
 */
static void *map_writable(void *addr, size_t len)
{
        void *vaddr;
        int nr_pages = DIV_ROUND_UP(offset_in_page(addr) + len, PAGE_SIZE);
        struct page **pages = kmalloc(nr_pages * sizeof(*pages), GFP_KERNEL);
        void *page_addr = (void *)((unsigned long)addr & PAGE_MASK);
        int i;

        if (pages == NULL)
                return NULL;

        for (i = 0; i < nr_pages; i++) {
                if (__module_address((unsigned long)page_addr) == NULL) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,22) || !defined(CONFIG_X86_64)
                        pages[i] = virt_to_page(page_addr);
#else /* LINUX_VERSION_CODE < && CONFIG_X86_64 */
/* e3ebadd95cb621e2c7436f3d3646447ac9d5c16d was after 2.6.21
 * This works around a broken virt_to_page() from the RHEL 5 backport
 * of x86-64 relocatable kernel support.
 */
                        pages[i] =
                            pfn_to_page(__pa_symbol(page_addr) >> PAGE_SHIFT);
#endif /* LINUX_VERSION_CODE || !CONFIG_X86_64 */
                        WARN_ON(!PageReserved(pages[i]));
                } else {
                        pages[i] = vmalloc_to_page(addr);
                }
                if (pages[i] == NULL) {
                        kfree(pages);
                        return NULL;
                }
                page_addr += PAGE_SIZE;
        }
        vaddr = vmap(pages, nr_pages, VM_MAP, PAGE_KERNEL);
        kfree(pages);
        if (vaddr == NULL)
                return NULL;
        return vaddr + offset_in_page(addr);
}

static void *talpa_syscallhook_unro(void *addr, size_t len, int rw)
{
    if ( rw )
    {
        return map_writable(addr, len);
    }
    else
    {
        vunmap((void *)((unsigned long)addr & PAGE_MASK));
        return NULL;
    }
}

#elif defined(TALPA_NEED_MANUAL_RODATA)

#  ifdef TALPA_NEEDS_VA_CPA

#    ifdef TALPA_NO_PA_SYMBOL
static unsigned long *talpa_phys_base = (unsigned long *)TALPA_PHYS_BASE;

#      define talpa_pa_symbol(x) \
        ({unsigned long v;  \
          asm("" : "=r" (v) : "0" (x)); \
          ((v - __START_KERNEL_map) + (*talpa_phys_base)); })
#    else
#      define talpa_pa_symbol __pa_symbol
#    endif

#    define talpa_ka_to_cpa(adr) ((unsigned long)__va(talpa_pa_symbol(adr)))

#  else /* NEEDS_VA_CPA */
#    define talpa_ka_to_cpa(adr) ((unsigned long)adr)
#  endif /* NEEDS_VA_CPA */

static void *talpa_syscallhook_unro(void *addr, size_t len, int rw)
{
    unsigned long nr_pages = len / PAGE_SIZE;


  #ifdef TALPA_HAS_SET_PAGES
    #ifdef CONFIG_X86_64
    typedef int (*kfunc)(unsigned long addr, int numpages);
    kfunc set_memory_rwro;


    if ( rw )
    {
        set_memory_rwro  = (kfunc)TALPA_KFUNC_SET_MEMORY_RW;
    }
    else
    {
        set_memory_rwro  = (kfunc)TALPA_KFUNC_SET_MEMORY_RO;
    }

    set_memory_rwro(addr, nr_pages);
    #elif CONFIG_X86
    typedef int (*kfunc)(struct page *page, int numpages);
    kfunc set_pages_rwro;


    if ( rw )
    {
        set_pages_rwro  = (kfunc)TALPA_KFUNC_SET_PAGES_RW;
    }
    else
    {
        set_pages_rwro  = (kfunc)TALPA_KFUNC_SET_PAGES_RO;
    }

    set_pages_rwro(virt_to_page(addr), nr_pages);
    #endif
  #else /* HAS_SET_PAGES */
    #ifdef CONFIG_X86_64
    typedef int (*kfunc)(unsigned long addr, int numpages, pgprot_t prot);
    static kfunc talpa_change_page_attr_addr = (kfunc)TALPA_KFUNC_CHANGE_PAGE_ATTR_ADDR;

    if ( rw )
    {
        talpa_change_page_attr_addr(talpa_ka_to_cpa(addr), nr_pages, PAGE_KERNEL);
    }
    else
    {
        talpa_change_page_attr_addr(talpa_ka_to_cpa(addr), nr_pages, PAGE_KERNEL_RO);
    }
    #elif CONFIG_X86
    if ( rw )
    {
        change_page_attr(virt_to_page(addr), nr_pages, PAGE_KERNEL);
    }
    else
    {
        change_page_attr(virt_to_page(addr), nr_pages, PAGE_KERNEL_RO);
    }
    #endif

    global_flush_tlb();
  #endif

    return addr;
}
#endif /* HAS_RODATA */

#ifdef TALPA_HIDDEN_SYSCALLS
static void **look_around(void **p, const unsigned int unique_syscalls[], const unsigned int num_unique_syscalls, const unsigned int zapped_syscalls[], const unsigned int num_zapped_syscalls, int symlookup)
{
    unsigned char* orig_addr = (unsigned char *)p;
    unsigned char* start_addr = orig_addr - sizeof(void *) * 2;
    unsigned char* end_addr = orig_addr + sizeof(void *) * 2;


    for ( ;start_addr < end_addr; start_addr++ )
    {
        unsigned int ok = 1;
        unsigned int i;


        for ( i = 0; i < 222; i++ )
        {
            if ( !looks_good((void *)start_addr + i) )
            {
                ok = 0;
                break;
            }
        }

        if ( ok && verify((void **)start_addr, unique_syscalls, num_unique_syscalls, zapped_syscalls, num_zapped_syscalls, symlookup) )
        {
            info("At offset %ld", start_addr - orig_addr);
            return (void **)start_addr;
        }
    }

    return NULL;
}

static void **find_around(void **p, const unsigned int unique_syscalls[], const unsigned int num_unique_syscalls, const unsigned int zapped_syscalls[], const unsigned int num_zapped_syscalls, int symlookup)
{
    unsigned char* orig_addr = (unsigned char *)p;
    unsigned char* start_addr = orig_addr - sizeof(void *) * 2;
    unsigned char* end_addr = orig_addr + sizeof(void *) * 2;
    void **res;


    for ( ;start_addr < end_addr; start_addr++ )
    {
        res = talpa_find_syscall_table((void **)start_addr, unique_syscalls, num_unique_syscalls, zapped_syscalls, num_zapped_syscalls, symlookup);
        if ( res )
        {
            info("Found with offset %ld", start_addr - orig_addr);
            return res;
        }
    }

    return NULL;
}

static int find_syscall_table(void)
{
    unsigned int num_unique_syscalls;
    unsigned int num_zapped_syscalls;


  #ifdef CONFIG_X86_64
    const unsigned int unique_syscalls[] = { __NR_read, __NR_dup, __NR_open, __NR_close, __NR_mmap, __NR_exit, __NR_kill };
    const unsigned int zapped_syscalls[] = { __NR_create_module, __NR_get_kernel_syms, __NR_security, __NR_get_thread_area, __NR_epoll_wait_old, __NR_vserver, 0 };
  #elif CONFIG_X86
    const unsigned int unique_syscalls[] = { __NR_exit, __NR_mount, __NR_read, __NR_write, __NR_open, __NR_close, __NR_unlink };
    const unsigned int zapped_syscalls[] = { __NR_break, __NR_stty, __NR_gtty, __NR_ftime, __NR_prof, __NR_lock, __NR_ulimit, 0 };
  #endif

    num_unique_syscalls = sizeof(unique_syscalls)/sizeof(unique_syscalls[0]);
    num_zapped_syscalls = (sizeof(zapped_syscalls)/sizeof(zapped_syscalls[0])) - 1;

    if ( syscall_table )
    {
        sys_call_table = (void **)syscall_table;

        if ( verify(sys_call_table, unique_syscalls, num_unique_syscalls, zapped_syscalls, num_zapped_syscalls, 1) )
        {
            dbg("userspace specified sys_call_table at 0x%p", sys_call_table);
        }
        else
        {
            dbg("not a sys_call_table at 0x%p", sys_call_table);
            /* Look around specified address before giving up. */
            sys_call_table = look_around(sys_call_table, unique_syscalls, num_unique_syscalls, zapped_syscalls, num_zapped_syscalls, 1);
            if ( !sys_call_table )
            {
                dbg("no sys_call_table around 0x%lx", syscall_table);
            }
            syscall_table = (unsigned long)sys_call_table;
        }
    }

    /* If valid address wasn't supplied to us we'll try to autodetect it */
    if ( !syscall_table )
    {
        sys_call_table = talpa_find_syscall_table(get_start_addr(), unique_syscalls, num_unique_syscalls, zapped_syscalls, num_zapped_syscalls, 1);
        if ( !sys_call_table )
        {
            dbg("no sys_call_table found");
            /* Look around specified address before giving up. */
            sys_call_table = find_around(get_start_addr(), unique_syscalls, num_unique_syscalls, zapped_syscalls, num_zapped_syscalls, 1);
            if ( !sys_call_table )
            {
                dbg("no sys_call_table found");
            }
        }
        syscall_table = (unsigned long)sys_call_table;
    }

    if ( sys_call_table == NULL )
    {
        err("Cannot find syscall table!");
        return -ESRCH;
    }

    dbg("Syscall table at 0x%p", sys_call_table);
    return 0;
}
#else
static int find_syscall_table(void)
{
    return 0;
}
#endif

static void save_originals(void)
{
#ifdef CONFIG_X86
    orig_mount = sys_call_table[__NR_mount];
#else
  #error "Architecture currently not supported!"
#endif
}

#define patch_syscall(base, num, new) talpa_syscallhook_poke(&base[num], new)

static void patch_table(void)
{
    patch_syscall(sys_call_table, __NR_mount, talpa_mount);
}

static unsigned int check_table(void)
{
    if ( sys_call_table[__NR_mount] != talpa_mount )
    {
        warn("mount() is patched by someone else!");
        return 1;
    }

    return 0;
}

static void restore_table(void)
{
#if defined CONFIG_X86
    patch_syscall(sys_call_table, __NR_mount, orig_mount);
#endif
}

/*
 * Module init and exit
 */

static int __init talpa_syscallhook_init(void)
{
    int ret;


#ifdef TALPA_HIDDEN_SYSCALLS
    lower_bound = (void*)((unsigned long)lower_bound & ~0xfffff);
    dbg("lower bound 0x%p", lower_bound);

    /* Relocate addresses (if needed) embedded at compile time. */
    syscall_table = (unsigned long)talpa_get_symbol("sys_call_table", (void *)syscall_table);
#endif

#ifdef TALPA_NEED_MANUAL_RODATA
   rodata_start = (unsigned long)talpa_get_symbol("__start_rodata", (void *)rodata_start);
   rodata_end = (unsigned long)talpa_get_symbol("__end_rodata", (void *)rodata_end);
#endif

ret = find_syscall_table();
    if ( ret < 0 )
    {
        return ret;
    }

    talpa_lock_kernel();
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
    fsync_dev(0);
#endif
    save_originals();
    ret = talpa_syscallhook_modify_start();
    if (ret)
    {
        talpa_unlock_kernel();
        err("Failed to unprotect read-only memory!");
        return ret;
    }
    patch_table();
    talpa_syscallhook_modify_finish();
    talpa_unlock_kernel();

    dbg("Hooked");

    return 0;
}

static void __exit talpa_syscallhook_exit(void)
{
    int ret;


    talpa_lock_kernel();
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
    fsync_dev(0);
#endif

    /* We have to cater for the possibility that someone else
       has modified the syscall table after us. Therefore we
       will check for that and sleep until the situation resolves.
       There isn't really a perfect solution in these sorts of
       unsupported oprations so it is better to hang here than
       to cause system instability in any way.
       The best thing would be to leave this module loaded forever.
    */
    while ( check_table() != 0 )
    {
        talpa_unlock_kernel();
        __set_current_state(TASK_UNINTERRUPTIBLE);
        schedule_timeout(HZ);
        talpa_lock_kernel();
    }
    do
    {
        ret = talpa_syscallhook_modify_start();
        if (ret)
        {
            talpa_unlock_kernel();
            err("Failing to unprotect read-only memory!");
            __set_current_state(TASK_UNINTERRUPTIBLE);
            schedule_timeout(HZ);
            talpa_lock_kernel();
        }
    } while (ret); /* Unfortunate but we can't possibly exit if we failed to restore original pointers. */
    restore_table();
    talpa_syscallhook_modify_finish();
    talpa_unlock_kernel();

    dbg("Unhooked");
}

module_init(talpa_syscallhook_init);
module_exit(talpa_syscallhook_exit);

MODULE_DESCRIPTION("TALPA Filesystem Interceptor Test Module");
MODULE_AUTHOR("Sophos Limited");
MODULE_LICENSE("GPL");

/*
 * End of talpa_syscalltable.c
 */
