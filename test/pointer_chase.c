#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#define MIN_SIZE 1024
#define MAX_SIZE (1024 * 128)
#define GRANULARITY 1

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

double chase_pointers(void** memory, size_t count) {
    clock_t start, end;
    start = clock();
    void* ptr = memory[0];
    for (size_t i = 0; i < count; i++) {
        ptr = *(void**)ptr;
    }
    end = clock();
    return (double)(end - start) / CLOCKS_PER_SEC;
}

void* pointer_chase_thread(void* arg) {
    size_t memsize = *(size_t*)arg;
    void** memory = create_random_chain(memsize);
    size_t count = fmax(memsize * 16, (size_t)1<<30);
    printf("Starting chase pointers\n");
    double t = chase_pointers(memory, count);
    free(memory);
    double ns = t * 1000000000 / count;
    printf(" %9zu  %10.5lf ns\n", memsize, ns);
    fflush(stdout);

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

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <mode>\n", argv[0]);
        fprintf(stderr, "mode 1: Only Pointer Chase\nmode 2: Pointer Chase with Busy Polling\n");
        return 1;
    }

    int mode = atoi(argv[1]);
    pthread_t chase_thread, poll_thread;
    size_t memsize = MAX_SIZE;  // Define this appropriately
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
        CPU_SET(1, &cpus);
        pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpus);
        pthread_create(&poll_thread, &attr, busy_poll_thread, NULL);
        pthread_join(poll_thread, NULL);
    }

    pthread_join(chase_thread, NULL);

    return 0;
}
