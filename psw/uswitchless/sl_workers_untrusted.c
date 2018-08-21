/*
 * Copyright (C) 2011-2018 Intel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <sl_workers.h>
#include <sl_fcall_mngr_common.h>
#include <sl_uswitchless.h>
#include <sl_debug.h>
#include <sl_atomic.h>
#include <internal/rts_cmd.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/syscall.h>   /* For SYS_xxx definitions */
//#include <linux/futex.h>
#include <limits.h>

/*=========================================================================
 * Initialization and Destroyment of Workers
 *========================================================================*/

int sl_workers_init(struct sl_workers* workers,
                    sl_worker_type_t type,
                    struct sl_uswitchless* handle)
{
    memset(workers, 0, sizeof(*workers));
    workers->handle = handle;
    workers->type = type;

    uint32_t num_workers = type == SL_WORKER_TYPE_UNTRUSTED ?
                                    handle->us_config.num_uworkers :
                                    handle->us_config.num_tworkers ;
    workers->num_all = num_workers;

    workers->threads = (pthread_t*)calloc(num_workers, sizeof(pthread_t));
    if (workers->threads == NULL) return -ENOMEM;

    return 0;
}

void sl_workers_destroy(struct sl_workers* workers) 
{
    BUG_ON(workers->num_running > 0);
    free(workers->threads);
}

/*=========================================================================
 * Sleep and wakeup threads
 *========================================================================*/

#if 0
static inline long futex(volatile int32_t* futex_addr, int32_t futex_op, int32_t futex_val) 
{
    return syscall(__NR_futex, futex_addr, futex_op, futex_val, NULL, NULL, 0);
}
#endif

void sleep_this_thread(struct sl_workers* workers) 
{
   lock_inc64(&workers->num_sleeping);
   //futex(&workers->should_wake, FUTEX_WAIT, 0);
   lock_dec64(&workers->num_sleeping);
}

void wake_all_threads(struct sl_workers* workers)
{
	uint32_t *addr;

	addr = (uint32_t *)&workers->should_wake;

    //futex(&workers->should_wake, FUTEX_WAKE, INT_MAX);
}


/*=========================================================================
 * Thread Management of Workers
 *========================================================================*/

typedef int(*process_calls_func_t)(struct sl_workers* workers);

static int tworker_process_calls(struct sl_workers* workers);
static int uworker_process_calls(struct sl_workers* workers);

static inline process_calls_func_t get_process_calls_fn(sl_worker_type_t type) {
    return (type == SL_WORKER_TYPE_UNTRUSTED) ? uworker_process_calls :
                                                tworker_process_calls ;
}


static void* run_worker(void* thread_data) 
{
    struct sl_workers* workers = (struct sl_workers*)thread_data;
    process_calls_func_t process_calls_fn = get_process_calls_fn(workers->type);
    lock_inc64(&workers->num_running);

    /* Start worker thread */
    sl_workers_notify_event(workers, SL_WORKER_EVENT_START);

    /* After creation, the worker thread will sleep until the first
     * user-defined ECall. There are two reasons for this sleep-after-creation
     * behaviour. The first reason is that workers have nothing to do before
     * users call an ECall on the enclave. So, this can save some CPU cycles.
     * The second reason, which is actually more important, is about the OCall
     * table of an enclave, whose address cannot be known until an user-defined,
     * EDL-generated ECall is called upon the enclave. This OCall table must be
     * given to trusted or untrusted workers so that they can function properly.
     * */
    sleep_this_thread(workers);
    BUG_ON(workers->handle->us_ocall_table == NULL);
        
    /* Main loop of worker thread */
    while (!workers->should_stop)
	{
        /* Process calls until idle for some time */
        process_calls_fn(workers);
        /* Notify idle event */
        sl_workers_notify_event(workers, SL_WORKER_EVENT_IDLE);
        sleep_this_thread(workers);
    }
    
    /* Exit worker thread */
    sl_workers_notify_event(workers, SL_WORKER_EVENT_EXIT);
    lock_dec64(&workers->num_running);
    return NULL;
}

int sl_workers_init_threads(struct sl_workers* workers)
{
    int ret = 0;
    uint32_t num_started = 0, ti = 0;
    for (; num_started < workers->num_all; num_started++)
	{
        ret = pthread_create(&workers->threads[num_started], NULL,
                             run_worker, (void*)workers);
        if (ret) goto on_error;
    }
    return 0;
on_error:
    workers->should_stop = 1;
    for (; ti < num_started; ti++)
        pthread_join(workers->threads[ti], NULL);
    return ret;
}

int sl_workers_run_threads(struct sl_workers* workers) 
{
    wake_all_threads(workers);
    return 0;
}

void sl_workers_kill_threads(struct sl_workers* workers) 
{
    uint32_t ti = 0;
    workers->should_stop = 1;
    workers->should_wake = 1;
    wake_all_threads(workers);
    for (; ti < workers->num_all; ti++)
    {
        pthread_join(workers->threads[ti], NULL);
    }
}

/*=========================================================================
 * Event Callbacks of Workers
 *========================================================================*/

void sl_workers_notify_event(struct sl_workers* workers,
                             sl_worker_event_t event)
{
    sl_config_t* config = &workers->handle->us_config;

    sl_worker_callback_t callback = config->callback_func[event];
    if (callback == NULL) return;

    callback(workers->type, event, &workers->stats);
}

/*=========================================================================
 * Process calls by trusted workers
 *========================================================================*/

static int tworker_process_calls(struct sl_workers* workers) 
{
    BUG_ON(workers->handle->us_ocall_table == NULL);
    struct sl_uswitchless* handle = workers->handle;
    sgx_ecall(handle->us_enclave_id, ECMD_RUN_SWITCHLESS_TWORKER,
              handle->us_ocall_table, NULL);
    return 0;
}

/*=========================================================================
 * Process calls by untrusted workers
 *========================================================================*/

static int uworker_process_calls(struct sl_workers* workers) 
{
    struct sl_uswitchless* handle = workers->handle;
    struct sl_fcall_mngr* focall_mngr = &handle->us_focall_mngr;

	uint32_t max_retries = handle->us_config.retries_before_sleep;
	uint32_t retries = 0;
	
	while (retries < max_retries)
	{
		if (sl_fcall_mngr_process(focall_mngr) == 0)
		{
			asm_pause();
			retries++;
		}
		else
		{
			retries = 0;
		}
	}

    /* Idle for some time */
    return 0;
}

