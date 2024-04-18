#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

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
    size_t* indices = malloc(len * sizeof(size_t));

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

unsigned int log2_custom(size_t val) {
    unsigned int count = 0;
    while (val >>= 1) {
        ++count;
    }
    return count;
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

#define MIN_SIZE 1024
#define MAX_SIZE 1024 * 1024 * 128
#define GRANULARITY 1



int main() {
    printf("   memsize  time in ns\n");
    for (size_t memsize = MIN_SIZE; memsize <= MAX_SIZE;
         memsize += (1u << (unsigned int)(fmax(GRANULARITY, log2_custom(memsize)) - GRANULARITY))) {
        void** memory = create_random_chain(memsize);
        size_t count = fmax(memsize * 16, (size_t)1<<30);
        double t = chase_pointers(memory, count);
        free(memory);  
        double ns = t * 1000000000 / count;
        printf(" %9zu  %10.5lf\n", memsize, ns);
        fflush(stdout);
    }
    return 0;
}
