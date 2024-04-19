#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include "iaa.h"

#define MIN_SIZE 1024
#define MAX_SIZE (1024 * 128)
#define GRANULARITY 1

struct acctest_context * iaa;
int num_desc = 100000;

// To compile
// gcc pointer_chase.c -o pointer_chase -lm -lpthread

volatile int keep_running = 1;

void shuffle_indices(size_t* indices, size_t len) {
    for (size_t i = 0; i < len - 1; i++) {
        size_t j = i + rand() / (RAND_MAX / (len - i) + 1);
        size_t temp = indices[i];
        indices[i] = indices[j];
        indices[j] = temp;
    }
}

void** create_random_chain(size_t size) {
    size_t len = size / sizeof(void*);
    void** memory = malloc(len * sizeof(void*));
    if (!memory) {
        fprintf(stderr, "Failed to allocate memory for pointers\n");
        return NULL;
    }

    size_t* indices = malloc(len * sizeof(size_t));
    if (!indices) {
        fprintf(stderr, "Failed to allocate memory for indices\n");
        free(memory);
        return NULL;
    }

    for (size_t i = 0; i < len; i++) {
        indices[i] = i;
    }

    shuffle_indices(indices, len);

    for (size_t i = 1; i < len; i++) {
        memory[indices[i - 1]] = &memory[indices[i]];
    }
    memory[indices[len - 1]] = &memory[indices[0]];

    free(indices);
    return memory;
}

void chase_pointers(void** memory, size_t count) {
    void* ptr = memory[0];
    for (size_t i = 0; i < count; i++) {
        ptr = *(void**)ptr;
    }
}

void* pointer_chase_thread(void* arg) {
    struct timespec times[2];
    size_t memsize = *(size_t*)arg;
    void** memory = create_random_chain(memsize);
    size_t count = fmax(memsize * 16, (size_t)1<<30);
    printf("Starting chase pointers\n");

    while(1){
        clock_gettime(CLOCK_MONOTONIC, &times[0]);
        chase_pointers(memory, count);
        clock_gettime(CLOCK_MONOTONIC, &times[1]);
        uint64_t nanos = (times[1].tv_sec - times[0].tv_sec) * 1000000000 + times[1].tv_nsec - times[0].tv_nsec;
        printf("nanos: %u\n", nanos);
    }

    keep_running = 0;
    return NULL;
}

void* busy_poll_thread(void* arg) {
    while (keep_running) {
        // Busy polling operation
    }
    printf("Busy polling thread exiting...\n");
    return NULL;
}

void *wait_for_iaa(void *arg) {
    struct task_node *iaa_tsk_node = iaa->multi_task_node;
    int rc;

    // while (1){
        iaa_tsk_node = iaa->multi_task_node;
        while(iaa_tsk_node) {
            // printf("Waiting...\n");
            rc = iaa_wait_compress(iaa, iaa_tsk_node->tsk);
            if (rc != ACCTEST_STATUS_OK)
                pthread_exit((void *)(intptr_t)rc);
            iaa_tsk_node = iaa_tsk_node->next;
        }
    // }

    pthread_exit((void *)ACCTEST_STATUS_OK);
}

void feed_iaa(void *arg){
    int buf_size = 4096;
    struct task_node *iaa_tsk_node;
	int rc = ACCTEST_STATUS_OK;
	int tflags = 0x1;
    int wq_type = SHARED;
    int wq_id = 0;
    int dev_id = 3;
    
    iaa = acctest_init(tflags);
    iaa->dev_type = ACCFG_DEVICE_IAX;
    if (!iaa)
        return -ENOMEM;
    rc = acctest_alloc(iaa, wq_type, dev_id, wq_id);
    if (rc < 0)
            return -ENOMEM;

    rc = acctest_alloc_multiple_tasks(iaa, num_desc);
    if (rc != ACCTEST_STATUS_OK)
        return rc;
    printf("Allocated tasks\n");

    iaa_tsk_node = iaa->multi_task_node;
	while (iaa_tsk_node) {
		iaa_tsk_node->tsk->iaa_compr_flags = 0;
		rc = iaa_init_task(iaa_tsk_node->tsk, tflags, IAX_OPCODE_COMPRESS, buf_size);
		if (rc != ACCTEST_STATUS_OK)
			return rc;
		iaa_tsk_node = iaa_tsk_node->next;
	}
    printf("Starting test\n");

    // while(1){

        iaa_tsk_node = iaa->multi_task_node;
        while(iaa_tsk_node){
            iaa_prep_sub_task_node(iaa, iaa_tsk_node);
            iaa_tsk_node = iaa_tsk_node->next;
        }
        printf("Done\n");
    // }
}



int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <mode>\n", argv[0]);
        fprintf(stderr, "mode 1: Only Pointer Chase\nmode 2: Pointer Chase with Busy Polling\n");
        return 1;
    }

    int mode = atoi(argv[1]);
    pthread_t chase_thread, poll_thread, iaa_feed;
    size_t memsize = MAX_SIZE;
    pthread_attr_t attr;
    cpu_set_t cpus;

    pthread_attr_init(&attr);
    CPU_ZERO(&cpus);
    CPU_SET(41, &cpus);
    pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpus);
    pthread_create(&chase_thread, &attr, pointer_chase_thread, &memsize);
    
    if (mode == 2) {
        pthread_attr_init(&attr);
        CPU_ZERO(&cpus);
        CPU_SET(10, &cpus);
        pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpus);
        pthread_create(&iaa_feed, &attr, feed_iaa, NULL);

        pthread_attr_init(&attr);
        CPU_ZERO(&cpus);
        CPU_SET(1, &cpus);
        pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpus);
        pthread_create(&poll_thread, &attr, wait_for_iaa, NULL);
        pthread_join(poll_thread, NULL);
        pthread_join(iaa_feed, NULL);
    }

    pthread_join(chase_thread, NULL);

    return 0;
}
