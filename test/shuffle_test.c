#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <sched.h> // For setting thread affinity


static void shuffle_elements(void *array, size_t size, unsigned int *seed) {
    uint32_t *arr = (uint32_t *)array;
    size_t n = size / sizeof(uint32_t);
    if (n > 1) {
        for (size_t i = n - 1; i > 0; i--) {
            size_t j = rand_r(seed) % (i + 1);
            // Swap arr[i] and arr[j]
            uint32_t tmp = arr[i];
            arr[i] = arr[j];
            arr[j] = tmp;
        }
    }
}

typedef struct {
    uint32_t *array;
    size_t size;
    unsigned int seed;
} ThreadData;

void *thread_func(void *arg) {
    ThreadData *data = (ThreadData *)arg;
    shuffle_elements(data->array, data->size, &data->seed);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s num_threads total_size\n", argv[0]);
        return 1;
    }

    int num_threads = atoi(argv[1]);
    size_t total_size = atoi(argv[2]);

    if (total_size % sizeof(uint32_t) != 0) {
        fprintf(stderr, "Total size must be a multiple of %lu\n", sizeof(uint32_t));
        return 1;
    }

    uint32_t *buffer = malloc(total_size);
    if (!buffer) {
        perror("Failed to allocate memory");
        return 1;
    }

    pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
    ThreadData *thread_data = malloc(num_threads * sizeof(ThreadData));
    size_t chunk_size = total_size / num_threads;

    cpu_set_t cpuset; // CPU set to bind thread to specific core

    // Start timing
    struct timespec start, end;

    for (int i = 0; i < num_threads; i++) {
        thread_data[i].array = buffer + (i * chunk_size / sizeof(uint32_t));
        thread_data[i].size = chunk_size;
        thread_data[i].seed = time(NULL) + i; // Unique seed for each thread
        
        pthread_create(&threads[i], NULL, thread_func, &thread_data[i]);

        // Set affinity to bind each thread to a different core
        CPU_ZERO(&cpuset);
        CPU_SET(i % num_threads, &cpuset); // This assumes you have at least as many cores as threads
        pthread_setaffinity_np(threads[i], sizeof(cpu_set_t), &cpuset);
    }

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    // End timing
    clock_gettime(CLOCK_MONOTONIC, &end);
    double time_taken = end.tv_sec - start.tv_sec;
    time_taken += (end.tv_nsec - start.tv_nsec) / 1000000000.0;
    
    printf("Time taken for %d threads: %.5f seconds\n", num_threads, time_taken);

    // Cleanup
    free(buffer);
    free(threads);
    free(thread_data);
    return 0;
}
