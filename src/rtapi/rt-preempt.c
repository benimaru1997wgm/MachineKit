/********************************************************************
* Description:  rt-preempt.c
*
*               This file, 'rt-preempt.c', implements the unique
*               functions for the RT_PREEMPT thread system.
********************************************************************/

#include "config.h"
#include "rtapi.h"
#include "rtapi_common.h"

/***********************************************************************
*                           TASK FUNCTIONS                             *
************************************************************************/

#include <unistd.h>		// getpid(), syscall()
#include <time.h>               // clock_nanosleep()
#include <sys/resource.h>	// rusage, getrusage(), RUSAGE_SELF

#ifdef RTAPI
#include <stdlib.h>		// malloc(), sizeof(), free()
#include <string.h>		// memset()
#include <syscall.h>            // syscall(SYS_gettid);
#include <sys/prctl.h>          // prctl(PR_SET_NAME)

/* Lock for task_array and module_array allocations */
static pthread_key_t task_key;
static pthread_once_t task_key_once = PTHREAD_ONCE_INIT;

int _rtapi_task_self_hook(void);

#endif  /* RTAPI */

#define MODULE_OFFSET		32768

typedef struct {
    int deleted;
    int destroyed;
    struct timespec next_time;

    /* The realtime thread. */
    pthread_t thread;
    pthread_barrier_t thread_init_barrier;
    void *stackaddr;
    pid_t tid;       // as returned by gettid(2)

    /* Statistics */
    unsigned long minfault_base;
    unsigned long majfault_base;
    unsigned int failures;
} extra_task_data_t;

extra_task_data_t extra_task_data[RTAPI_MAX_TASKS + 1];


#ifdef ULAPI

int _rtapi_init(const char *modname) {
    /* do nothing for ULAPI */
    return _rtapi_next_module_id();
}

int _rtapi_exit(int module_id) {
	/* do nothing for ULAPI */
	return 0;
}

#else /* RTAPI */

int _rtapi_init(const char *modname) {
    int n, module_id = -ENOMEM;
    module_data *module;

    /* say hello */
    rtapi_print_msg(RTAPI_MSG_DBG, "RTAPI: initing module %s\n", modname);

    /* get the mutex */
    rtapi_mutex_get(&(rtapi_data->mutex));
    /* find empty spot in module array */
    n = 1;
    while ((n <= RTAPI_MAX_MODULES) && (module_array[n].state != NO_MODULE)) {
	n++;
    }
    if (n > RTAPI_MAX_MODULES) {
	/* no room */
	rtapi_mutex_give(&(rtapi_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR, "RTAPI: ERROR: reached module limit %d\n",
			n);
	return -EMFILE;
    }
    /* we have space for the module */
    module_id = n + MODULE_OFFSET;
    module = &(module_array[n]);
    /* update module data */
    module->state = REALTIME;
    if (modname != NULL) {
	/* use name supplied by caller, truncating if needed */
	rtapi_snprintf(module->name, RTAPI_NAME_LEN, "%s", modname);
    } else {
	/* make up a name */
	rtapi_snprintf(module->name, RTAPI_NAME_LEN, "ULMOD%03d", module_id);
    }
    rtapi_data->ul_module_count++;
    rtapi_print_msg(RTAPI_MSG_DBG, "RTAPI: module '%s' loaded, ID: %d\n",
	module->name, module_id);
    rtapi_mutex_give(&(rtapi_data->mutex));
    return module_id;
}

int _rtapi_exit(int module_id) {
    module_id -= MODULE_OFFSET;

    if (module_id < 0 || module_id >= RTAPI_MAX_MODULES)
	return -1;
    /* Remove the module from the module_array. */
    rtapi_mutex_get(&(rtapi_data->mutex));
    module_array[module_id].state = NO_MODULE;
    rtapi_print_msg(RTAPI_MSG_DBG,
		    "rtapi_exit: freed module slot %d, was %s\n",
		    module_id, module_array[module_id].name);
    rtapi_mutex_give(&(rtapi_data->mutex));

    return 0;
}

static inline int task_id(task_data *task) {
    return (int)(task - task_array);
}

/***********************************************************************
*                           RT thread statistics update                *
************************************************************************/
#ifdef RTAPI
int _rtapi_task_update_stats_hook(void)
{
    int task_id = _rtapi_task_self_hook();

    // paranoia
    if ((task_id < 0) || (task_id > RTAPI_MAX_TASKS)) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"_rtapi_task_update_stats_hook: BUG -"
			" task_id out of range: %d\n",
			task_id);
	return -ENOENT;
    }

    struct rusage ru;

    if (getrusage(RUSAGE_THREAD, &ru)) {
	rtapi_print_msg(RTAPI_MSG_ERR,"getrusage(): %d - %s\n",
			errno, strerror(-errno));
	return errno;
    }

    rtapi_threadstatus_t *ts = &global_data->thread_status[task_id];

    ts->flavor.rtpreempt.utime_usec = ru.ru_utime.tv_usec;
    ts->flavor.rtpreempt.utime_sec  = ru.ru_utime.tv_sec;

    ts->flavor.rtpreempt.stime_usec = ru.ru_stime.tv_usec;
    ts->flavor.rtpreempt.stime_sec  = ru.ru_stime.tv_sec;

    ts->flavor.rtpreempt.ru_minflt = ru.ru_minflt;
    ts->flavor.rtpreempt.ru_majflt = ru.ru_majflt;
    ts->flavor.rtpreempt.ru_nsignals = ru.ru_nsignals;
    ts->flavor.rtpreempt.ru_nivcsw = ru.ru_nivcsw;
    ts->flavor.rtpreempt.ru_nivcsw = ru.ru_nivcsw;

    ts->num_updates++;

    return task_id;
}
#endif

static void _rtapi_advance_time(struct timespec *tv, unsigned long ns,
			       unsigned long s) {
    ns += tv->tv_nsec;
    while (ns > 1000000000) {
	s++;
	ns -= 1000000000;
    }
    tv->tv_nsec = ns;
    tv->tv_sec += s;
}

static void rtapi_key_alloc() {
    pthread_key_create(&task_key, NULL);
}

static void rtapi_set_task(task_data *t) {
    pthread_once(&task_key_once, rtapi_key_alloc);
    pthread_setspecific(task_key, (void *)t);

    // set this thread's name so it can be identified in ps/top
    if (prctl(PR_SET_NAME, t->name) < 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"rtapi_set_task: prctl(PR_SETNAME,%s) failed: %s\n",
			t->name,strerror(errno));
    }
    // pthread_setname_np() attempts the same thing as
    // prctl(PR_SET_NAME) in a more portable way, but is
    // only available from glibc 2.12 onwards
    // pthread_t self = pthread_self();
    // pthread_setname_np(self, t->name);
}

static task_data *rtapi_this_task() {
    pthread_once(&task_key_once, rtapi_key_alloc);
    return (task_data *)pthread_getspecific(task_key);
}

int _rtapi_task_new_hook(task_data *task, int task_id) {
    void *stackaddr;

    stackaddr = malloc(task->stacksize);
    if (!stackaddr) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"Failed to allocate realtime thread stack\n");
	return -ENOMEM;
    }
    memset(stackaddr, 0, task->stacksize);
    extra_task_data[task_id].stackaddr = stackaddr;
    extra_task_data[task_id].destroyed = 0;
    return task_id;
}


void _rtapi_task_delete_hook(task_data *task, int task_id) {
    int err;
    void *returncode;

    /* Signal thread termination and wait for the thread to exit. */
    if (!extra_task_data[task_id].deleted) {
	extra_task_data[task_id].deleted = 1;
	err = pthread_join(extra_task_data[task_id].thread, &returncode);
	if (err)
	    rtapi_print_msg
		(RTAPI_MSG_ERR, "pthread_join() on realtime thread failed\n");
    }
    /* Free the thread stack. */
    free(extra_task_data[task_id].stackaddr);
    extra_task_data[task_id].stackaddr = NULL;
}

static int realtime_set_affinity(task_data *task) {
    cpu_set_t set;
    int err, cpu_nr, use_cpu = -1;

    pthread_getaffinity_np(extra_task_data[task_id(task)].thread,
			   sizeof(set), &set);
    if (task->cpu > -1) { // CPU set explicitly
	if (!CPU_ISSET(task->cpu, &set)) {
	    rtapi_print_msg(RTAPI_MSG_ERR, 
			    "RTAPI: ERROR: realtime_set_affinity(%s): "
			    "CPU %d not available\n",
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
	    rtapi_print_msg(RTAPI_MSG_ERR,
			    "Unable to get ID of the last CPU\n");
	    return -EINVAL;
	}
	rtapi_print_msg(RTAPI_MSG_DBG, "task %s: using default CPU %d\n",
			task->name, use_cpu);
    }
    CPU_ZERO(&set);
    CPU_SET(use_cpu, &set);

    err = pthread_setaffinity_np(extra_task_data[task_id(task)].thread,
				 sizeof(set), &set);
    if (err) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"%d %s: Failed to set CPU affinity to CPU %d (%s)\n",
			task_id(task), task->name, use_cpu, strerror(errno));
	return -EINVAL;
    }
    rtapi_print_msg(RTAPI_MSG_DBG,
		    "realtime_set_affinity(): task %s assigned to CPU %d\n", 
		    task->name, use_cpu);
    return 0;
}

#ifdef RTAPI
extern rtapi_exception_handler_t rt_exception_handler;
#endif

#ifndef RTAPI_POSIX
 static int realtime_set_priority(task_data *task) {
    struct sched_param schedp;

    memset(&schedp, 0, sizeof(schedp));
    schedp.sched_priority = task->prio;
    if (sched_setscheduler(0, SCHED_FIFO, &schedp)) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"Unable to set FIFO scheduling policy: %s",
			strerror(errno));
	return 1;
    }

    return 0;
}
#endif

static void *realtime_thread(void *arg) {
    task_data *task = arg;

    rtapi_set_task(task);


    if (task->period < period)
	task->period = period;
    task->ratio = task->period / period;

    extra_task_data[task_id(task)].tid = (pid_t) syscall(SYS_gettid);

    rtapi_print_msg(RTAPI_MSG_INFO,
		    "RTAPI: task '%s' at %p period = %d ratio=%d id=%d TID=%d\n",
		    task->name,
		    task, task->period, task->ratio,
		    task_id(task), extra_task_data[task_id(task)].tid);

    if (realtime_set_affinity(task))
	goto error;
#ifndef RTAPI_POSIX // This requires privs; skip it in simulator build
    if (realtime_set_priority(task))
	goto error;
#endif

    /* We're done initializing. Open the barrier. */
    pthread_barrier_wait(&extra_task_data[task_id(task)].thread_init_barrier);

    clock_gettime(CLOCK_MONOTONIC, &extra_task_data[task_id(task)].next_time);
    _rtapi_advance_time(&extra_task_data[task_id(task)].next_time,
		       task->period, 0);

    _rtapi_task_update_stats_hook(); // inital stats update

    /* The task should not pagefault at all. So record initial counts now.
     * Note that currently we _do_ receive a few pagefaults in the
     * taskcode init. This is noncritical and probably not worth
     * fixing. */
    {
	struct rusage ru;

	if (getrusage(RUSAGE_THREAD, &ru)) {
	    rtapi_print_msg(RTAPI_MSG_ERR,"getrusage(): %d - %s\n",
			    errno, strerror(-errno));
	} else {
	    rtapi_threadstatus_t *ts =
		&global_data->thread_status[task_id(task)];
	    ts->flavor.rtpreempt.startup_ru_nivcsw = ru.ru_nivcsw;
	    ts->flavor.rtpreempt.startup_ru_minflt = ru.ru_minflt;
	    ts->flavor.rtpreempt.startup_ru_majflt = ru.ru_majflt;
	}
    }

    /* call the task function with the task argument */
    task->taskcode(task->arg);

    rtapi_print_msg
	(RTAPI_MSG_ERR,
	 "ERROR: reached end of realtime thread for task %d\n",
	 task_id(task));
    extra_task_data[task_id(task)].deleted = 1;

    return NULL;
 error:
    /* Signal that we're dead and open the barrier. */
    extra_task_data[task_id(task)].deleted = 1;
    pthread_barrier_wait(&extra_task_data[task_id(task)].thread_init_barrier);
    return NULL;
}

int _rtapi_task_start_hook(task_data *task, int task_id) {
    pthread_attr_t attr;
    int retval;

    extra_task_data[task_id].deleted = 0;

    pthread_barrier_init(&extra_task_data[task_id].thread_init_barrier,
			 NULL, 2);
    pthread_attr_init(&attr);
    pthread_attr_setstack(&attr, extra_task_data[task_id].stackaddr,
			  task->stacksize);
    rtapi_print_msg(RTAPI_MSG_DBG,
		    "About to pthread_create task %d\n", task_id);
    retval = pthread_create(&extra_task_data[task_id].thread,
			    &attr, realtime_thread, (void *)task);
    rtapi_print_msg(RTAPI_MSG_DBG,"Created task %d\n", task_id);
    pthread_attr_destroy(&attr);
    if (retval) {
	pthread_barrier_destroy
	    (&extra_task_data[task_id].thread_init_barrier);
	rtapi_print_msg(RTAPI_MSG_ERR, "Failed to create realtime thread\n");
	return -ENOMEM;
    }
    /* Wait for the thread to do basic initialization. */
    pthread_barrier_wait(&extra_task_data[task_id].thread_init_barrier);
    pthread_barrier_destroy(&extra_task_data[task_id].thread_init_barrier);
    if (extra_task_data[task_id].deleted) {
	/* The thread died in the init phase. */
	rtapi_print_msg(RTAPI_MSG_ERR,
			"Realtime thread initialization failed\n");
	return -ENOMEM;
    }
    rtapi_print_msg(RTAPI_MSG_DBG,
		    "Task %d finished its basic init\n", task_id);

    return 0;
}

void _rtapi_task_stop_hook(task_data *task, int task_id) {
    extra_task_data[task_id].destroyed = 1;
}

int _rtapi_wait_hook(void) {
    struct timespec ts;
    task_data *task = rtapi_this_task();

    if (extra_task_data[task_id(task)].deleted)
	pthread_exit(0);

    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
		    &extra_task_data[task_id(task)].next_time, NULL);
    _rtapi_advance_time(&extra_task_data[task_id(task)].next_time,
		       task->period, 0);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (ts.tv_sec > extra_task_data[task_id(task)].next_time.tv_sec
	|| (ts.tv_sec == extra_task_data[task_id(task)].next_time.tv_sec
	    && ts.tv_nsec > extra_task_data[task_id(task)].next_time.tv_nsec)) {

	// timing went wrong:

	// update stats counters in thread status
	_rtapi_task_update_stats_hook();

	rtapi_threadstatus_t *ts =
	    &global_data->thread_status[task_id(task)];

	ts->flavor.rtpreempt.wait_errors++;

	rtapi_exception_detail_t detail = {0};
	detail.task_id = task_id(task);

#ifndef RTAPI_POSIX
	if (rt_exception_handler)
	    rt_exception_handler(RTP_DEADLINE_MISSED, &detail, ts);
#endif
    }
    return 0;
}

void _rtapi_delay_hook(long int nsec)
{
    struct timespec t;

    t.tv_nsec = nsec;
    t.tv_sec = 0;
    clock_nanosleep(CLOCK_MONOTONIC, 0, &t, NULL);
}


int _rtapi_task_self_hook(void) {
    int n;

    /* ask OS for pointer to its data for the current pthread */
    pthread_t self = pthread_self();

    /* find matching entry in task array */
    n = 1;
    while (n <= RTAPI_MAX_TASKS) {
	if (extra_task_data[n].thread == self) {
	    /* found a match */
	    return n;
	}
	n++;
    }
    return -EINVAL;
}

#endif /* ULAPI/RTAPI */
