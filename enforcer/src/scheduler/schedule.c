/*
 * Copyright (c) 2009 NLNet Labs. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/**
 * Task scheduling.
 * 
 * This module maintains a collection of tasks. All external functions
 * should be thread safe. Beware not to call an external function from
 * within this module, it will cause deadlocks.
 *
 * In principle the calling function should never need to lock the
 * scheduler.
 */

#include "config.h"

#include <ldns/ldns.h>
#include <pthread.h>
#include <signal.h>

#include "scheduler/schedule.h"
#include "scheduler/task.h"
#include "duration.h"
#include "log.h"

static const char* schedule_str = "scheduler";

/* Condition must be accessible from ISR */
static pthread_cond_t *schedule_cond;
static pthread_mutex_t *schedule_lock;
static int numwaiting = 0;

static task_t* get_first_task(schedule_type *schedule);

/**
 * Interrupt service routine on SIGALRM. When caught such signal one of
 * the threads waiting for a task is notified.
 */
static void*
alarm_handler(sig_atomic_t sig)
{
    pthread_t tid = pthread_self();
    switch (sig) {
        case SIGALRM:
        
            ods_log_debug("[%s] SIGALRM received", schedule_str);
            printf("[%s] SIGALRM received: %d\n", schedule_str, tid);
            pthread_mutex_lock(schedule_lock);
                pthread_cond_signal(schedule_cond);
            pthread_mutex_unlock(schedule_lock);
            break;
        default:
            ods_log_debug("[%s] Spurious signal %d received", 
                schedule_str, (int)sig);
    }
    return NULL;
}

/**
 * Inspect head of queue and wakeup a worker now or set alarm.
 * Caller SHOULD hold schedule->schedule_lock. Failing to do so
 * could possibly cause a thread to miss the wakeup.
 */
static void
set_alarm(schedule_type* schedule)
{
    time_t now = time_now();
    task_t *task = get_first_task(schedule);
    if (!task) {
        ods_log_debug("[%s] no alarm set", schedule_str);
    } else if (task->due_date <= now) {
        ods_log_debug("[%s] signal now", schedule_str);
        pthread_cond_signal(&schedule->schedule_cond);
    } else {
        ods_log_debug("[%s] SIGALRM set", schedule_str);
        alarm(task->due_date - now);
    }
}

/**
 * Convert task to a tree node.
 * NULL on malloc failure
 */
static ldns_rbnode_t*
task2node(task_t *task)
{
    ldns_rbnode_t* node = (ldns_rbnode_t*) malloc(sizeof(ldns_rbnode_t));
    if (node) {
        node->key = task;
        node->data = task;
    }
    return node;
}

/**
 * Get the first scheduled task. As long as return value is used
 * caller should hold schedule->schedule_lock.
 * 
 * \param[in] schedule schedule
 * \return task_type* first scheduled task, NULL on no task or error.
 */
static task_t*
get_first_task(schedule_type* schedule)
{
    ldns_rbnode_t* first_node;

    if (!schedule || !schedule->tasks) return NULL;
    first_node = ldns_rbtree_first(schedule->tasks);
    if (!first_node) return NULL;
    return (task_t*) first_node->data;
}

/**
 * pop the first scheduled task. Caller must hold
 * schedule->schedule_lock. Result is safe to use outside lock.
 * 
 * \param[in] schedule schedule
 * \return task_type* first scheduled task, NULL on no task or error.
 */
static task_t*
pop_first_task(schedule_type* schedule)
{
    ldns_rbnode_t *node, *delnode;
    task_t *task;

    if (!schedule || !schedule->tasks) return NULL;
    node = ldns_rbtree_first(schedule->tasks);
    if (!node) return NULL;
    delnode = ldns_rbtree_delete(schedule->tasks, node->data);
    /* delnode == node, but we don't free it just yet, data is shared
     * with tasks_by_name tree */
    if (!delnode) return NULL;
    delnode = ldns_rbtree_delete(schedule->tasks_by_name, node->data);
    free(node);
    if (!delnode) return NULL;
    task = (task_t*) delnode->data;
    free(delnode); /* this delnode != node */
    set_alarm(schedule);
    return task;
}

/**
 * Internal task cleanup function.
 *
 */
static void
task_delfunc(ldns_rbnode_t* node)
{
    task_t *task;

    if (node && node != LDNS_RBTREE_NULL) {
        task = (task_t*) node->data;
        task_delfunc(node->left);
        task_delfunc(node->right);
        task_deepfree(task);
        free(node);
    }
}

/**
 * Create new schedule. Allocate and initialise scheduler. To clean
 * up schedule_cleanup() should be called.
 */
schedule_type*
schedule_create()
{
    schedule_type* schedule;
    struct sigaction action;
    pthread_mutexattr_t mtx_attr;

    schedule = (schedule_type*) malloc(sizeof(schedule_type));
    if (!schedule) {
        ods_log_error("[%s] unable to create: malloc failed", schedule_str);
        return NULL;
    }

    schedule->tasks = ldns_rbtree_create(task_compare_time_then_ttuple);
    schedule->tasks_by_name = ldns_rbtree_create(task_compare_ttuple);
    schedule->locks_by_name = ldns_rbtree_create(task_compare_ttuple);

    /* The reason we allow recursive locks is because we broadcast
     * a condition in an ISR. The main thread would receive this
     * interrupt. If we would do no locking in the ISR we risk missing
     * conditions. If we use a normal mutex we risk a deadlock if the
     * main thread occasionally grabs the lock. */
    (void)pthread_mutexattr_init(&mtx_attr);
    (void)pthread_mutexattr_settype(&mtx_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&schedule->schedule_lock, &mtx_attr);
    
    pthread_cond_init(&schedule->schedule_cond, NULL);
    /* static condition for alarm. Must be accessible from interrupt */
    schedule_cond = &schedule->schedule_cond;
    schedule_lock = &schedule->schedule_lock;
    schedule->num_waiting = 0;

    action.sa_handler = (void (*)(int))&alarm_handler;
    sigfillset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGALRM, &action, NULL);
    
    return schedule;
}

/**
 * Clean up schedule. deinitialise and free scheduler.
 * Threads MUST be stopped before calling this function.
 */
void
schedule_cleanup(schedule_type* schedule)
{
    if (!schedule) return;
    ods_log_debug("[%s] cleanup schedule", schedule_str);

    /* Disable any pending alarm before we destroy the pthread stuff
     * to prevent segfaults */
    alarm(0);
    
    if (schedule->tasks) {
        task_delfunc(schedule->tasks->root);
        task_delfunc(schedule->tasks_by_name->root);
        ldns_rbtree_free(schedule->tasks);
        ldns_rbtree_free(schedule->tasks_by_name);
        schedule->tasks = NULL;
    }
    pthread_mutex_destroy(&schedule->schedule_lock);
    pthread_cond_destroy(&schedule->schedule_cond);
    free(schedule);
}

/**
 * exported convenience functions should all be thread safe
 */

time_t
schedule_time_first(schedule_type* schedule)
{
    task_t *task;
    time_t time;
    
    if (!schedule || !schedule->tasks) return -1;

    pthread_mutex_lock(&schedule->schedule_lock);
        task = get_first_task(schedule);
        if (!task)
            time = -1;
        else 
            time = task->due_date;
    pthread_mutex_unlock(&schedule->schedule_lock);
    return time;
}

size_t
schedule_taskcount(schedule_type* schedule)
{
    if (!schedule || !schedule->tasks) return 0;
    return schedule->tasks->count;
}

void
schedule_flush(schedule_type* schedule)
{
    ldns_rbnode_t *node;
    
    ods_log_debug("[%s] flush all tasks", schedule_str);
    if (!schedule || !schedule->tasks) return;

    pthread_mutex_lock(&schedule->schedule_lock);
        node = ldns_rbtree_first(schedule->tasks);
        while (node && node != LDNS_RBTREE_NULL) {
            ((task_t*) node->data)->due_date = 0;
            node = ldns_rbtree_next(node);
        }
        /* wakeup! work to do! */
        pthread_cond_signal(&schedule->schedule_cond);
    pthread_mutex_unlock(&schedule->schedule_lock);
}

int
schedule_flush_type(schedule_type* schedule, char const *class, char const *type)
{
    ldns_rbnode_t *node, *nextnode;
    int nflushed = 0;
    
    ods_log_debug("[%s] flush task", schedule_str);
    if (!schedule || !schedule->tasks) return 0;

    pthread_mutex_lock(&schedule->schedule_lock);
        node = ldns_rbtree_first(schedule->tasks);
        while (node && node != LDNS_RBTREE_NULL) {
            nextnode = ldns_rbtree_next(node);
            if (node->data && ((task_t*)node->data)->type == type
                && ((task_t*)node->data)->class == class)
            {
                /* Merely setting flush is not enough. We must set it
                 * to the front of the queue as well. */
                node = ldns_rbtree_delete(schedule->tasks, node->data);
                if (!node) break; /* strange, bail out */
                if (node->data) { /* task */
                    ((task_t*)node->data)->due_date = 0;
                    if (!ldns_rbtree_insert(schedule->tasks, node)) {
                        ods_log_crit("[%s] Could not reschedule task "
                            "after flush. A task has been lost!",
                            schedule_str);
                        free(node);
                        /* Do not free node->data it is still in use
                         * by the other rbtree. */
                        break;
                    }
                    nflushed++;
                }
            }
            node = nextnode;
        }
        /* wakeup! work to do! */
        pthread_cond_signal(&schedule->schedule_cond);
    pthread_mutex_unlock(&schedule->schedule_lock);
    return nflushed;
}

void
schedule_purge(schedule_type* schedule)
{
    ldns_rbnode_t* node;
    
    if (!schedule || !schedule->tasks) return;

    pthread_mutex_lock(&schedule->schedule_lock);
        /* don't attempt to free payload, still referenced by other tree*/
        while ((node = ldns_rbtree_first(schedule->tasks)) !=
            LDNS_RBTREE_NULL)
        {
            node = ldns_rbtree_delete(schedule->tasks, node->data);
            if (node == 0) break;
            free(node);
        }
        /* also clean up name tree */
        while ((node = ldns_rbtree_first(schedule->tasks_by_name)) !=
            LDNS_RBTREE_NULL)
        {
            node = ldns_rbtree_delete(schedule->tasks_by_name, node->data);
            if (node == 0) break;
            task_deepfree((task_t*) node->data);
            free(node);
        }
        /* also clean up locks tree */
        while ((node = ldns_rbtree_first(schedule->locks_by_name)) !=
            LDNS_RBTREE_NULL)
        {
            node = ldns_rbtree_delete(schedule->locks_by_name, node->data);
            if (node == 0) break;
            pthread_mutex_destroy(((task_t*) node->data)->lock);
            free(((task_t*) node->data)->lock);
            task_deepfree((task_t*) node->data);
            free(node);
        }
    pthread_mutex_unlock(&schedule->schedule_lock);
}

int
schedule_get_num_waiting(schedule_type* schedule)
{
    int num_waiting;

    pthread_mutex_lock(&schedule->schedule_lock);
        num_waiting = schedule->num_waiting;
    pthread_mutex_unlock(&schedule->schedule_lock);

    return num_waiting;
}

task_t*
schedule_pop_task(schedule_type* schedule)
{
    time_t now = time_now();
    task_t *task;

    pthread_mutex_lock(&schedule->schedule_lock);
        task = get_first_task(schedule);
        if (!task || task->due_date > now) {
            /* nothing to do now, sleep and wait for signal */
            schedule->num_waiting += 1;
            pthread_cond_wait(&schedule->schedule_cond,
                &schedule->schedule_lock);
            schedule->num_waiting -= 1;
            task = NULL;
        } else {
            task = pop_first_task(schedule);
        }
    pthread_mutex_unlock(&schedule->schedule_lock);
    return task;
}

task_t*
schedule_pop_first_task(schedule_type* schedule)
{
    task_t *task;

    pthread_mutex_lock(&schedule->schedule_lock);
        task = pop_first_task(schedule);
    pthread_mutex_unlock(&schedule->schedule_lock);
    return task;
}

/* Removes task from both trees and assign nodes to node1 and node2.
 * These belong to the caller now
 * 
 * 0 on success */
static int
remove_node_pair(schedule_type *schedule, task_t *task,
    ldns_rbnode_t **node1, ldns_rbnode_t **node2)
{
    ldns_rbnode_t *n1, *n2;
    task_t *t;

    ods_log_assert(schedule);
    ods_log_assert(task);
    
    n2 = ldns_rbtree_delete(schedule->tasks_by_name, task);
    if (!n2) return 1; /* could not find task*/
    t = (task_t*)n2->key; /* This is the original task, it has the
                             correct time so we can find it in tasks */
    ods_log_assert(t);
    n1 = ldns_rbtree_delete(schedule->tasks, t);
    ods_log_assert(n1);
    *node1 = n1;
    *node2 = n2;
    return 0;
}

void
schedule_purge_owner(schedule_type* schedule, char const *class,
    char const *owner)
{
    /* This method is somewhat inefficient but not too bad. Approx:
     * O(N + M log N). Where N total tasks, M tasks to remove. Probably
     * a bit worse since the trees are balanced. */
    task_t **tasks, *task;
    int i, num_slots = 10, num_tasks = 0;
    ldns_rbnode_t *n1, *n2, *node;

    /* We expect around 3 tasks per owner so we probably never have to
     * realloc if we start with num_slots = 10 */
    tasks = (task_t **)malloc(num_slots * sizeof(task_t *));
    if (!tasks) return;

    pthread_mutex_lock(&schedule->schedule_lock);

        /* First collect all tasks that match. Don't fiddle with the
         * tree. That is not save and might mess up our iteration. */
        node = ldns_rbtree_first(schedule->tasks_by_name);
        while (node != LDNS_RBTREE_NULL) {
            task = node->key;
            node = ldns_rbtree_next(node);
            if (!strcmp(task->owner, owner) && !strcmp(task->class, class)) {
                tasks[num_tasks++] = task;
                if (num_tasks == num_slots) {
                    num_slots *= 2;
                    tasks = realloc(tasks, num_slots * sizeof(task_t *));
                    if (!tasks) {
                        pthread_mutex_unlock(&schedule->schedule_lock);
                        return;
                    }
                }
            }
        }

        /* Be free my little tasks, be free! */
        for (i = 0; i<num_tasks; i++) {
            if (!remove_node_pair(schedule, tasks[i], &n1, &n2)) {
                task_deepfree(tasks[i]);
                free(n1);
                free(n2);
            }
        }
        free(tasks);

    pthread_mutex_unlock(&schedule->schedule_lock);
}

ods_status
schedule_task(schedule_type *schedule, task_t *task)
{
    ldns_rbnode_t *node1, *node2;
    task_t *existing_task, *t;

    ods_log_assert(task);

    if (!schedule || !schedule->tasks) {
        ods_log_error("[%s] unable to schedule task: no schedule",
            schedule_str);
        return ODS_STATUS_ERR;
    }

    ods_log_debug("[%s] schedule task [%s] for %s", schedule_str,
        task->type, task->owner);

    pthread_mutex_lock(&schedule->schedule_lock);
    if (remove_node_pair(schedule, task, &node1, &node2)) {
        /* Though no such task is scheduled at the moment, there could
         * be a lock for it. If task already has a lock, keep using that.
         */
        if (!task->lock) {
            node1 = ldns_rbtree_search(schedule->locks_by_name, task);
            if (!node1) {
                /* New lock, insert in tree */
                t = task_duplicate_shallow(task);
                t->lock = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t));
                if (pthread_mutex_init(t->lock, NULL)) {
                    task_deepfree(t);
                    pthread_mutex_unlock(&schedule->schedule_lock);
                    return ODS_STATUS_ERR;
                }
                node1 = task2node(t);
                ods_log_assert(ldns_rbtree_insert(schedule->locks_by_name, node1));
            }
            task->lock = ((task_t *)node1->key)->lock;
        }
        /* not is schedule yet */
        node1 = task2node(task);
        node2 = task2node(task);
        if (!node1 || !node2) {
            pthread_mutex_unlock(&schedule->schedule_lock);
            free(node1);
            free(node2);
            return ODS_STATUS_ERR;
        }
        ods_log_assert(ldns_rbtree_insert(schedule->tasks, node1));
        ods_log_assert(ldns_rbtree_insert(schedule->tasks_by_name, node2));
    } else {
        ods_log_assert(node1->key == node2->key);
        existing_task = (task_t*)node1->key;
        if (task->due_date < existing_task->due_date)
            existing_task->due_date = task->due_date;
        if (existing_task->free_context) {
            existing_task->free_context(existing_task->context);
        }
        existing_task->context = task->context;
        existing_task->free_context = task->free_context;
        task->context = NULL; /* context is now assigned to
            existing_task, prevent it from freeing */
        task_deepfree(task);
        ods_log_assert(ldns_rbtree_insert(schedule->tasks, node1));
        ods_log_assert(ldns_rbtree_insert(schedule->tasks_by_name, node2));
    }
    set_alarm(schedule);
    pthread_mutex_unlock(&schedule->schedule_lock);
    return ODS_STATUS_OK;
}

void
schedule_release_all(schedule_type* schedule)
{
    pthread_mutex_lock(&schedule->schedule_lock);
        pthread_cond_broadcast(&schedule->schedule_cond);
    pthread_mutex_unlock(&schedule->schedule_lock);
}
