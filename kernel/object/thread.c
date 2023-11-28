/*
 * Copyright (c) 2023 Institute of Parallel And Distributed Systems (IPADS),
 * Shanghai Jiao Tong University (SJTU) Licensed under the Mulan PSL v2. You can
 * use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY
 * KIND, EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 * NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE. See the
 * Mulan PSL v2 for more details.
 */

#include <common/kprint.h>
#include <common/macro.h>
#include <common/types.h>
#include <common/util.h>
#include <mm/kmalloc.h>
#include <mm/mm.h>
#include <mm/uaccess.h>
#include <object/thread.h>
#include <object/recycle.h>
#include <sched/context.h>
#include <arch/machine/registers.h>
#include <arch/machine/smp.h>
#include <arch/mmu.h>
#include <arch/time.h>
#include <irq/ipi.h>
#include <common/endianness.h>
#include <ipc/futex.h>

#include "thread_env.h"

void test_root_thread_basic(const struct cap_group *ptr)
{
        BUG_ON(ptr == NULL);
        BUG_ON(container_of(ptr, struct object, opaque)->type
               != TYPE_CAP_GROUP);
        kinfo("Cap_create Pretest Ok!\n");
}

void test_root_thread_after_create(const struct cap_group *ptr,
                                   const int thread_cap)
{
        BUG_ON(ptr->thread_cnt == 0);
        BUG_ON(thread_cap == 0);
        kinfo("Thread_create Ok!\n");

}

/*
 * local functions
 */
#ifdef CHCORE
static int thread_init(struct thread *thread, struct cap_group *cap_group,
#else /* For unit test */
int thread_init(struct thread *thread, struct cap_group *cap_group,
#endif
                       vaddr_t stack, vaddr_t pc, u32 prio, u32 type, s32 aff)
{
        thread->cap_group =
                obj_get(cap_group, CAP_GROUP_OBJ_ID, TYPE_CAP_GROUP);
        thread->vmspace = obj_get(cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE);
        obj_put(thread->cap_group);
        obj_put(thread->vmspace);

        /* Thread context is used as the kernel stack for that thread */
        thread->thread_ctx = create_thread_ctx(type);
        if (!thread->thread_ctx)
                return -ENOMEM;
        init_thread_ctx(thread, stack, pc, prio, type, aff);

        /*
         * Field prev_thread records the previous thread runs
         * just before this thread. Obviously, it is NULL at the beginning.
         */
        thread->prev_thread = NULL;

        /* The ipc_config will be allocated on demand */
        thread->general_ipc_config = NULL;

        thread->sleep_state.cb = NULL;

        lock_init(&thread->sleep_state.queue_lock);

        return 0;
}

void thread_deinit(void *thread_ptr)
{
        struct thread *thread;
        struct cap_group *cap_group;

        thread = (struct thread *)thread_ptr;

        BUG_ON(thread->thread_ctx->thread_exit_state != TE_EXITED);
        if (thread->thread_ctx->state != TS_EXIT)
                kwarn("thread ctx->state is %d\n", thread->thread_ctx->state);

        cap_group = thread->cap_group;
        lock(&cap_group->threads_lock);
        list_del(&thread->node);
        unlock(&cap_group->threads_lock);

        if (thread->general_ipc_config)
                kfree(thread->general_ipc_config);

        destroy_thread_ctx(thread);

        /* The thread struct itself will be freed in __free_object */
}

#define PFLAGS2VMRFLAGS(PF)                                       \
        (((PF)&PF_X ? VMR_EXEC : 0) | ((PF)&PF_W ? VMR_WRITE : 0) \
         | ((PF)&PF_R ? VMR_READ : 0))

#define OFFSET_MASK (0xFFF)

/* Required by LibC */
void prepare_env(char *env, vaddr_t top_vaddr, char *name,
                 struct process_metadata *meta);

/*
 * exported functions
 */
void switch_thread_vmspace_to(struct thread *thread)
{
        switch_vmspace_to(thread->vmspace);
}

/* Arguments for the inital thread */
#if __SIZEOF_POINTER__ == 4
#define ROOT_THREAD_STACK_BASE (0x50000000UL)
#define ROOT_THREAD_STACK_SIZE (0x200000UL)
#else
#define ROOT_THREAD_STACK_BASE (0x500000000000UL)
#define ROOT_THREAD_STACK_SIZE (0x800000UL)
#endif
#define ROOT_THREAD_PRIO DEFAULT_PRIO

#define ROOT_THREAD_VADDR 0x400000

char ROOT_NAME[] = "/procmgr.srv";

/*
 * The root_thread is actually a first user thread
 * which has no difference with other user threads
 */
void create_root_thread(void)
{
        struct cap_group *root_cap_group;
        cap_t thread_cap;
        struct thread *root_thread;
        char data[8];
        int ret;
        cap_t stack_pmo_cap;
        struct thread *thread;
        struct pmobject *stack_pmo;
        struct vmspace *init_vmspace;
        vaddr_t stack;
        vaddr_t kva;
        struct process_metadata meta;

        /*
         * Read from binary.
         * The msg and the binary of of the init process(procmgr) are linked
         * behind the kernel image via the incbin instruction.
         * The binary_procmgr_bin_start points to the first piece of info:
         * the entry point of the init process, followed by eight bytes of data
         * that stores the mem_size of the binary.
         */

        memcpy(data,
               (void *)((unsigned long)&binary_procmgr_bin_start
                        + ROOT_ENTRY_OFF),
               sizeof(data));
        meta.entry = (unsigned long)be64_to_cpu(*(u64 *)data);

        memcpy(data,
               (void *)((unsigned long)&binary_procmgr_bin_start
                        + ROOT_FLAGS_OFF),
               sizeof(data));
        meta.flags = (unsigned long)be64_to_cpu(*(u64 *)data);

        memcpy(data,
               (void *)((unsigned long)&binary_procmgr_bin_start
                        + ROOT_PHENT_SIZE_OFF),
               sizeof(data));
        meta.phentsize = (unsigned long)be64_to_cpu(*(u64 *)data);

        memcpy(data,
               (void *)((unsigned long)&binary_procmgr_bin_start
                        + ROOT_PHNUM_OFF),
               sizeof(data));
        meta.phnum = (unsigned long)be64_to_cpu(*(u64 *)data);

        memcpy(data,
               (void *)((unsigned long)&binary_procmgr_bin_start
                        + ROOT_PHDR_ADDR_OFF),
               sizeof(data));
        meta.phdr_addr = (unsigned long)be64_to_cpu(*(u64 *)data);


        root_cap_group = create_root_cap_group(ROOT_NAME, strlen(ROOT_NAME));
        test_root_thread_basic(root_cap_group);
        
        init_vmspace = obj_get(root_cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE);

        /* Allocate and setup a user stack for the init thread */
        stack_pmo_cap = create_pmo(ROOT_THREAD_STACK_SIZE,
                                   PMO_ANONYM,
                                   root_cap_group,
                                   0,
                                   &stack_pmo);
        BUG_ON(stack_pmo_cap < 0);


        ret = vmspace_map_range(init_vmspace,
                                ROOT_THREAD_STACK_BASE,
                                ROOT_THREAD_STACK_SIZE,
                                VMR_READ | VMR_WRITE,
                                stack_pmo);
        BUG_ON(ret != 0);

        /* Allocate the init thread */
        thread = obj_alloc(TYPE_THREAD, sizeof(*thread));
        BUG_ON(thread == NULL);

        for (int i = 0; i < meta.phnum; i++) {
                unsigned int flags;
                unsigned long offset, vaddr, filesz, memsz;

                memcpy(data,
                       (void *)((unsigned long)&binary_procmgr_bin_start
                                + ROOT_PHDR_OFF + i * ROOT_PHENT_SIZE
                                + PHDR_FLAGS_OFF),
                       sizeof(data));
                flags = (unsigned int)le32_to_cpu(*(u32 *)data);
        printk("begin lab3 task2 \n");

                /* LAB 3 TODO BEGIN */
                /* Get offset, vaddr, filesz, memsz from image*/
        /* Get offset from image */
        memcpy(data,
               (void *)((unsigned long)&binary_procmgr_bin_start
                        + ROOT_PHDR_OFF + i * ROOT_PHENT_SIZE
                        + PHDR_OFFSET_OFF),
               sizeof(unsigned long));
        offset = le64_to_cpu(*(u64 *)data);
        printk("offset: %lx\n", offset);


        /* Get vaddr from image */
        memcpy(data,
               (void *)((unsigned long)&binary_procmgr_bin_start
                        + ROOT_PHDR_OFF + i * ROOT_PHENT_SIZE
                        + PHDR_VADDR_OFF),
               sizeof(unsigned long));
        vaddr = le64_to_cpu(*(u64 *)data);
        printk("vaddr: %lx\n", vaddr);


        /* Get filesz from image */
        memcpy(data,
               (void *)((unsigned long)&binary_procmgr_bin_start
                        + ROOT_PHDR_OFF + i * ROOT_PHENT_SIZE
                        + PHDR_FILESZ_OFF),
               sizeof(unsigned long));
        filesz = le64_to_cpu(*(u64 *)data);
        printk("filesz: %lx\n", filesz);

        /* Get memsz from image */
        memcpy(data,
               (void *)((unsigned long)&binary_procmgr_bin_start
                        + ROOT_PHDR_OFF + i * ROOT_PHENT_SIZE
                        + PHDR_MEMSZ_OFF),
               sizeof(unsigned long));
        memsz = le64_to_cpu(*(u64 *)data);
        printk("memsz: %lx\n", memsz);



                /* LAB 3 TODO END */

                struct pmobject *segment_pmo;
                /* LAB 3 TODO BEGIN */
        cap_t segment_pmo_cap = create_pmo(ROUND_UP(memsz, PAGE_SIZE), PMO_DATA, root_cap_group, 0, &segment_pmo);



        BUG_ON(segment_pmo_cap < 0);
        printk("segment_pmo_cap: %d\n", segment_pmo_cap);

                /* LAB 3 TODO END */

                BUG_ON(ret < 0);

                /* LAB 3 TODO BEGIN */
                /* Copy elf file contents into memory*/

        /* Copy the file content of the ELF segment to the allocated memory */



//     vaddr_t vaddr_start = ROUND_DOWN(vaddr, PAGE_SIZE);
//     vaddr_t vaddr_end = ROUND_UP(vaddr + memsz, PAGE_SIZE);
//     size_t seg_map_sz = vaddr_end - vaddr_start;
//     char *pmo_start = (char *)phys_to_virt(segment_pmo->start) + (vaddr - vaddr_start);
//     char *seg_start = (char *)((unsigned long)&binary_procmgr_bin_start + offset);
//     for (size_t idx = 0; idx < filesz; idx++) {
//         pmo_start[idx] = seg_start[idx];
//     }








        printk("start copy to vaddr: %lx\n", phys_to_virt(segment_pmo->start));
        memset((void *)( phys_to_virt(segment_pmo->start)), 0, segment_pmo->size);
        memcpy((void *)( phys_to_virt(segment_pmo->start)),  /* Destination in virtual memory */
               (void *)((unsigned long)&binary_procmgr_bin_start + ROOT_BIN_HDR_SIZE +
               offset), /* Source in the binary image */
               filesz); /* Number of bytes to copy */
        // printk("filesz copy to vaddr: %lx\n", vaddr);










                /* LAB 3 TODO END */
                
                unsigned vmr_flags = 0;    
                /* LAB 3 TODO BEGIN */
                /* Set flags*/
        // if (flags & PHDR_FLAGS_R) vmr_flags |= VMR_READ;
        // if (flags & PHDR_FLAGS_W) vmr_flags |= VMR_WRITE;
        // if (flags & PHDR_FLAGS_X) vmr_flags |= VMR_EXEC;
        // #define PF_X = PHDR_FLAGS_X
        // #define PF_W = PHDR_FLAGS_W
        // #define PF_R = PHDR_FLAGS_R

        // vmr_flags = (((flags)&= (1 << 0) ? (1 << 2) : 0) | ((flags)&= (1 << 1) ? (1 << 1) : 0) | ((flags)&= (1 << 2) ? (1 << 0) : 0));
        if (flags & PHDR_FLAGS_R) vmr_flags |= VMR_READ;
        if (flags & PHDR_FLAGS_W) vmr_flags |= VMR_WRITE;
        if (flags & PHDR_FLAGS_X) vmr_flags |= VMR_EXEC;


        printk("vmr_flags: %x\n", vmr_flags);

                /* LAB 3 TODO END */

                ret = vmspace_map_range(init_vmspace,
                                        vaddr,
                                        segment_pmo->size,
                                        vmr_flags,
                                        segment_pmo);
                BUG_ON(ret < 0);

        }
        obj_put(init_vmspace);

        stack = ROOT_THREAD_STACK_BASE + ROOT_THREAD_STACK_SIZE;

        /* Allocate a physical page for the main stack for prepare_env */
        kva = (vaddr_t)get_pages(0);
        BUG_ON(kva == 0);
        commit_page_to_pmo(stack_pmo,
                           ROOT_THREAD_STACK_SIZE / PAGE_SIZE - 1,
                           virt_to_phys((void *)kva));

        prepare_env((char *)kva, stack, ROOT_NAME, &meta);
        stack -= ENV_SIZE_ON_STACK;

        ret = thread_init(thread,
                          root_cap_group,
                          stack,
                          meta.entry,
                          ROOT_THREAD_PRIO,
                          TYPE_USER,
                          smp_get_cpu_id());
        BUG_ON(ret != 0);

        /* Add the thread into the thread_list of the cap_group */
        lock(&root_cap_group->threads_lock);
        list_add(&thread->node, &root_cap_group->thread_list);
        root_cap_group->thread_cnt += 1;
        unlock(&root_cap_group->threads_lock);

        /* Allocate the cap for the init thread */
        thread_cap = cap_alloc(root_cap_group, thread);
        BUG_ON(thread_cap < 0);
        test_root_thread_after_create(root_cap_group, thread_cap);

        /* L1 icache & dcache have no coherence on aarch64 */
        flush_idcache();

        root_thread = obj_get(root_cap_group, thread_cap, TYPE_THREAD);
        /* Enqueue: put init thread into the ready queue */
        BUG_ON(sched_enqueue(root_thread));
        obj_put(root_thread);
}

static cap_t create_thread(struct cap_group *cap_group, vaddr_t stack,
                           vaddr_t pc, unsigned long arg, u32 prio, u32 type,
                           u64 tls, int *clear_child_tid)
{
        struct thread *thread;
        cap_t cap, ret = 0;
        bool startup_suspend = false;

        thread = obj_alloc(TYPE_THREAD, sizeof(*thread));
        if (!thread) {
                ret = -ENOMEM;
                goto out_fail;
        }
        if (type == TYPE_TRACEE) {
                startup_suspend = true;
                type = TYPE_USER;
        }
        ret = thread_init(thread, cap_group, stack, pc, prio, type, NO_AFF);
        if (ret != 0)
                goto out_free_obj;

        lock(&cap_group->threads_lock);

        /*
         * Check the exiting state: do not create new threads if exiting (e.g.,
         * after sys_exit_group is executed.
         */
        if (current_thread->thread_ctx->thread_exit_state == TE_EXITING) {
                unlock(&cap_group->threads_lock);
                obj_free(thread);
                obj_put(cap_group);
                sched();
                eret_to_thread(switch_context());
                /* No return */
        }

        list_add(&thread->node, &cap_group->thread_list);
        cap_group->thread_cnt += 1;
        unlock(&cap_group->threads_lock);

        arch_set_thread_arg0(thread, arg);

        /* set thread tls */
        arch_set_thread_tls(thread, tls);

        /* set arch-specific thread state */
        set_thread_arch_spec_state(thread);

        /* cap is thread_cap in the target cap_group */
        cap = cap_alloc(cap_group, thread);
        if (cap < 0) {
                ret = cap;
                goto out_free_obj;
        }
        thread->cap = cap;

        thread->clear_child_tid = clear_child_tid;

        /* ret is thread_cap in the current_cap_group */
        if (cap_group != current_cap_group)
                cap = cap_copy(cap_group, current_cap_group, cap);
        if (type == TYPE_USER) {
                thread->thread_ctx->state = TS_INTER;
                if (startup_suspend) {
                        thread->thread_ctx->is_suspended = true;
                }
                BUG_ON(sched_enqueue(thread));
        } else if ((type == TYPE_SHADOW) || (type == TYPE_REGISTER)) {
                thread->thread_ctx->state = TS_WAITING;
        }
        return cap;

out_free_obj:
        obj_free(thread);
out_fail:
        return ret;
}

struct thread_args {
        /* specify the cap_group in which the new thread will be created */
        cap_t cap_group_cap;
        vaddr_t stack;
        vaddr_t pc;
        unsigned long arg;
        vaddr_t tls;
        unsigned int prio;
        unsigned int type;
        int *clear_child_tid;
};

/* Create one thread in a specified cap_group and return the thread cap in it.
 */
cap_t sys_create_thread(unsigned long thread_args_p)
{
        struct thread_args args = {0};
        struct cap_group *cap_group;
        cap_t thread_cap;
        u32 type;

        if (check_user_addr_range(thread_args_p, sizeof(args)) != 0)
                return -EINVAL;

        int r = copy_from_user(&args, (void *)thread_args_p, sizeof(args));
        if (r) {
                return -EINVAL;
        }
        type = args.type;

        if ((type != TYPE_USER) && (type != TYPE_SHADOW)
            && (type != TYPE_REGISTER) && (type != TYPE_TRACEE))
                return -EINVAL;

        if ((args.prio > MAX_PRIO) || (args.prio < MIN_PRIO))
                return -EINVAL;

        cap_group =
                obj_get(current_cap_group, args.cap_group_cap, TYPE_CAP_GROUP);
        if (cap_group == NULL)
                return -ECAPBILITY;

        thread_cap = create_thread(cap_group,
                                   args.stack,
                                   args.pc,
                                   args.arg,
                                   args.prio,
                                   type,
                                   args.tls,
                                   args.clear_child_tid);

        obj_put(cap_group);
        return thread_cap;
}

/* Exit the current running thread */
void sys_thread_exit(void)
{
        int cnt;

        /* As a normal application, the main thread will eventually invoke
         * sys_exit_group or trigger unrecoverable fault (e.g., segfault).
         *
         * However a malicious application, all of its thread may invoke
         * sys_thread_exit. So, we monitor the number of non-shadow threads
         * in a cap_group (as a user process now).
         */

        kdebug("%s is invoked\n", __func__);

        /* Set thread state, which will be recycle afterwards */
        current_thread->thread_ctx->thread_exit_state = TE_EXITING;

        lock(&(current_cap_group->threads_lock));
        cnt = --current_cap_group->thread_cnt;
        unlock(&(current_cap_group->threads_lock));

        if (cnt == 0) {
                /*
                 * Current thread is the last thread in this cap_group,
                 * so we invoke sys_exit_group.
                 */
                kdebug("%s invokes sys_exit_group\n", __func__);
                sys_exit_group(0);
                /* The control flow will not go through */
        }

        if (current_thread->clear_child_tid) {
                int val = 0;
                copy_to_user(current_thread->clear_child_tid, &val, sizeof(int));
                sys_futex_wake(current_thread->clear_child_tid, 0, 1);
        }

        kdebug("%s invokes sched\n", __func__);
        /* Reschedule */
        sched();
        eret_to_thread(switch_context());
}

int sys_set_affinity(cap_t thread_cap, int aff)
{
        struct thread *thread;

        if (aff >= PLAT_CPU_NUM)
                return -EINVAL;

        if (thread_cap == 0)
                /* 0 represents current thread */
                thread = current_thread;
        else
                thread = obj_get(current_cap_group, thread_cap, TYPE_THREAD);

        if (thread == NULL)
                return -ECAPBILITY;

        thread->thread_ctx->affinity = aff;

        if (thread_cap != 0)
                obj_put(thread);

        return 0;
}

int sys_get_affinity(cap_t thread_cap)
{
        struct thread *thread;
        int aff;

        if (thread_cap == 0)
                /* 0 represents current thread */
                thread = current_thread;
        else
                thread = obj_get(current_cap_group, thread_cap, TYPE_THREAD);

        if (thread == NULL)
                return -ECAPBILITY;

        aff = thread->thread_ctx->affinity;

        if (thread_cap != 0)
                obj_put(thread);

        return aff;
}

int sys_set_prio(cap_t thread_cap, int prio)
{
        /* Only support the thread itself */
        if (thread_cap != 0)
                return -EINVAL;
        /* Need to limit setting arbitrary priority */
        if (prio <= 0 || prio > MAX_PRIO)
                return -EINVAL;

        current_thread->thread_ctx->sc->prio = prio;

        return 0;
}

int sys_get_prio(cap_t thread_cap)
{
        /* Only support the thread itself */
        if (thread_cap != 0)
                return -EINVAL;

        return current_thread->thread_ctx->sc->prio;
}

int sys_set_tid_address(int *tidptr)
{
        current_thread->clear_child_tid = tidptr;
        return 0;
}
