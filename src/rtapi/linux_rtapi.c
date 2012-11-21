/********************************************************************
* Description:  linux_rtapi.c
*               This file, 'linux_rtapi.c', implements the RT API
*               functions for machines with Linux-realtime
*
* Author: John Kasunich, Paul Corner
* Copyright (c) 2004 All rights reserved.
*
* Copyright (c) 2009 Michael Buesch <mb@bu3sch.de>
*
* License: GPL Version 2
*
********************************************************************/

#define _GNU_SOURCE
#include <stdio.h>		/* vprintf() */
#include <stdlib.h>		/* malloc(), sizeof() */
#include <stdarg.h>		/* va_* */
#include <unistd.h>		/* usleep() */
#include <sys/ipc.h>		/* IPC_* */
#include <sys/shm.h>		/* shmget() */
#include <sys/io.h>		/* shmget() */
#include <time.h>		/* clock_gettime etc */
#include "rtapi.h"		/* these decls */
#include "rtapi_common.h"	
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <signal.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/mman.h>

#define MODULE_MAGIC		30812
#define TASK_MAGIC		21979	/* random numbers used as signatures */

#define MAX_TASKS		64
#define MAX_MODULES		64
#define MODULE_OFFSET		32768

#ifndef max
# define max(a, b)	((a) > (b) ? (a) : (b))
#endif

/* Lock for task_array and module_array allocations. */
static pthread_mutex_t array_mutex = PTHREAD_MUTEX_INITIALIZER;

/* local functions and data */

static pthread_key_t task_key;
static pthread_once_t task_key_once = PTHREAD_ONCE_INIT;
static int period;


static inline int task_id(task_data *task)
{
	return (int)(task - task_array);
}

static unsigned long rtapi_get_pagefault_count(task_data *task)
{
	struct rusage rusage;
	unsigned long minor, major;

	getrusage(RUSAGE_SELF, &rusage);
	minor = rusage.ru_minflt;
	major = rusage.ru_majflt;
	if (minor < task->minfault_base || major < task->majfault_base) {
		rtapi_print_msg(RTAPI_MSG_ERR, "rtapi task %d %s: Got invalid fault counts.\n",
				task_id(task), task->name);
		return 0;
	}
	minor -= task->minfault_base;
	major -= task->majfault_base;

	return minor + major;
}

static void rtapi_reset_pagefault_count(task_data *task)
{
	struct rusage rusage;

	getrusage(RUSAGE_SELF, &rusage);
	if (task->minfault_base != rusage.ru_minflt ||
	    task->majfault_base != rusage.ru_majflt) {
		task->minfault_base = rusage.ru_minflt;
		task->majfault_base = rusage.ru_majflt;
		rtapi_print_msg(RTAPI_MSG_DBG, "rtapi task %d %s: Reset pagefault counter\n",
				task_id(task), task->name);
	}
}

static void rtapi_advance_time(struct timespec *tv, unsigned long ns,
			       unsigned long s)
{
	ns += tv->tv_nsec;
	while (ns > 1000000000) {
		s++;
		ns -= 1000000000;
	}
	tv->tv_nsec = ns;
	tv->tv_sec += s;
}

static void rtapi_key_alloc()
{
	pthread_key_create(&task_key, NULL);
}

static void rtapi_set_task(task_data *t)
{
	pthread_once(&task_key_once, rtapi_key_alloc);
	pthread_setspecific(task_key, (void *)t);
}

static task_data *rtapi_this_task()
{
	pthread_once(&task_key_once, rtapi_key_alloc);
	return (task_data *)pthread_getspecific(task_key);
}

int rtapi_prio_highest(void)
{
	return sched_get_priority_max(SCHED_FIFO);
}

int rtapi_prio_lowest(void)
{
	return sched_get_priority_min(SCHED_FIFO);
}

int rtapi_prio_next_higher(int prio)
{
	/* return a valid priority for out of range arg */
	if (prio >= rtapi_prio_highest())
		return rtapi_prio_highest();
	if (prio < rtapi_prio_lowest())
		return rtapi_prio_lowest();

	/* return next higher priority for in-range arg */
	return prio + 1;
}

int rtapi_prio_next_lower(int prio)
{
	/* return a valid priority for out of range arg */
	if (prio <= rtapi_prio_lowest())
		return rtapi_prio_lowest();
	if (prio > rtapi_prio_highest())
		return rtapi_prio_highest();

	/* return next lower priority for in-range arg */
	return prio - 1;
}

int rtapi_init(const char *modname)
{
	int n, result = -ENOMEM;

	pthread_mutex_lock(&array_mutex);
	for (n = 0; n < MAX_MODULES; n++) {
		if (module_array[n].magic != MODULE_MAGIC) {
			result = n + MODULE_OFFSET;
			module_array[n].magic = MODULE_MAGIC;
			break;
		}
	}
	pthread_mutex_unlock(&array_mutex);

	return result;
}

int rtapi_exit(int id)
{
	int n = id - MODULE_OFFSET;

	if (n < 0 || n >= MAX_MODULES)
		return -1;
	/* Remove the module from the module_array. */
	pthread_mutex_lock(&array_mutex);
	module_array[n].magic = 0;
	pthread_mutex_unlock(&array_mutex);

	return 0;
}

int rtapi_clock_set_period(unsigned long int nsecs)
{
	struct timespec res = { 0, 0 };

	if (nsecs == 0)
		return period;
	if (period != 0) {
		rtapi_print_msg(RTAPI_MSG_ERR, "attempt to set period twice\n");
		return -EINVAL;
	}
	clock_getres(CLOCK_MONOTONIC, &res);
	period = (nsecs / res.tv_nsec) * res.tv_nsec;
	if (period < 1)
		period = res.tv_nsec;
	rtapi_print_msg(RTAPI_MSG_DBG,
			"rtapi_clock_set_period (res=%ld) -> %d\n", res.tv_nsec,
			period);

	return period;
}

int rtapi_task_new(void (*taskcode)(void *), void *arg,
		   int prio, int owner, unsigned long int stacksize,
		   int uses_fp, char *name, int cpu_id)
{
	int n;
	task_data *task;
	void *stackaddr;

	stacksize = max(stacksize, 16384);
	stackaddr = malloc(stacksize);
	if (!stackaddr) {
		rtapi_print_msg(RTAPI_MSG_ERR,
				"Failed to allocate realtime thread stack\n");
		return -ENOMEM;
	}
	memset(stackaddr, 0, stacksize);

	/* find an empty entry in the task array */
	pthread_mutex_lock(&array_mutex);
	n = 0;
	while ((n < MAX_TASKS) && (task_array[n].magic == TASK_MAGIC))
		n++;
	if (n == MAX_TASKS) {
		pthread_mutex_unlock(&array_mutex);
		free(stackaddr);
		return -ENOMEM;
	}
	task = &(task_array[n]);
	task->magic = TASK_MAGIC;
	pthread_mutex_unlock(&array_mutex);

	/* check requested priority */
	{
		int highest = rtapi_prio_highest();
		int lowest = rtapi_prio_lowest();
		if (prio < lowest || prio > highest) {
			rtapi_print_msg(RTAPI_MSG_ERR,
					"New task  %d  '%s': invalid priority %d (highest=%d lowest=%d)\n",
					n, name, prio, highest, lowest);
			free(stackaddr);
			return -EINVAL;
		}
		rtapi_print_msg(RTAPI_MSG_DBG,
				"Creating new task %d  '%s': requested priority %d (highest=%d lowest=%d)\n",
				n, name, prio, highest, lowest);
	}

	task->owner = owner;
	task->arg = arg;
	task->stacksize = stacksize;
	task->stackaddr = stackaddr;
	task->destroyed = 0;
	task->taskcode = taskcode;
	task->prio = prio;
	task->uses_fp = uses_fp;
	task->cpu = cpu_id;
	strncpy(task->name, name, sizeof(task->name));
	task->name[sizeof(task->name) - 1]= '\0';

	/* and return handle to the caller */

	return n;
}

int rtapi_task_delete(int id)
{
	task_data *task;
	void *returncode;
	int err;

	if (id < 0 || id >= MAX_TASKS)
		return -EINVAL;

	task = &(task_array[id]);
	/* validate task handle */
	if (task->magic != TASK_MAGIC)
		return -EINVAL;

	/* Signal thread termination and wait for the thread to exit. */
	if (!task->deleted) {
		task->deleted = 1;
		err = pthread_join(task->thread, &returncode);
		if (err)
			rtapi_print_msg(RTAPI_MSG_ERR, "pthread_join() on realtime thread failed\n");
	}
	/* Free the thread stack. */
	free(task->stackaddr);
	task->stackaddr = NULL;
	/* Remove the task from the task_array. */
	pthread_mutex_lock(&array_mutex);
	task->magic = 0;
	pthread_mutex_unlock(&array_mutex);

	return 0;
}

static int realtime_set_affinity(task_data *task)
{
	cpu_set_t set;
	int err, cpu_nr, use_cpu = -1;

	pthread_getaffinity_np(task->thread, sizeof(set), &set);
	if (task->cpu > -1) { // CPU set explicitly
	    if (!CPU_ISSET(task->cpu, &set)) {
		rtapi_print_msg(RTAPI_MSG_ERR, 
				"RTAPI: ERROR: realtime_set_affinity(%s): CPU %d not available\n",
				task->name, task->cpu);
		return -EINVAL;
	    }
	    use_cpu = task->cpu;
	} else {
	    // select last CPU as default
	    for (cpu_nr = CPU_SETSIZE - 1; cpu_nr >= 0; cpu_nr--) {
		if (CPU_ISSET(cpu_nr, &set)) {
		    use_cpu = cpu_nr;
		    break;
		}
	    }
	    if (use_cpu < 0) {
		rtapi_print_msg(RTAPI_MSG_ERR, "Unable to get ID of the last CPU\n");
		return -EINVAL;
	    }
	    rtapi_print_msg(RTAPI_MSG_DBG, "task %s: using default CPU %d\n",
			    task->name, use_cpu);
	}
	CPU_ZERO(&set);
	CPU_SET(use_cpu, &set);

	err = pthread_setaffinity_np(task->thread, sizeof(set), &set);
	if (err) {
	    rtapi_print_msg(RTAPI_MSG_ERR, "%d %s: Failed to set CPU affinity to CPU %d (%s)\n",
			    task_id(task), task->name, use_cpu, strerror(errno));
	    return -EINVAL;
	}
	rtapi_print_msg(RTAPI_MSG_DBG,"realtime_set_affinity(): task %s assigned to CPU %d\n", 
			task->name, use_cpu);
	return 0;
}

#define ENABLE_SCHED_DEADLINE	0 /*XXX set to 1 to enable deadline scheduling. */

#ifndef __NR_sched_setscheduler_ex
# if defined(__x86_64__)
#  define __NR_sched_setscheduler_ex	299
#  define __NR_sched_wait_interval	302
# elif defined(__i386__)
#  define __NR_sched_setscheduler_ex	337
#  define __NR_sched_wait_interval	340
# else
#  warning "SCHED_DEADLINE syscall numbers unknown"
# endif
#endif

#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE		6

struct sched_param_ex {
	int sched_priority;
	struct timespec sched_runtime;
	struct timespec sched_deadline;
	struct timespec sched_period;
	int sched_flags;
};

#define SCHED_SIG_RORUN		0x80000000
#define SCHED_SIG_DMISS		0x40000000

static inline int sched_setscheduler_ex(pid_t pid, int policy, unsigned len, struct sched_param_ex *param)
{
#ifdef __NR_sched_setscheduler_ex
	return syscall(__NR_sched_setscheduler_ex, pid, policy, len, param);
#endif
	return -ENOSYS;
}
static inline int sched_wait_interval(int flags, const struct timespec *rqtp,
				      struct timespec *rmtp)
{
#ifdef __NR_sched_wait_interval
	return syscall(__NR_sched_wait_interval, flags, rqtp, rmtp);
#endif
	return -ENOSYS;
}
#endif /* SCHED_DEADLINE */

static int error_printed;

static void deadline_exception(int signr)
{
	if (signr != SIGXCPU) {
		rtapi_print_msg(RTAPI_MSG_ERR, "Received unknown signal %d\n", signr);
		return;
	}
	if (!error_printed++)
	    rtapi_print_msg(RTAPI_MSG_ERR, "Missed scheduling deadline or overran "
			    "scheduling runtime!\n");
}

static int realtime_set_priority(task_data *task)
{
	struct sched_param schedp;
	struct sched_param_ex ex;
	struct sigaction sa;

	task->deadline_scheduling = 0;
	if (ENABLE_SCHED_DEADLINE) {
		memset(&sa, 0, sizeof(sa));
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = SA_RESTART;
		sa.sa_handler = deadline_exception;
		if (sigaction(SIGXCPU, &sa, NULL)) {
			rtapi_print_msg(RTAPI_MSG_ERR,
					"Unable to register SIGXCPU handler.\n");
			return -1;
		}

		memset(&ex, 0, sizeof(ex));
		ex.sched_deadline.tv_nsec = period;
		ex.sched_runtime.tv_nsec = 8000; //FIXME
		ex.sched_flags = SCHED_SIG_RORUN | SCHED_SIG_DMISS;
		rtapi_print_msg(RTAPI_MSG_DBG,"Setting deadline scheduler for %d\n", task_id(task));
		if (sched_setscheduler_ex(0, SCHED_DEADLINE, sizeof(ex), &ex)) {
			rtapi_print_msg(RTAPI_MSG_INFO,
					"Unable to set DEADLINE scheduling policy (%s). Trying FIFO.\n",
					strerror(errno));
		} else {
			rtapi_print_msg(RTAPI_MSG_INFO,
					"Running DEADLINE scheduling policy.\n");
			task->deadline_scheduling = 1;
			return 0;
		}
	}

	memset(&schedp, 0, sizeof(schedp));
	schedp.sched_priority = task->prio;
	if (sched_setscheduler(0, SCHED_FIFO, &schedp)) {
		rtapi_print_msg(RTAPI_MSG_ERR, "Unable to set FIFO scheduling policy: %s",
				strerror(errno));
		return 1;
	}

	return 0;
}

static void *realtime_thread(void *arg)
{
	task_data *task = arg;

	rtapi_set_task(task);

	/* The task should not pagefault at all. So reset the counter now.
	 * Note that currently we _do_ receive a few pagefaults in the taskcode
	 * init. This is noncritical and probably not worth fixing. */
	rtapi_reset_pagefault_count(task);

	if (task->period < period)
		task->period = period;
	task->ratio = task->period / period;
	rtapi_print_msg(RTAPI_MSG_DBG, "task %p period = %d ratio=%d\n",
			task, task->period, task->ratio);

	if (realtime_set_affinity(task))
		goto error;
	if (realtime_set_priority(task))
		goto error;

	/* We're done initializing. Open the barrier. */
	pthread_barrier_wait(&task->thread_init_barrier);

	clock_gettime(CLOCK_MONOTONIC, &task->next_time);
	rtapi_advance_time(&task->next_time, task->period, 0);

	/* call the task function with the task argument */
	task->taskcode(task->arg);

	rtapi_print_msg(RTAPI_MSG_ERR, "ERROR: reached end of realtime thread for task %d\n",
			task_id(task));
	task->deleted = 1;

	return NULL;
error:
	/* Signal that we're dead and open the barrier. */
	task->deleted = 1;
	pthread_barrier_wait(&task->thread_init_barrier);
	return NULL;
}

int rtapi_task_start(int task_id, unsigned long int period_nsec)
{
	task_data *task;
	pthread_attr_t attr;
	int retval;

	if (task_id < 0 || task_id >= MAX_TASKS)
		return -EINVAL;

	task = &task_array[task_id];

	/* validate task handle */
	if (task->magic != TASK_MAGIC)
		return -EINVAL;

	if (period_nsec < period)
		period_nsec = period;
	task->period = period_nsec;
	task->ratio = period_nsec / period;
	task->deleted = 0;

	/* create the thread - use the wrapper function, pass it a pointer
	   to the task structure so it can call the actual task function */

	pthread_barrier_init(&task->thread_init_barrier, NULL, 2);
	pthread_attr_init(&attr);
	pthread_attr_setstack(&attr, task->stackaddr, task->stacksize);
	rtapi_print_msg(RTAPI_MSG_DBG,"About to pthread_create task %d\n", task_id);
	retval = pthread_create(&task->thread, &attr, realtime_thread, (void *)task);
	rtapi_print_msg(RTAPI_MSG_DBG,"Created task %d\n", task_id);
	pthread_attr_destroy(&attr);
	if (retval) {
		pthread_barrier_destroy(&task->thread_init_barrier);
		rtapi_print_msg(RTAPI_MSG_ERR, "Failed to create realtime thread\n");
		return -ENOMEM;
	}
	/* Wait for the thread to do basic initialization. */
	pthread_barrier_wait(&task->thread_init_barrier);
	pthread_barrier_destroy(&task->thread_init_barrier);
	if (task->deleted) { /* The thread died in the init phase. */
		rtapi_print_msg(RTAPI_MSG_ERR, "Realtime thread initialization failed\n");
		return -ENOMEM;
	}
	rtapi_print_msg(RTAPI_MSG_DBG,"Task %d finished its basic init\n", task_id);

	return 0;
}

int rtapi_task_stop(int task_id)
{
	task_data *task;

	if (task_id < 0 || task_id >= MAX_TASKS)
		return -EINVAL;

	task = &task_array[task_id];

	/* validate task handle */
	if (task->magic != TASK_MAGIC)
		return -EINVAL;

	task->destroyed = 1;

	return 0;
}

int rtapi_task_pause(int task_id)
{
	task_data *task;

	if (task_id < 0 || task_id >= MAX_TASKS)
		return -EINVAL;

	task = &task_array[task_id];

	/* validate task handle */
	if (task->magic != TASK_MAGIC)
		return -EINVAL;

	return -ENOSYS;
}

int rtapi_task_resume(int task_id)
{
	task_data *task;

	if (task_id < 0 || task_id >= MAX_TASKS)
		return -EINVAL;

	task = &task_array[task_id];

	/* validate task handle */
	if (task->magic != TASK_MAGIC)
		return -EINVAL;

	return -ENOSYS;
}

int rtapi_task_set_period(int task_id, unsigned long int period_nsec)
{
	task_data *task;

	if (task_id < 0 || task_id >= MAX_TASKS)
		return -EINVAL;

	task = &task_array[task_id];

	/* validate task handle */
	if (task->magic != TASK_MAGIC)
		return -EINVAL;

	task->period = period_nsec;

	return 0;
}

int rtapi_wait(void)
{
	struct timespec ts;
	task_data *task = rtapi_this_task();

	if (task->deleted)
		pthread_exit(0);

	if (task->deadline_scheduling)
		sched_wait_interval(TIMER_ABSTIME, &task->next_time, NULL);
	else
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &task->next_time, NULL);
	rtapi_advance_time(&task->next_time, task->period, 0);
	clock_gettime(CLOCK_MONOTONIC, &ts);
	if (ts.tv_sec > task->next_time.tv_sec
	    || (ts.tv_sec == task->next_time.tv_sec
		&& ts.tv_nsec > task->next_time.tv_nsec)) {
		int msg_level = RTAPI_MSG_NONE;

		task->failures++;
		if (task->failures == 1)
			msg_level = RTAPI_MSG_ERR;
		//else if (task->failures < 10 || (task->failures % 10000 == 0))
		else if (task->failures < 10)
			msg_level = RTAPI_MSG_WARN;

		if (msg_level != RTAPI_MSG_NONE) {
			rtapi_print_msg(msg_level,
					"ERROR: Missed scheduling deadline for task %d [%d times]\n"
					"Now is %ld.%09ld, deadline was %ld.%09ld\n"
					"Absolute number of pagefaults in realtime context: %lu\n",
					task_id(task), task->failures,
					(long)ts.tv_sec, (long)ts.tv_nsec,
					(long)task->next_time.tv_sec,
					(long)task->next_time.tv_nsec,
					rtapi_get_pagefault_count(task));
		}
	}

	return 0;
}

void rtapi_outb(unsigned char byte, unsigned int port)
{
	outb(byte, port);
}

unsigned char rtapi_inb(unsigned int port)
{
	return inb(port);
}

void rtapi_outw(unsigned short word, unsigned int port)
{
	outw(word, port);
}

unsigned short rtapi_inw(unsigned int port)
{
	return inw(port);
}

long int simple_strtol(const char *nptr, char **endptr, int base)
{
	return strtol(nptr, endptr, base);
}

long long rtapi_get_time(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
}

// #include "rtapi/linux_common.h"
