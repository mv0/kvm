/*
 * handling kvm guest interrupts
 *
 * Copyright IBM Corp. 2008
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License (version 2 only)
 * as published by the Free Software Foundation.
 *
 *    Author(s): Carsten Otte <cotte@de.ibm.com>
 */

#include <linux/interrupt.h>
#include <linux/kvm_host.h>
#include <linux/hrtimer.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <asm/asm-offsets.h>
#include <asm/uaccess.h>
#include "kvm-s390.h"
#include "gaccess.h"
#include "trace-s390.h"

#define IOINT_SCHID_MASK 0x0000ffff
#define IOINT_SSID_MASK 0x00030000
#define IOINT_CSSID_MASK 0x03fc0000
#define IOINT_AI_MASK 0x04000000

static int is_ioint(u64 type)
{
	return ((type & 0xfffe0000u) != 0xfffe0000u);
}

int psw_extint_disabled(struct kvm_vcpu *vcpu)
{
	return !(vcpu->arch.sie_block->gpsw.mask & PSW_MASK_EXT);
}

static int psw_ioint_disabled(struct kvm_vcpu *vcpu)
{
	return !(vcpu->arch.sie_block->gpsw.mask & PSW_MASK_IO);
}

static int psw_mchk_disabled(struct kvm_vcpu *vcpu)
{
	return !(vcpu->arch.sie_block->gpsw.mask & PSW_MASK_MCHECK);
}

static int psw_interrupts_disabled(struct kvm_vcpu *vcpu)
{
	if ((vcpu->arch.sie_block->gpsw.mask & PSW_MASK_PER) ||
	    (vcpu->arch.sie_block->gpsw.mask & PSW_MASK_IO) ||
	    (vcpu->arch.sie_block->gpsw.mask & PSW_MASK_EXT))
		return 0;
	return 1;
}

static u64 int_word_to_isc_bits(u32 int_word)
{
	u8 isc = (int_word & 0x38000000) >> 27;

	return (0x80 >> isc) << 24;
}

static int __interrupt_is_deliverable(struct kvm_vcpu *vcpu,
				      struct kvm_s390_interrupt_info *inti)
{
	switch (inti->type) {
	case KVM_S390_INT_EXTERNAL_CALL:
		if (psw_extint_disabled(vcpu))
			return 0;
		if (vcpu->arch.sie_block->gcr[0] & 0x2000ul)
			return 1;
	case KVM_S390_INT_EMERGENCY:
		if (psw_extint_disabled(vcpu))
			return 0;
		if (vcpu->arch.sie_block->gcr[0] & 0x4000ul)
			return 1;
		return 0;
	case KVM_S390_INT_SERVICE:
	case KVM_S390_INT_PFAULT_INIT:
	case KVM_S390_INT_PFAULT_DONE:
	case KVM_S390_INT_VIRTIO:
		if (psw_extint_disabled(vcpu))
			return 0;
		if (vcpu->arch.sie_block->gcr[0] & 0x200ul)
			return 1;
		return 0;
	case KVM_S390_PROGRAM_INT:
	case KVM_S390_SIGP_STOP:
	case KVM_S390_SIGP_SET_PREFIX:
	case KVM_S390_RESTART:
		return 1;
	case KVM_S390_MCHK:
		if (psw_mchk_disabled(vcpu))
			return 0;
		if (vcpu->arch.sie_block->gcr[14] & inti->mchk.cr14)
			return 1;
		return 0;
	case KVM_S390_INT_IO_MIN...KVM_S390_INT_IO_MAX:
		if (psw_ioint_disabled(vcpu))
			return 0;
		if (vcpu->arch.sie_block->gcr[6] &
		    int_word_to_isc_bits(inti->io.io_int_word))
			return 1;
		return 0;
	default:
		printk(KERN_WARNING "illegal interrupt type %llx\n",
		       inti->type);
		BUG();
	}
	return 0;
}

static void __set_cpu_idle(struct kvm_vcpu *vcpu)
{
	BUG_ON(vcpu->vcpu_id > KVM_MAX_VCPUS - 1);
	atomic_set_mask(CPUSTAT_WAIT, &vcpu->arch.sie_block->cpuflags);
	set_bit(vcpu->vcpu_id, vcpu->arch.local_int.float_int->idle_mask);
}

static void __unset_cpu_idle(struct kvm_vcpu *vcpu)
{
	BUG_ON(vcpu->vcpu_id > KVM_MAX_VCPUS - 1);
	atomic_clear_mask(CPUSTAT_WAIT, &vcpu->arch.sie_block->cpuflags);
	clear_bit(vcpu->vcpu_id, vcpu->arch.local_int.float_int->idle_mask);
}

static void __reset_intercept_indicators(struct kvm_vcpu *vcpu)
{
	atomic_clear_mask(CPUSTAT_ECALL_PEND |
		CPUSTAT_IO_INT | CPUSTAT_EXT_INT | CPUSTAT_STOP_INT,
		&vcpu->arch.sie_block->cpuflags);
	vcpu->arch.sie_block->lctl = 0x0000;
	vcpu->arch.sie_block->ictl &= ~ICTL_LPSW;
}

static void __set_cpuflag(struct kvm_vcpu *vcpu, u32 flag)
{
	atomic_set_mask(flag, &vcpu->arch.sie_block->cpuflags);
}

static void __set_intercept_indicator(struct kvm_vcpu *vcpu,
				      struct kvm_s390_interrupt_info *inti)
{
	switch (inti->type) {
	case KVM_S390_INT_EXTERNAL_CALL:
	case KVM_S390_INT_EMERGENCY:
	case KVM_S390_INT_SERVICE:
	case KVM_S390_INT_PFAULT_INIT:
	case KVM_S390_INT_PFAULT_DONE:
	case KVM_S390_INT_VIRTIO:
		if (psw_extint_disabled(vcpu))
			__set_cpuflag(vcpu, CPUSTAT_EXT_INT);
		else
			vcpu->arch.sie_block->lctl |= LCTL_CR0;
		break;
	case KVM_S390_SIGP_STOP:
		__set_cpuflag(vcpu, CPUSTAT_STOP_INT);
		break;
	case KVM_S390_MCHK:
		if (psw_mchk_disabled(vcpu))
			vcpu->arch.sie_block->ictl |= ICTL_LPSW;
		else
			vcpu->arch.sie_block->lctl |= LCTL_CR14;
		break;
	case KVM_S390_INT_IO_MIN...KVM_S390_INT_IO_MAX:
		if (psw_ioint_disabled(vcpu))
			__set_cpuflag(vcpu, CPUSTAT_IO_INT);
		else
			vcpu->arch.sie_block->lctl |= LCTL_CR6;
		break;
	default:
		BUG();
	}
}

static void __do_deliver_interrupt(struct kvm_vcpu *vcpu,
				   struct kvm_s390_interrupt_info *inti)
{
	const unsigned short table[] = { 2, 4, 4, 6 };
	int rc = 0;

	switch (inti->type) {
	case KVM_S390_INT_EMERGENCY:
		VCPU_EVENT(vcpu, 4, "%s", "interrupt: sigp emerg");
		vcpu->stat.deliver_emergency_signal++;
		trace_kvm_s390_deliver_interrupt(vcpu->vcpu_id, inti->type,
						 inti->emerg.code, 0);
		rc  = put_guest(vcpu, 0x1201, (u16 __user *)__LC_EXT_INT_CODE);
		rc |= put_guest(vcpu, inti->emerg.code,
				(u16 __user *)__LC_EXT_CPU_ADDR);
		rc |= copy_to_guest(vcpu, __LC_EXT_OLD_PSW,
				    &vcpu->arch.sie_block->gpsw, sizeof(psw_t));
		rc |= copy_from_guest(vcpu, &vcpu->arch.sie_block->gpsw,
				      __LC_EXT_NEW_PSW, sizeof(psw_t));
		break;
	case KVM_S390_INT_EXTERNAL_CALL:
		VCPU_EVENT(vcpu, 4, "%s", "interrupt: sigp ext call");
		vcpu->stat.deliver_external_call++;
		trace_kvm_s390_deliver_interrupt(vcpu->vcpu_id, inti->type,
						 inti->extcall.code, 0);
		rc  = put_guest(vcpu, 0x1202, (u16 __user *)__LC_EXT_INT_CODE);
		rc |= put_guest(vcpu, inti->extcall.code,
				(u16 __user *)__LC_EXT_CPU_ADDR);
		rc |= copy_to_guest(vcpu, __LC_EXT_OLD_PSW,
				    &vcpu->arch.sie_block->gpsw, sizeof(psw_t));
		rc |= copy_from_guest(vcpu, &vcpu->arch.sie_block->gpsw,
				      __LC_EXT_NEW_PSW, sizeof(psw_t));
		break;
	case KVM_S390_INT_SERVICE:
		VCPU_EVENT(vcpu, 4, "interrupt: sclp parm:%x",
			   inti->ext.ext_params);
		vcpu->stat.deliver_service_signal++;
		trace_kvm_s390_deliver_interrupt(vcpu->vcpu_id, inti->type,
						 inti->ext.ext_params, 0);
		rc  = put_guest(vcpu, 0x2401, (u16 __user *)__LC_EXT_INT_CODE);
		rc |= copy_to_guest(vcpu, __LC_EXT_OLD_PSW,
				    &vcpu->arch.sie_block->gpsw, sizeof(psw_t));
		rc |= copy_from_guest(vcpu, &vcpu->arch.sie_block->gpsw,
				      __LC_EXT_NEW_PSW, sizeof(psw_t));
		rc |= put_guest(vcpu, inti->ext.ext_params,
				(u32 __user *)__LC_EXT_PARAMS);
		break;
	case KVM_S390_INT_PFAULT_INIT:
		trace_kvm_s390_deliver_interrupt(vcpu->vcpu_id, inti->type, 0,
						 inti->ext.ext_params2);
		rc  = put_guest(vcpu, 0x2603, (u16 __user *) __LC_EXT_INT_CODE);
		rc |= put_guest(vcpu, 0x0600, (u16 __user *) __LC_EXT_CPU_ADDR);
		rc |= copy_to_guest(vcpu, __LC_EXT_OLD_PSW,
				    &vcpu->arch.sie_block->gpsw, sizeof(psw_t));
		rc |= copy_from_guest(vcpu, &vcpu->arch.sie_block->gpsw,
				      __LC_EXT_NEW_PSW, sizeof(psw_t));
		rc |= put_guest(vcpu, inti->ext.ext_params2,
				(u64 __user *) __LC_EXT_PARAMS2);
		break;
	case KVM_S390_INT_PFAULT_DONE:
		trace_kvm_s390_deliver_interrupt(vcpu->vcpu_id, inti->type, 0,
						 inti->ext.ext_params2);
		rc  = put_guest(vcpu, 0x2603, (u16 __user *) __LC_EXT_INT_CODE);
		rc |= put_guest(vcpu, 0x0680, (u16 __user *) __LC_EXT_CPU_ADDR);
		rc |= copy_to_guest(vcpu, __LC_EXT_OLD_PSW,
				    &vcpu->arch.sie_block->gpsw, sizeof(psw_t));
		rc |= copy_from_guest(vcpu, &vcpu->arch.sie_block->gpsw,
				      __LC_EXT_NEW_PSW, sizeof(psw_t));
		rc |= put_guest(vcpu, inti->ext.ext_params2,
				(u64 __user *) __LC_EXT_PARAMS2);
		break;
	case KVM_S390_INT_VIRTIO:
		VCPU_EVENT(vcpu, 4, "interrupt: virtio parm:%x,parm64:%llx",
			   inti->ext.ext_params, inti->ext.ext_params2);
		vcpu->stat.deliver_virtio_interrupt++;
		trace_kvm_s390_deliver_interrupt(vcpu->vcpu_id, inti->type,
						 inti->ext.ext_params,
						 inti->ext.ext_params2);
		rc  = put_guest(vcpu, 0x2603, (u16 __user *)__LC_EXT_INT_CODE);
		rc |= put_guest(vcpu, 0x0d00, (u16 __user *)__LC_EXT_CPU_ADDR);
		rc |= copy_to_guest(vcpu, __LC_EXT_OLD_PSW,
				    &vcpu->arch.sie_block->gpsw, sizeof(psw_t));
		rc |= copy_from_guest(vcpu, &vcpu->arch.sie_block->gpsw,
				      __LC_EXT_NEW_PSW, sizeof(psw_t));
		rc |= put_guest(vcpu, inti->ext.ext_params,
				(u32 __user *)__LC_EXT_PARAMS);
		rc |= put_guest(vcpu, inti->ext.ext_params2,
				(u64 __user *)__LC_EXT_PARAMS2);
		break;
	case KVM_S390_SIGP_STOP:
		VCPU_EVENT(vcpu, 4, "%s", "interrupt: cpu stop");
		vcpu->stat.deliver_stop_signal++;
		trace_kvm_s390_deliver_interrupt(vcpu->vcpu_id, inti->type,
						 0, 0);
		__set_intercept_indicator(vcpu, inti);
		break;

	case KVM_S390_SIGP_SET_PREFIX:
		VCPU_EVENT(vcpu, 4, "interrupt: set prefix to %x",
			   inti->prefix.address);
		vcpu->stat.deliver_prefix_signal++;
		trace_kvm_s390_deliver_interrupt(vcpu->vcpu_id, inti->type,
						 inti->prefix.address, 0);
		kvm_s390_set_prefix(vcpu, inti->prefix.address);
		break;

	case KVM_S390_RESTART:
		VCPU_EVENT(vcpu, 4, "%s", "interrupt: cpu restart");
		vcpu->stat.deliver_restart_signal++;
		trace_kvm_s390_deliver_interrupt(vcpu->vcpu_id, inti->type,
						 0, 0);
		rc  = copy_to_guest(vcpu,
				    offsetof(struct _lowcore, restart_old_psw),
				    &vcpu->arch.sie_block->gpsw, sizeof(psw_t));
		rc |= copy_from_guest(vcpu, &vcpu->arch.sie_block->gpsw,
				      offsetof(struct _lowcore, restart_psw),
				      sizeof(psw_t));
		atomic_clear_mask(CPUSTAT_STOPPED, &vcpu->arch.sie_block->cpuflags);
		break;
	case KVM_S390_PROGRAM_INT:
		VCPU_EVENT(vcpu, 4, "interrupt: pgm check code:%x, ilc:%x",
			   inti->pgm.code,
			   table[vcpu->arch.sie_block->ipa >> 14]);
		vcpu->stat.deliver_program_int++;
		trace_kvm_s390_deliver_interrupt(vcpu->vcpu_id, inti->type,
						 inti->pgm.code, 0);
		rc  = put_guest(vcpu, inti->pgm.code, (u16 __user *)__LC_PGM_INT_CODE);
		rc |= put_guest(vcpu, table[vcpu->arch.sie_block->ipa >> 14],
				(u16 __user *)__LC_PGM_ILC);
		rc |= copy_to_guest(vcpu, __LC_PGM_OLD_PSW,
				    &vcpu->arch.sie_block->gpsw, sizeof(psw_t));
		rc |= copy_from_guest(vcpu, &vcpu->arch.sie_block->gpsw,
				      __LC_PGM_NEW_PSW, sizeof(psw_t));
		break;

	case KVM_S390_MCHK:
		VCPU_EVENT(vcpu, 4, "interrupt: machine check mcic=%llx",
			   inti->mchk.mcic);
		trace_kvm_s390_deliver_interrupt(vcpu->vcpu_id, inti->type,
						 inti->mchk.cr14,
						 inti->mchk.mcic);
		rc  = kvm_s390_vcpu_store_status(vcpu,
						 KVM_S390_STORE_STATUS_PREFIXED);
		rc |= put_guest(vcpu, inti->mchk.mcic, (u64 __user *) __LC_MCCK_CODE);
		rc |= copy_to_guest(vcpu, __LC_MCK_OLD_PSW,
				    &vcpu->arch.sie_block->gpsw, sizeof(psw_t));
		rc |= copy_from_guest(vcpu, &vcpu->arch.sie_block->gpsw,
				      __LC_MCK_NEW_PSW, sizeof(psw_t));
		break;

	case KVM_S390_INT_IO_MIN...KVM_S390_INT_IO_MAX:
	{
		__u32 param0 = ((__u32)inti->io.subchannel_id << 16) |
			inti->io.subchannel_nr;
		__u64 param1 = ((__u64)inti->io.io_int_parm << 32) |
			inti->io.io_int_word;
		VCPU_EVENT(vcpu, 4, "interrupt: I/O %llx", inti->type);
		vcpu->stat.deliver_io_int++;
		trace_kvm_s390_deliver_interrupt(vcpu->vcpu_id, inti->type,
						 param0, param1);
		rc  = put_guest(vcpu, inti->io.subchannel_id,
				(u16 __user *) __LC_SUBCHANNEL_ID);
		rc |= put_guest(vcpu, inti->io.subchannel_nr,
				(u16 __user *) __LC_SUBCHANNEL_NR);
		rc |= put_guest(vcpu, inti->io.io_int_parm,
				(u32 __user *) __LC_IO_INT_PARM);
		rc |= put_guest(vcpu, inti->io.io_int_word,
				(u32 __user *) __LC_IO_INT_WORD);
		rc |= copy_to_guest(vcpu, __LC_IO_OLD_PSW,
				    &vcpu->arch.sie_block->gpsw, sizeof(psw_t));
		rc |= copy_from_guest(vcpu, &vcpu->arch.sie_block->gpsw,
				      __LC_IO_NEW_PSW, sizeof(psw_t));
		break;
	}
	default:
		BUG();
	}
	if (rc) {
		printk("kvm: The guest lowcore is not mapped during interrupt "
		       "delivery, killing userspace\n");
		do_exit(SIGKILL);
	}
}

static int __try_deliver_ckc_interrupt(struct kvm_vcpu *vcpu)
{
	int rc;

	if (psw_extint_disabled(vcpu))
		return 0;
	if (!(vcpu->arch.sie_block->gcr[0] & 0x800ul))
		return 0;
	rc  = put_guest(vcpu, 0x1004, (u16 __user *)__LC_EXT_INT_CODE);
	rc |= copy_to_guest(vcpu, __LC_EXT_OLD_PSW,
			    &vcpu->arch.sie_block->gpsw, sizeof(psw_t));
	rc |= copy_from_guest(vcpu, &vcpu->arch.sie_block->gpsw,
			      __LC_EXT_NEW_PSW, sizeof(psw_t));
	if (rc) {
		printk("kvm: The guest lowcore is not mapped during interrupt "
			"delivery, killing userspace\n");
		do_exit(SIGKILL);
	}
	return 1;
}

int kvm_cpu_has_interrupt(struct kvm_vcpu *vcpu)
{
	struct kvm_s390_local_interrupt *li = &vcpu->arch.local_int;
	struct kvm_s390_float_interrupt *fi = vcpu->arch.local_int.float_int;
	struct kvm_s390_interrupt_info  *inti;
	int rc = 0;

	if (atomic_read(&li->active)) {
		spin_lock_bh(&li->lock);
		list_for_each_entry(inti, &li->list, list)
			if (__interrupt_is_deliverable(vcpu, inti)) {
				rc = 1;
				break;
			}
		spin_unlock_bh(&li->lock);
	}

	if ((!rc) && atomic_read(&fi->active)) {
		spin_lock(&fi->lock);
		list_for_each_entry(inti, &fi->list, list)
			if (__interrupt_is_deliverable(vcpu, inti)) {
				rc = 1;
				break;
			}
		spin_unlock(&fi->lock);
	}

	if ((!rc) && (vcpu->arch.sie_block->ckc <
		get_tod_clock_fast() + vcpu->arch.sie_block->epoch)) {
		if ((!psw_extint_disabled(vcpu)) &&
			(vcpu->arch.sie_block->gcr[0] & 0x800ul))
			rc = 1;
	}

	return rc;
}

int kvm_cpu_has_pending_timer(struct kvm_vcpu *vcpu)
{
	return 0;
}

int kvm_s390_handle_wait(struct kvm_vcpu *vcpu)
{
	u64 now, sltime;
	DECLARE_WAITQUEUE(wait, current);

	vcpu->stat.exit_wait_state++;
	if (kvm_cpu_has_interrupt(vcpu))
		return 0;

	__set_cpu_idle(vcpu);
	spin_lock_bh(&vcpu->arch.local_int.lock);
	vcpu->arch.local_int.timer_due = 0;
	spin_unlock_bh(&vcpu->arch.local_int.lock);

	if (psw_interrupts_disabled(vcpu)) {
		VCPU_EVENT(vcpu, 3, "%s", "disabled wait");
		__unset_cpu_idle(vcpu);
		return -EOPNOTSUPP; /* disabled wait */
	}

	if (psw_extint_disabled(vcpu) ||
	    (!(vcpu->arch.sie_block->gcr[0] & 0x800ul))) {
		VCPU_EVENT(vcpu, 3, "%s", "enabled wait w/o timer");
		goto no_timer;
	}

	now = get_tod_clock_fast() + vcpu->arch.sie_block->epoch;
	if (vcpu->arch.sie_block->ckc < now) {
		__unset_cpu_idle(vcpu);
		return 0;
	}

	sltime = tod_to_ns(vcpu->arch.sie_block->ckc - now);

	hrtimer_start(&vcpu->arch.ckc_timer, ktime_set (0, sltime) , HRTIMER_MODE_REL);
	VCPU_EVENT(vcpu, 5, "enabled wait via clock comparator: %llx ns", sltime);
no_timer:
	srcu_read_unlock(&vcpu->kvm->srcu, vcpu->srcu_idx);
	spin_lock(&vcpu->arch.local_int.float_int->lock);
	spin_lock_bh(&vcpu->arch.local_int.lock);
	add_wait_queue(&vcpu->wq, &wait);
	while (list_empty(&vcpu->arch.local_int.list) &&
		list_empty(&vcpu->arch.local_int.float_int->list) &&
		(!vcpu->arch.local_int.timer_due) &&
		!signal_pending(current)) {
		set_current_state(TASK_INTERRUPTIBLE);
		spin_unlock_bh(&vcpu->arch.local_int.lock);
		spin_unlock(&vcpu->arch.local_int.float_int->lock);
		schedule();
		spin_lock(&vcpu->arch.local_int.float_int->lock);
		spin_lock_bh(&vcpu->arch.local_int.lock);
	}
	__unset_cpu_idle(vcpu);
	__set_current_state(TASK_RUNNING);
	remove_wait_queue(&vcpu->wq, &wait);
	spin_unlock_bh(&vcpu->arch.local_int.lock);
	spin_unlock(&vcpu->arch.local_int.float_int->lock);
	vcpu->srcu_idx = srcu_read_lock(&vcpu->kvm->srcu);

	hrtimer_try_to_cancel(&vcpu->arch.ckc_timer);
	return 0;
}

void kvm_s390_tasklet(unsigned long parm)
{
	struct kvm_vcpu *vcpu = (struct kvm_vcpu *) parm;

	spin_lock(&vcpu->arch.local_int.lock);
	vcpu->arch.local_int.timer_due = 1;
	if (waitqueue_active(&vcpu->wq))
		wake_up_interruptible(&vcpu->wq);
	spin_unlock(&vcpu->arch.local_int.lock);
}

/*
 * low level hrtimer wake routine. Because this runs in hardirq context
 * we schedule a tasklet to do the real work.
 */
enum hrtimer_restart kvm_s390_idle_wakeup(struct hrtimer *timer)
{
	struct kvm_vcpu *vcpu;

	vcpu = container_of(timer, struct kvm_vcpu, arch.ckc_timer);
	tasklet_schedule(&vcpu->arch.tasklet);

	return HRTIMER_NORESTART;
}

void kvm_s390_deliver_pending_interrupts(struct kvm_vcpu *vcpu)
{
	struct kvm_s390_local_interrupt *li = &vcpu->arch.local_int;
	struct kvm_s390_float_interrupt *fi = vcpu->arch.local_int.float_int;
	struct kvm_s390_interrupt_info  *n, *inti = NULL;
	int deliver;

	__reset_intercept_indicators(vcpu);
	if (atomic_read(&li->active)) {
		do {
			deliver = 0;
			spin_lock_bh(&li->lock);
			list_for_each_entry_safe(inti, n, &li->list, list) {
				if (__interrupt_is_deliverable(vcpu, inti)) {
					list_del(&inti->list);
					deliver = 1;
					break;
				}
				__set_intercept_indicator(vcpu, inti);
			}
			if (list_empty(&li->list))
				atomic_set(&li->active, 0);
			spin_unlock_bh(&li->lock);
			if (deliver) {
				__do_deliver_interrupt(vcpu, inti);
				kfree(inti);
			}
		} while (deliver);
	}

	if ((vcpu->arch.sie_block->ckc <
		get_tod_clock_fast() + vcpu->arch.sie_block->epoch))
		__try_deliver_ckc_interrupt(vcpu);

	if (atomic_read(&fi->active)) {
		do {
			deliver = 0;
			spin_lock(&fi->lock);
			list_for_each_entry_safe(inti, n, &fi->list, list) {
				if (__interrupt_is_deliverable(vcpu, inti)) {
					list_del(&inti->list);
					fi->irq_count--;
					deliver = 1;
					break;
				}
				__set_intercept_indicator(vcpu, inti);
			}
			if (list_empty(&fi->list))
				atomic_set(&fi->active, 0);
			spin_unlock(&fi->lock);
			if (deliver) {
				__do_deliver_interrupt(vcpu, inti);
				kfree(inti);
			}
		} while (deliver);
	}
}

void kvm_s390_deliver_pending_machine_checks(struct kvm_vcpu *vcpu)
{
	struct kvm_s390_local_interrupt *li = &vcpu->arch.local_int;
	struct kvm_s390_float_interrupt *fi = vcpu->arch.local_int.float_int;
	struct kvm_s390_interrupt_info  *n, *inti = NULL;
	int deliver;

	__reset_intercept_indicators(vcpu);
	if (atomic_read(&li->active)) {
		do {
			deliver = 0;
			spin_lock_bh(&li->lock);
			list_for_each_entry_safe(inti, n, &li->list, list) {
				if ((inti->type == KVM_S390_MCHK) &&
				    __interrupt_is_deliverable(vcpu, inti)) {
					list_del(&inti->list);
					deliver = 1;
					break;
				}
				__set_intercept_indicator(vcpu, inti);
			}
			if (list_empty(&li->list))
				atomic_set(&li->active, 0);
			spin_unlock_bh(&li->lock);
			if (deliver) {
				__do_deliver_interrupt(vcpu, inti);
				kfree(inti);
			}
		} while (deliver);
	}

	if (atomic_read(&fi->active)) {
		do {
			deliver = 0;
			spin_lock(&fi->lock);
			list_for_each_entry_safe(inti, n, &fi->list, list) {
				if ((inti->type == KVM_S390_MCHK) &&
				    __interrupt_is_deliverable(vcpu, inti)) {
					list_del(&inti->list);
					fi->irq_count--;
					deliver = 1;
					break;
				}
				__set_intercept_indicator(vcpu, inti);
			}
			if (list_empty(&fi->list))
				atomic_set(&fi->active, 0);
			spin_unlock(&fi->lock);
			if (deliver) {
				__do_deliver_interrupt(vcpu, inti);
				kfree(inti);
			}
		} while (deliver);
	}
}

int kvm_s390_inject_program_int(struct kvm_vcpu *vcpu, u16 code)
{
	struct kvm_s390_local_interrupt *li = &vcpu->arch.local_int;
	struct kvm_s390_interrupt_info *inti;

	inti = kzalloc(sizeof(*inti), GFP_KERNEL);
	if (!inti)
		return -ENOMEM;

	inti->type = KVM_S390_PROGRAM_INT;
	inti->pgm.code = code;

	VCPU_EVENT(vcpu, 3, "inject: program check %d (from kernel)", code);
	trace_kvm_s390_inject_vcpu(vcpu->vcpu_id, inti->type, code, 0, 1);
	spin_lock_bh(&li->lock);
	list_add(&inti->list, &li->list);
	atomic_set(&li->active, 1);
	BUG_ON(waitqueue_active(li->wq));
	spin_unlock_bh(&li->lock);
	return 0;
}

struct kvm_s390_interrupt_info *kvm_s390_get_io_int(struct kvm *kvm,
						    u64 cr6, u64 schid)
{
	struct kvm_s390_float_interrupt *fi;
	struct kvm_s390_interrupt_info *inti, *iter;

	if ((!schid && !cr6) || (schid && cr6))
		return NULL;
	mutex_lock(&kvm->lock);
	fi = &kvm->arch.float_int;
	spin_lock(&fi->lock);
	inti = NULL;
	list_for_each_entry(iter, &fi->list, list) {
		if (!is_ioint(iter->type))
			continue;
		if (cr6 &&
		    ((cr6 & int_word_to_isc_bits(iter->io.io_int_word)) == 0))
			continue;
		if (schid) {
			if (((schid & 0x00000000ffff0000) >> 16) !=
			    iter->io.subchannel_id)
				continue;
			if ((schid & 0x000000000000ffff) !=
			    iter->io.subchannel_nr)
				continue;
		}
		inti = iter;
		break;
	}
	if (inti) {
		list_del_init(&inti->list);
		fi->irq_count--;
	}
	if (list_empty(&fi->list))
		atomic_set(&fi->active, 0);
	spin_unlock(&fi->lock);
	mutex_unlock(&kvm->lock);
	return inti;
}

static int __inject_vm(struct kvm *kvm, struct kvm_s390_interrupt_info *inti)
{
	struct kvm_s390_local_interrupt *li;
	struct kvm_s390_float_interrupt *fi;
	struct kvm_s390_interrupt_info *iter;
	int sigcpu;
	int rc = 0;

	mutex_lock(&kvm->lock);
	fi = &kvm->arch.float_int;
	spin_lock(&fi->lock);
	if (fi->irq_count >= KVM_S390_MAX_FLOAT_IRQS) {
		rc = -EINVAL;
		goto unlock_fi;
	}
	fi->irq_count++;
	if (!is_ioint(inti->type)) {
		list_add_tail(&inti->list, &fi->list);
	} else {
		u64 isc_bits = int_word_to_isc_bits(inti->io.io_int_word);

		/* Keep I/O interrupts sorted in isc order. */
		list_for_each_entry(iter, &fi->list, list) {
			if (!is_ioint(iter->type))
				continue;
			if (int_word_to_isc_bits(iter->io.io_int_word)
			    <= isc_bits)
				continue;
			break;
		}
		list_add_tail(&inti->list, &iter->list);
	}
	atomic_set(&fi->active, 1);
	sigcpu = find_first_bit(fi->idle_mask, KVM_MAX_VCPUS);
	if (sigcpu == KVM_MAX_VCPUS) {
		do {
			sigcpu = fi->next_rr_cpu++;
			if (sigcpu == KVM_MAX_VCPUS)
				sigcpu = fi->next_rr_cpu = 0;
		} while (fi->local_int[sigcpu] == NULL);
	}
	li = fi->local_int[sigcpu];
	spin_lock_bh(&li->lock);
	atomic_set_mask(CPUSTAT_EXT_INT, li->cpuflags);
	if (waitqueue_active(li->wq))
		wake_up_interruptible(li->wq);
	spin_unlock_bh(&li->lock);
unlock_fi:
	spin_unlock(&fi->lock);
	mutex_unlock(&kvm->lock);
	return rc;
}

int kvm_s390_inject_vm(struct kvm *kvm,
		       struct kvm_s390_interrupt *s390int)
{
	struct kvm_s390_interrupt_info *inti;

	inti = kzalloc(sizeof(*inti), GFP_KERNEL);
	if (!inti)
		return -ENOMEM;

	inti->type = s390int->type;
	switch (inti->type) {
	case KVM_S390_INT_VIRTIO:
		VM_EVENT(kvm, 5, "inject: virtio parm:%x,parm64:%llx",
			 s390int->parm, s390int->parm64);
		inti->ext.ext_params = s390int->parm;
		inti->ext.ext_params2 = s390int->parm64;
		break;
	case KVM_S390_INT_SERVICE:
		VM_EVENT(kvm, 5, "inject: sclp parm:%x", s390int->parm);
		inti->ext.ext_params = s390int->parm;
		break;
	case KVM_S390_INT_PFAULT_DONE:
		inti->type = s390int->type;
		inti->ext.ext_params2 = s390int->parm64;
		break;
	case KVM_S390_MCHK:
		VM_EVENT(kvm, 5, "inject: machine check parm64:%llx",
			 s390int->parm64);
		inti->mchk.cr14 = s390int->parm; /* upper bits are not used */
		inti->mchk.mcic = s390int->parm64;
		break;
	case KVM_S390_INT_IO_MIN...KVM_S390_INT_IO_MAX:
		if (inti->type & IOINT_AI_MASK)
			VM_EVENT(kvm, 5, "%s", "inject: I/O (AI)");
		else
			VM_EVENT(kvm, 5, "inject: I/O css %x ss %x schid %04x",
				 s390int->type & IOINT_CSSID_MASK,
				 s390int->type & IOINT_SSID_MASK,
				 s390int->type & IOINT_SCHID_MASK);
		inti->io.subchannel_id = s390int->parm >> 16;
		inti->io.subchannel_nr = s390int->parm & 0x0000ffffu;
		inti->io.io_int_parm = s390int->parm64 >> 32;
		inti->io.io_int_word = s390int->parm64 & 0x00000000ffffffffull;
		break;
	default:
		kfree(inti);
		return -EINVAL;
	}
	trace_kvm_s390_inject_vm(s390int->type, s390int->parm, s390int->parm64,
				 2);

	return __inject_vm(kvm, inti);
}

int kvm_s390_inject_vcpu(struct kvm_vcpu *vcpu,
			 struct kvm_s390_interrupt *s390int)
{
	struct kvm_s390_local_interrupt *li;
	struct kvm_s390_interrupt_info *inti;

	inti = kzalloc(sizeof(*inti), GFP_KERNEL);
	if (!inti)
		return -ENOMEM;

	switch (s390int->type) {
	case KVM_S390_PROGRAM_INT:
		if (s390int->parm & 0xffff0000) {
			kfree(inti);
			return -EINVAL;
		}
		inti->type = s390int->type;
		inti->pgm.code = s390int->parm;
		VCPU_EVENT(vcpu, 3, "inject: program check %d (from user)",
			   s390int->parm);
		break;
	case KVM_S390_SIGP_SET_PREFIX:
		inti->prefix.address = s390int->parm;
		inti->type = s390int->type;
		VCPU_EVENT(vcpu, 3, "inject: set prefix to %x (from user)",
			   s390int->parm);
		break;
	case KVM_S390_SIGP_STOP:
	case KVM_S390_RESTART:
		VCPU_EVENT(vcpu, 3, "inject: type %x", s390int->type);
		inti->type = s390int->type;
		break;
	case KVM_S390_INT_EXTERNAL_CALL:
		if (s390int->parm & 0xffff0000) {
			kfree(inti);
			return -EINVAL;
		}
		VCPU_EVENT(vcpu, 3, "inject: external call source-cpu:%u",
			   s390int->parm);
		inti->type = s390int->type;
		inti->extcall.code = s390int->parm;
		break;
	case KVM_S390_INT_EMERGENCY:
		if (s390int->parm & 0xffff0000) {
			kfree(inti);
			return -EINVAL;
		}
		VCPU_EVENT(vcpu, 3, "inject: emergency %u\n", s390int->parm);
		inti->type = s390int->type;
		inti->emerg.code = s390int->parm;
		break;
	case KVM_S390_MCHK:
		VCPU_EVENT(vcpu, 5, "inject: machine check parm64:%llx",
			   s390int->parm64);
		inti->type = s390int->type;
		inti->mchk.mcic = s390int->parm64;
		break;
	case KVM_S390_INT_PFAULT_INIT:
		inti->type = s390int->type;
		inti->ext.ext_params2 = s390int->parm64;
		break;
	case KVM_S390_INT_VIRTIO:
	case KVM_S390_INT_SERVICE:
	case KVM_S390_INT_IO_MIN...KVM_S390_INT_IO_MAX:
	default:
		kfree(inti);
		return -EINVAL;
	}
	trace_kvm_s390_inject_vcpu(vcpu->vcpu_id, s390int->type, s390int->parm,
				   s390int->parm64, 2);

	mutex_lock(&vcpu->kvm->lock);
	li = &vcpu->arch.local_int;
	spin_lock_bh(&li->lock);
	if (inti->type == KVM_S390_PROGRAM_INT)
		list_add(&inti->list, &li->list);
	else
		list_add_tail(&inti->list, &li->list);
	atomic_set(&li->active, 1);
	if (inti->type == KVM_S390_SIGP_STOP)
		li->action_bits |= ACTION_STOP_ON_STOP;
	atomic_set_mask(CPUSTAT_EXT_INT, li->cpuflags);
	if (waitqueue_active(&vcpu->wq))
		wake_up_interruptible(&vcpu->wq);
	spin_unlock_bh(&li->lock);
	mutex_unlock(&vcpu->kvm->lock);
	return 0;
}

static void clear_floating_interrupts(struct kvm *kvm)
{
	struct kvm_s390_float_interrupt *fi;
	struct kvm_s390_interrupt_info	*n, *inti = NULL;

	mutex_lock(&kvm->lock);
	fi = &kvm->arch.float_int;
	spin_lock(&fi->lock);
	list_for_each_entry_safe(inti, n, &fi->list, list) {
		list_del(&inti->list);
		kfree(inti);
	}
	fi->irq_count = 0;
	atomic_set(&fi->active, 0);
	spin_unlock(&fi->lock);
	mutex_unlock(&kvm->lock);
}

static inline int copy_irq_to_user(struct kvm_s390_interrupt_info *inti,
				   u8 *addr)
{
	struct kvm_s390_irq __user *uptr = (struct kvm_s390_irq __user *) addr;
	struct kvm_s390_irq irq = {0};

	irq.type = inti->type;
	switch (inti->type) {
	case KVM_S390_INT_PFAULT_INIT:
	case KVM_S390_INT_PFAULT_DONE:
	case KVM_S390_INT_VIRTIO:
	case KVM_S390_INT_SERVICE:
		irq.u.ext = inti->ext;
		break;
	case KVM_S390_INT_IO_MIN...KVM_S390_INT_IO_MAX:
		irq.u.io = inti->io;
		break;
	case KVM_S390_MCHK:
		irq.u.mchk = inti->mchk;
		break;
	default:
		return -EINVAL;
	}

	if (copy_to_user(uptr, &irq, sizeof(irq)))
		return -EFAULT;

	return 0;
}

static int get_all_floating_irqs(struct kvm *kvm, __u8 *buf, __u64 len)
{
	struct kvm_s390_interrupt_info *inti;
	struct kvm_s390_float_interrupt *fi;
	int ret = 0;
	int n = 0;

	mutex_lock(&kvm->lock);
	fi = &kvm->arch.float_int;
	spin_lock(&fi->lock);

	list_for_each_entry(inti, &fi->list, list) {
		if (len < sizeof(struct kvm_s390_irq)) {
			/* signal userspace to try again */
			ret = -ENOMEM;
			break;
		}
		ret = copy_irq_to_user(inti, buf);
		if (ret)
			break;
		buf += sizeof(struct kvm_s390_irq);
		len -= sizeof(struct kvm_s390_irq);
		n++;
	}

	spin_unlock(&fi->lock);
	mutex_unlock(&kvm->lock);

	return ret < 0 ? ret : n;
}

static int flic_get_attr(struct kvm_device *dev, struct kvm_device_attr *attr)
{
	int r;

	switch (attr->group) {
	case KVM_DEV_FLIC_GET_ALL_IRQS:
		r = get_all_floating_irqs(dev->kvm, (u8 *) attr->addr,
					  attr->attr);
		break;
	default:
		r = -EINVAL;
	}

	return r;
}

static inline int copy_irq_from_user(struct kvm_s390_interrupt_info *inti,
				     u64 addr)
{
	struct kvm_s390_irq __user *uptr = (struct kvm_s390_irq __user *) addr;
	void *target = NULL;
	void __user *source;
	u64 size;

	if (get_user(inti->type, (u64 __user *)addr))
		return -EFAULT;

	switch (inti->type) {
	case KVM_S390_INT_PFAULT_INIT:
	case KVM_S390_INT_PFAULT_DONE:
	case KVM_S390_INT_VIRTIO:
	case KVM_S390_INT_SERVICE:
		target = (void *) &inti->ext;
		source = &uptr->u.ext;
		size = sizeof(inti->ext);
		break;
	case KVM_S390_INT_IO_MIN...KVM_S390_INT_IO_MAX:
		target = (void *) &inti->io;
		source = &uptr->u.io;
		size = sizeof(inti->io);
		break;
	case KVM_S390_MCHK:
		target = (void *) &inti->mchk;
		source = &uptr->u.mchk;
		size = sizeof(inti->mchk);
		break;
	default:
		return -EINVAL;
	}

	if (copy_from_user(target, source, size))
		return -EFAULT;

	return 0;
}

static int enqueue_floating_irq(struct kvm_device *dev,
				struct kvm_device_attr *attr)
{
	struct kvm_s390_interrupt_info *inti = NULL;
	int r = 0;
	int len = attr->attr;

	if (len % sizeof(struct kvm_s390_irq) != 0)
		return -EINVAL;
	else if (len > KVM_S390_FLIC_MAX_BUFFER)
		return -EINVAL;

	while (len >= sizeof(struct kvm_s390_irq)) {
		inti = kzalloc(sizeof(*inti), GFP_KERNEL);
		if (!inti)
			return -ENOMEM;

		r = copy_irq_from_user(inti, attr->addr);
		if (r) {
			kfree(inti);
			return r;
		}
		r = __inject_vm(dev->kvm, inti);
		if (r) {
			kfree(inti);
			return r;
		}
		len -= sizeof(struct kvm_s390_irq);
		attr->addr += sizeof(struct kvm_s390_irq);
	}

	return r;
}

static int flic_set_attr(struct kvm_device *dev, struct kvm_device_attr *attr)
{
	int r = 0;
	unsigned int i;
	struct kvm_vcpu *vcpu;

	switch (attr->group) {
	case KVM_DEV_FLIC_ENQUEUE:
		r = enqueue_floating_irq(dev, attr);
		break;
	case KVM_DEV_FLIC_CLEAR_IRQS:
		r = 0;
		clear_floating_interrupts(dev->kvm);
		break;
	case KVM_DEV_FLIC_APF_ENABLE:
		dev->kvm->arch.gmap->pfault_enabled = 1;
		break;
	case KVM_DEV_FLIC_APF_DISABLE_WAIT:
		dev->kvm->arch.gmap->pfault_enabled = 0;
		/*
		 * Make sure no async faults are in transition when
		 * clearing the queues. So we don't need to worry
		 * about late coming workers.
		 */
		synchronize_srcu(&dev->kvm->srcu);
		kvm_for_each_vcpu(i, vcpu, dev->kvm)
			kvm_clear_async_pf_completion_queue(vcpu);
		break;
	default:
		r = -EINVAL;
	}

	return r;
}

static int flic_create(struct kvm_device *dev, u32 type)
{
	if (!dev)
		return -EINVAL;
	if (dev->kvm->arch.flic)
		return -EINVAL;
	dev->kvm->arch.flic = dev;
	return 0;
}

static void flic_destroy(struct kvm_device *dev)
{
	dev->kvm->arch.flic = NULL;
	kfree(dev);
}

/* s390 floating irq controller (flic) */
struct kvm_device_ops kvm_flic_ops = {
	.name = "kvm-flic",
	.get_attr = flic_get_attr,
	.set_attr = flic_set_attr,
	.create = flic_create,
	.destroy = flic_destroy,
};
