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
#include <math.h>
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


static struct timespec times[2];
static struct acctest_context *dsa, *iaa;
static unsigned long buf_size = DSA_TEST_SIZE;

typedef struct {
    void *buffer;
    size_t size;
    size_t count;  // Result from host_op
		int (*host_op)(void *buffer, size_t size);
} host_op_args;

// Function prototypes
void *memcpy_and_submit(void *arg);
void *wait_for_iaa(void *arg);
void *dsa_submit(void *arg);
void *host_operation_thread(void *arg);
int shuffle_host_op(void *buffer, size_t size);
int stencil(void *buffer, size_t size);
int host_op(void *buffer, size_t size);

static int setup_dsa_iaa(int num_desc) {
	struct task_node *dsa_tsk_node, *iaa_tsk_node;
	int rc = ACCTEST_STATUS_OK;
	int tflags = 0x1;

	info("testmemory: opcode %d len %#lx tflags %#x num_desc %ld\n",
	     DSA_OPCODE_MEMMOVE, buf_size, tflags, num_desc);

	dsa->is_batch = 0;

	rc = acctest_alloc_multiple_tasks(dsa, num_desc);
	if (rc != ACCTEST_STATUS_OK)
		return rc;
	rc = acctest_alloc_multiple_tasks(iaa, num_desc);
	if (rc != ACCTEST_STATUS_OK)
		return rc;

	/* allocate memory to src and dest buffers and fill in the desc for all the nodes*/
	dsa_tsk_node = dsa->multi_task_node;
	iaa_tsk_node = iaa->multi_task_node;
	while (dsa_tsk_node) {
		dsa_tsk_node->tsk->xfer_size = buf_size;

		rc = dsa_init_task(dsa_tsk_node->tsk, tflags, DSA_OPCODE_MEMMOVE, buf_size);
		if (rc != ACCTEST_STATUS_OK)
			return rc;
		rc = iaa_init_task(iaa_tsk_node->tsk, tflags, IAX_OPCODE_COMPRESS, buf_size);
		if (rc != ACCTEST_STATUS_OK)
			return rc;
		iaa_tsk_node = iaa_tsk_node->next;
		dsa_tsk_node = dsa_tsk_node->next;
	}
	return rc;
}

int (*select_host_op(int host_op_sel))(void *buffer, size_t size){
	switch(host_op_sel){
		case 0:
			return host_op;
		case 1:
			return shuffle_host_op;
		case 2:
			return stencil;
		default:
			return host_op;
	}
}
int stencil(void *buffer, size_t size){
	int filter[3][3] = {
			{-1, -1, -1},
			{-1, 8, -1},
			{-1, -1, -1}
	};

	double squareRoot = sqrt(size);
	int height = (int)squareRoot;
	int width = (int)squareRoot;
	int output[height][width];
	int *ptr = (int *)buffer;
	int inpBuf[height][width];
	int count = 0;
	for (int i = 0; i < height; i++) {
				for (int j = 0; j < width; j++) {
						inpBuf[i][j] = ptr[i * width + j];
				}
		}

	for (int y = 1; y < height - 1; y++) {
        for (int x = 1; x < width - 1; x++) {
            int sum = 0;
            for (int fy = -1; fy <= 1; fy++) {
                for (int fx = -1; fx <= 1; fx++) {
                    sum += inpBuf[y + fy][x + fx] * filter[fy + 1][fx + 1];
                }
            }
            output[y][x] = sum;
        }
    }
	return count;
}
int shuffle_host_op(void *buffer, size_t size){
	for(int i=0; i<100; i++){
		shuffle_elements(buffer, size);
	}
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
	clock_gettime(CLOCK_MONOTONIC, &times[0]);
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

#define RING_SIZE 1024


#define RING_SIZE 1024

typedef struct {
    void *ops[RING_SIZE];
    int head;
    int tail;
    pthread_mutex_t lock;
} opRing;

typedef struct {
    opRing *ring;
} kWorkerArgs;

void enqueue(opRing *ring, void *op) {
    pthread_mutex_lock(&ring->lock);
    if ((ring->head + 1) % RING_SIZE != ring->tail) {
        ring->ops[ring->head] = op;
        ring->head = (ring->head + 1) % RING_SIZE;
    }
    pthread_mutex_unlock(&ring->lock);
}

void *dequeue(opRing *ring) {
    pthread_mutex_lock(&ring->lock);
    void *op = NULL;
    if (ring->head != ring->tail) {
        op = ring->ops[ring->tail];
        ring->tail = (ring->tail + 1) % RING_SIZE;
    }
    pthread_mutex_unlock(&ring->lock);
    return op;
}

void *app_worker_thread(void *arg){
	pth_t pth;
	pth_attr_t attr;
	pth_init();
	attr = pth_attr_new();
	pth_attr_set(attr, PTH_ATTR_NAME, "host_op_thread");
	pth_attr_set(attr, PTH_ATTR_STACK_SIZE, 64*1024);
	pth_attr_set(attr, PTH_ATTR_JOINABLE, TRUE);
	info("App worker thread created\n");

	kWorkerArgs *args = (kWorkerArgs *)arg;
	while (1) {
			host_op_args *arg = (host_op_args *)dequeue(args->ring);
			if (arg != NULL) {
				if (pth_spawn(attr, host_operation_thread, arg) == NULL) {
					printf("Error creating host op thread\n");
					exit(-1);
				}
				pth_join(pth, NULL);
			}
	}
	return NULL;

}

void *memcpy_and_submit(void *arg) {
    struct task_node *dsa_tsk_node, *iaa_tsk_node;
    int rc;
	host_op_args *args;
	pthread_t host_thread;

	pthread_t cbTd;

	opRing *ring = malloc(sizeof(opRing));
	memset(ring, 0, sizeof(opRing));
	kWorkerArgs *kArgs = malloc(sizeof(kWorkerArgs));
	memset(kArgs, 0, sizeof(kWorkerArgs));
	kArgs->ring = ring;
	pthread_mutex_init(&ring->lock, NULL);

	pthread_create(&cbTd, NULL, app_worker_thread, kArgs);
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(5, &cpuset);
	pthread_setaffinity_np(cbTd, sizeof(cpu_set_t), &cpuset);

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
				args->host_op = selected_op;
				args->buffer = dsa_tsk_node->tsk->dst1;
				args->size = buf_size;
				enqueue(ring, args);

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
		while( !complete ){}
		clock_gettime(CLOCK_MONOTONIC, &times[1]);
    pthread_exit((void *)ACCTEST_STATUS_OK);
}

void submit_poll_hostop_submit_poll(void *arg){
	struct task_node *dsa_tsk_node, *iaa_tsk_node;
	/* submit */
	int rc = 0;
	clock_gettime(CLOCK_MONOTONIC, &times[0]);
	rc = dsa_memcpy_submit_task_nodes(dsa);
	if (rc != ACCTEST_STATUS_OK)
		pthread_exit((void *)(intptr_t)rc);

	/* memcpy and submit */
	host_op_args *args;
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
			// if(do_spt_spinup){
			// 	args->host_op = selected_op;
			// 	args->buffer = dsa_tsk_node->tsk->dst1;
			// 	args->size = buf_size;
			// 	enqueue(ring, args);

			// } else {
				selected_op(dsa_tsk_node->tsk->dst1, buf_size);
				// no atomic ctr update from thread -- do it here
				finalHostOpCtr += 1;
				if(finalHostOpCtr == expectedHostOps){
					complete = 1;
				}
			// }

        // Continue with other operations
        iaa_tsk_node->tsk->src1 = dsa_tsk_node->tsk->dst1;
        iaa_prep_sub_task_node(iaa, iaa_tsk_node);

        dsa_tsk_node = dsa_tsk_node->next;
        iaa_tsk_node = iaa_tsk_node->next;
    }

		iaa_tsk_node = iaa->multi_task_node;
		while(iaa_tsk_node) {
		// printf("Wait for IAA itr: %d\n", itr++);
        rc = iaa_wait_compress(iaa, iaa_tsk_node->tsk);
        if (rc != ACCTEST_STATUS_OK)
            pthread_exit((void *)(intptr_t)rc);
        iaa_tsk_node = iaa_tsk_node->next;
    }
		while( !complete ){}
		clock_gettime(CLOCK_MONOTONIC, &times[1]);
    pthread_exit((void *)ACCTEST_STATUS_OK);

}


int main(int argc, char *argv[])
{
	int rc = 0;
	int wq_type = SHARED;
	int opt;
	int tflags = TEST_FLAGS_BOF;
	int wq_id = ACCTEST_DEVICE_ID_NO_INPUT;
	int dev_id = ACCTEST_DEVICE_ID_NO_INPUT;
	unsigned int num_desc = 1;
	// unsigned int num_iter = 1;
	pthread_t dsa_wait_thread, iaa_wait_thread;
	pthread_t dsa_submit_thread;
	int rc0, rc1, rc2;
	long long lat = 0;


	while ((opt = getopt(argc, argv, "w:l:i:t:n:vh:s:")) != -1) {
		switch (opt) {
		case 'w':
			wq_type = atoi(optarg);
			break;
		// case 'i':
		// 	num_iter = strtoul(optarg, NULL, 0);
		// 	break;
		case 't':
			ms_timeout = strtoul(optarg, NULL, 0);
			break;
		case 'n':
			num_desc = strtoul(optarg, NULL, 0);
			expectedHostOps = num_desc;
			break;
		case 'v':
			debug_logging = 1;
			break;
		case 'h':
			host_op_sel = strtoul(optarg, NULL, 0);
		case 's':
			do_spt_spinup = strtoul(optarg, NULL, 0);
		default:
			break;
		}
	}

	// iaa setup
	iaa = acctest_init(tflags);
	iaa->dev_type = ACCFG_DEVICE_IAX;

	if (!iaa)
		return -ENOMEM;

	rc = acctest_alloc(iaa, wq_type, dev_id, wq_id);
	if (rc < 0)
		return -ENOMEM;

	if (buf_size > iaa->max_xfer_size) {
		err("invalid transfer size: %lu\n", buf_size);
		return -EINVAL;
	}

	// DSA setup
	dsa = acctest_init(tflags);
	dsa->dev_type = ACCFG_DEVICE_DSA;

	if (!dsa)
		return -ENOMEM;

	rc = acctest_alloc(dsa, wq_type, dev_id, wq_id);
	if (rc < 0)
		return -ENOMEM;

	if (buf_size > dsa->max_xfer_size) {
		err("invalid transfer size: %lu\n", buf_size);
		return -EINVAL;
	}

	rc = setup_dsa_iaa(num_desc);
	if (rc != ACCTEST_STATUS_OK)
		goto error;

	cpu_set_t cpuset;
	pthread_create(&dsa_wait_thread, NULL, memcpy_and_submit, NULL);
	CPU_ZERO(&cpuset);
	CPU_SET(2, &cpuset);
	pthread_setaffinity_np(dsa_wait_thread, sizeof(cpu_set_t), &cpuset);

	pthread_create(&iaa_wait_thread, NULL, wait_for_iaa, NULL);
	CPU_ZERO(&cpuset);
	CPU_SET(3, &cpuset);
	pthread_setaffinity_np(iaa_wait_thread, sizeof(cpu_set_t), &cpuset);

	pthread_create(&dsa_submit_thread, NULL, dsa_submit, NULL);
	CPU_ZERO(&cpuset);
	CPU_SET(1, &cpuset);
	pthread_setaffinity_np(dsa_submit_thread, sizeof(cpu_set_t), &cpuset);


	    // Wait for threads to finish
	pthread_join(dsa_submit_thread, (void **)&rc0);
    pthread_join(dsa_wait_thread, (void **)&rc1);
    pthread_join(iaa_wait_thread, (void **)&rc2);


	clock_gettime(CLOCK_MONOTONIC, &times[1]);

	lat = ((times[1].tv_nsec) + (times[1].tv_sec * 1000000000))  -
					((times[0].tv_nsec) + (times[0].tv_sec * 1000000000));

	if (rc0 != ACCTEST_STATUS_OK || rc1 != ACCTEST_STATUS_OK
		|| rc2 != ACCTEST_STATUS_OK)
		goto error;
	// Final verification and cleanup

	printf("Total Latency: %lld ns\n", lat);
	printf("Throughput: %f\n", (buf_size * num_desc)/(double)lat);

    acctest_free_task(dsa);
	acctest_free_task(iaa);

 error:
	acctest_free(dsa);
	acctest_free(iaa);
	return rc;
}
