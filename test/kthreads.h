#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/user.h>
#include <sys/mman.h>
#include <linux/mman.h>
#include "accel_test.h"
#include "dsa.h"
#include "iaa.h"
#include "algorithms/iaa_filter.h"
#include "util.h"
#include <pthread.h>
#include <pth.h>
#define DSA_TEST_SIZE 16384
#define IAA_COMPRESS_AECS_SIZE (1568)
#define IAA_COMPRESS_SRC2_SIZE (IAA_COMPRESS_AECS_SIZE * 2)
#define IAA_COMPRESS_MAX_DEST_SIZE (2097152 * 2)

_Atomic int finalHostOpCtr = 0;
_Atomic int expectedHostOps = 0;
_Atomic int complete = 0;
_Atomic int host_op_sel = 0;
_Atomic int do_spt_spinup = 0;

static unsigned long buf_size = DSA_TEST_SIZE;

static struct acctest_context *dsa, *iaa;

typedef struct {
    void *buffer;
    size_t size;
    size_t count;  // Result from host_op
		int (*host_op)(void *buffer, size_t size);
} host_op_args;


typedef struct {
	int dep;
	host_op_args *args;
} cb_event;


// Function prototypes
void *memcpy_and_submit(void *arg);
void *wait_for_iaa(void *arg);
void *dsa_submit(void *arg);
void *host_operation_thread(void *arg);
int shuffle_host_op(void *buffer, size_t size);
int host_op(void *buffer, size_t size);

int (*select_host_op(int host_op_sel))(void *buffer, size_t size){
	switch(host_op_sel){
		case 0:
			return host_op;
		case 1:
			return shuffle_host_op;
		default:
			return host_op;
	}
}

int shuffle_host_op(void *buffer, size_t size){
	shuffle_elements(buffer, size);
	return 1;
}

int host_op(void *buffer, size_t size) {
	uint32_t *ptr;
	size_t count;
	size_t num_elements;
    if (buffer == NULL || size % sizeof(uint32_t) != 0) {
        return -1;
    }

    ptr = (uint32_t *) buffer;
    count = 0;
    num_elements = size / sizeof(uint32_t);

    for (size_t i = 0; i < num_elements; ++i) {
        if (ptr[i] >= 10000) {
            count++;
        }
    }
    return count;
}

void *dsa_submit(void *arg) {
	int rc = 0;
	rc = dsa_memcpy_submit_task_nodes(dsa);
	if (rc != ACCTEST_STATUS_OK)
		pthread_exit((void *)(intptr_t)rc);
	pthread_exit((void *)ACCTEST_STATUS_OK);
}


void *host_operation_thread(void *arg) {
		host_op_args *args = (host_op_args *) arg;
    args->count = args->host_op(args->buffer, args->size);  // Store the result in the structure
		// printf("Count is : %d\n", count);
		finalHostOpCtr += 1;
		if(finalHostOpCtr == expectedHostOps){
			complete = 1;
		}
    return NULL;  // Return nothing as the result is stored in the passed structure
}


void *app_worker_thread(void *arg){
	while(1){
		cb_event *event = (cb_event *)arg;
		if(event->dep == 0){
			host_operation_thread(event->args);
		}
	}
}

void *memcpy_and_submit(void *arg) {
    struct task_node *dsa_tsk_node, *iaa_tsk_node;
    int rc;
	host_op_args *args;
	pthread_t host_thread;
	pth_t pth;
	pth_attr_t attr;
	pth_init();
	attr = pth_attr_new();
	pth_attr_set(attr, PTH_ATTR_NAME, "host_op_thread");
	pth_attr_set(attr, PTH_ATTR_STACK_SIZE, 64*1024);
	pth_attr_set(attr, PTH_ATTR_JOINABLE, TRUE);

    dsa_tsk_node = dsa->multi_task_node;
    iaa_tsk_node = iaa->multi_task_node;

	while (dsa_tsk_node) {
			rc = dsa_wait_memcpy(dsa, dsa_tsk_node->tsk);
			if (rc != ACCTEST_STATUS_OK)
					pthread_exit((void *)(intptr_t)rc);

			args = malloc(sizeof(host_op_args));
			if (args == NULL) {
					pthread_exit((void *)(intptr_t)ENOMEM);  // Handle memory allocation failure
			}
			int *(*selected_op)(void *buffer, size_t size) = select_host_op(host_op_sel);
			if(do_spt_spinup){
				pth_t pth;
				args->host_op = selected_op;
				args->buffer = dsa_tsk_node->tsk->dst1;
				args->size = buf_size;
				// Create a thread to perform the host operation
				if (pth_spawn(attr, host_operation_thread, args) == NULL) {
						printf("Error creating host op thread\n");
						free(args);  // Clean up if thread creation fails
						exit(-1);
				}
				pth_join(pth, NULL);
			} else {
				selected_op(dsa_tsk_node->tsk->dst1, buf_size);
				// no atomic ctr update from thread -- do it here
				finalHostOpCtr += 1;
				if(finalHostOpCtr == expectedHostOps){
					complete = 1;
				}
			}

        // Continue with other operations
        iaa_tsk_node->tsk->src1 = dsa_tsk_node->tsk->dst1;
        iaa_prep_sub_task_node(iaa, iaa_tsk_node);

        dsa_tsk_node = dsa_tsk_node->next;
        iaa_tsk_node = iaa_tsk_node->next;
    }

    pthread_exit((void *)ACCTEST_STATUS_OK);
}

void *wait_for_iaa(void *arg) {
    struct task_node *iaa_tsk_node = iaa->multi_task_node;
    int rc;
	// int itr = 0;

    while(iaa_tsk_node) {
		// printf("Wait for IAA itr: %d\n", itr++);
        rc = iaa_wait_compress(iaa, iaa_tsk_node->tsk);
        if (rc != ACCTEST_STATUS_OK)
            pthread_exit((void *)(intptr_t)rc);
        iaa_tsk_node = iaa_tsk_node->next;
    }

    pthread_exit((void *)ACCTEST_STATUS_OK);
}