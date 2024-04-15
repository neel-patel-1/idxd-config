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
#include <setjmp.h>
#include <jpeglib.h>
#include "util.h"
#include <dirent.h>

#define DSA_MEMCPY_MAX_DEST_SIZE (2097152 * 2)

static struct iaa_filter_aecs_t iaa_filter_aecs = {
	.rsvd = 0,
	.rsvd2 = 0,
	.rsvd3 = 0,
	.rsvd4 = 0,
	.rsvd5 = 0,
	.rsvd6 = 0
};

double scan_lat = 0;
double select_lat = 0;
double shuffle_lat = 0;
double memcpy_lat = 0;
struct timespec times[2];
bool print_contents = false;
bool verify_data = false;
struct acctest_context *dsa, *scan_iaa, *select_iaa;
unsigned int num_desc = 1;
unsigned int num_images = 1;
const char* directory_path = "./test/images2";

void *scan_submit(void *arg);
void *scan_wait_select_submit(void *arg);
void *select_wait_memcpy_submit(void *arg);
void *memcpy_wait(void *arg);


int init_scan_from_image(struct task *tsk, int tflags, int opcode, const char *image_path, uint64_t frag_start, uint64_t frag_size) {
    int status;
    tsk->opcode = opcode;
    tsk->test_flags = tflags;

    uint64_t total_size;
    void *full_image;

    status = read_jpeg_to_buffer(image_path, &full_image, &total_size);
    if (status != ACCTEST_STATUS_OK) {
        return status;
    }

    tsk->xfer_size = frag_size;
    if (frag_start + frag_size > total_size) {
        tsk->xfer_size = total_size - frag_start; // adjust last fragment size
    }

    tsk->src1 = aligned_alloc(32, tsk->xfer_size);
    if (!tsk->src1)
        return -ENOMEM;

    memcpy(tsk->src1, full_image + frag_start, tsk->xfer_size);
    memcpy(tsk->input, tsk->src1, tsk->xfer_size);

    tsk->src2 = aligned_alloc(32, IAA_FILTER_MAX_SRC2_SIZE);
	if (!tsk->src2)
		return -ENOMEM;
	memset_pattern(tsk->src2, 0, IAA_FILTER_AECS_SIZE);
	iaa_filter_aecs.low_filter_param = 0x000000;
	iaa_filter_aecs.high_filter_param = 0xF00000;
	memcpy(tsk->src2, (void *)&iaa_filter_aecs, IAA_FILTER_AECS_SIZE);
	tsk->iaa_src2_xfer_size = IAA_FILTER_AECS_SIZE;

	tsk->dst1 = aligned_alloc(ADDR_ALIGNMENT, IAA_FILTER_MAX_DEST_SIZE);
	if (!tsk->dst1)
		return -ENOMEM;
	memset_pattern(tsk->dst1, 0, IAA_FILTER_MAX_DEST_SIZE);

	tsk->iaa_max_dst_size = IAA_FILTER_MAX_DEST_SIZE;

	tsk->output = aligned_alloc(ADDR_ALIGNMENT, IAA_FILTER_MAX_DEST_SIZE);
	if (!tsk->output)
		return -ENOMEM;
	memset_pattern(tsk->output, 0, IAA_FILTER_MAX_DEST_SIZE);

	return ACCTEST_STATUS_OK;
}

static int memcpy_init(struct task *tsk, int tflags, int opcode) {
	unsigned long force_align = PAGE_SIZE;
	tsk->opcode = opcode;
	tsk->test_flags = tflags;

	tsk->dst1 = aligned_alloc(force_align, DSA_MEMCPY_MAX_DEST_SIZE);
	if (!tsk->dst1)
		return -ENOMEM;
	memset_pattern(tsk->dst1, 0, DSA_MEMCPY_MAX_DEST_SIZE);

	return ACCTEST_STATUS_OK;
}

static int select_init(struct task *tsk, int tflags,
		       int opcode)
{
	tsk->opcode = opcode;
	tsk->test_flags = tflags;
	tsk->iaa_src2_xfer_size = IAA_FILTER_MAX_SRC2_SIZE;

	tsk->dst1 = aligned_alloc(ADDR_ALIGNMENT, IAA_FILTER_MAX_DEST_SIZE);
	if (!tsk->dst1)
		return -ENOMEM;
	memset_pattern(tsk->dst1, 0, IAA_FILTER_MAX_DEST_SIZE);

	tsk->iaa_max_dst_size = IAA_FILTER_MAX_DEST_SIZE;

	tsk->output = aligned_alloc(ADDR_ALIGNMENT, IAA_FILTER_MAX_DEST_SIZE);
	if (!tsk->output)
		return -ENOMEM;
	memset_pattern(tsk->output, 0, IAA_FILTER_MAX_DEST_SIZE);

	return ACCTEST_STATUS_OK;
}

static int setup_scan(void) {
    DIR *dir;
    struct dirent *ent;
    char image_path[1024];
    uint64_t frag_size = 4096; 
    uint64_t num_desc = 0; 
    uint64_t total_size;
    int image_count = 0; 
    int tflags = 0x1;
	int extra_flags_2 = 0x5c;

    dir = opendir(directory_path);
    if (dir == NULL) {
        perror("Failed to open directory");
        return -1;
    }

    // First Pass: Calculate total number of fragments for the specified number of images
    while ((ent = readdir(dir)) != NULL && image_count < num_images) {
        if (ent->d_type == DT_REG && strncmp(ent->d_name, "ILSVRC2017_test_", 16) == 0) {
            snprintf(image_path, sizeof(image_path), "%s/%s", directory_path, ent->d_name);
            void *buffer;
            read_jpeg_to_buffer(image_path, &buffer, &total_size);
            free(buffer); 
            num_desc += (total_size + frag_size - 1) / frag_size; 
            image_count++; 
        }
    }
    rewinddir(dir); // Reset directory stream for the second pass

    // Allocate memory to all the task nodes, desc, completion record based on calculated num_desc
    int rc = acctest_alloc_multiple_tasks(scan_iaa, num_desc);
    if (rc != ACCTEST_STATUS_OK) {
        closedir(dir);
        return rc;
    }

    // Second Pass: Setup task nodes for each fragment
    struct task_node *current_node = scan_iaa->multi_task_node;
    image_count = 0; 
    while ((ent = readdir(dir)) != NULL && image_count < num_images) {
        if (ent->d_type == DT_REG && strncmp(ent->d_name, "ILSVRC2017_test_", 16) == 0) {
            current_node->tsk->iaa_filter_flags = (uint32_t)extra_flags_2;
            snprintf(image_path, sizeof(image_path), "%s/%s", directory_path, ent->d_name);
            void *buffer;
            read_jpeg_to_buffer(image_path, &buffer, &total_size);
            uint64_t num_frags = (total_size + frag_size - 1) / frag_size;
            for (uint64_t i = 0; i < num_frags; i++, current_node = current_node->next) {
                uint64_t frag_start = i * frag_size;
                uint64_t frag_end = frag_start + frag_size;
                if (frag_end > total_size) frag_end = total_size;
                init_scan_from_image(current_node->tsk, tflags, IAX_OPCODE_SCAN, image_path, frag_start, frag_end - frag_start);
            }
            free(buffer);
            image_count++; 
        }
    }

    closedir(dir);
    return ACCTEST_STATUS_OK;
}

static int setup_select(void) {
	struct task_node *tsk_node;
    int rc = ACCTEST_STATUS_OK;
	int tflags = 0x1;
	int extra_flags_2 = 0x5c;
	info("testselect: opcode %d tflags %#x num_desc %ld\n",
	     IAX_OPCODE_SELECT, tflags, num_desc);

    /* Allocate memory to all the task nodes, desc, completion record */
    rc = acctest_alloc_multiple_tasks(select_iaa, num_desc);
    if (rc != ACCTEST_STATUS_OK) {
        return rc;
    }

    tsk_node = select_iaa->multi_task_node;

	while (tsk_node) {
		tsk_node->tsk->iaa_filter_flags = (uint32_t)extra_flags_2;

		rc = select_init(tsk_node->tsk, tflags, IAX_OPCODE_SELECT);
		if (rc != ACCTEST_STATUS_OK)
			return rc;

		tsk_node = tsk_node->next;
	}
    return ACCTEST_STATUS_OK;
}

static int setup_memcpy(void) {
	struct task_node *dsa_tsk_node;
	int rc = ACCTEST_STATUS_OK;
	int tflags = 0x1;

	info("testmemory: opcode %d tflags %#x num_desc %ld\n",
	     DSA_OPCODE_MEMMOVE, tflags, num_desc);

	dsa->is_batch = 0;

	/*DSA: Allocate memory to all the task nodes, desc, completion record*/
	rc = acctest_alloc_multiple_tasks(dsa, num_desc);
	if (rc != ACCTEST_STATUS_OK)
		return rc;

	/* DSA: allocate memory to src and dest buffers and fill in the desc for all the nodes*/
	dsa_tsk_node = dsa->multi_task_node;

	while (dsa_tsk_node) {
		rc = memcpy_init(dsa_tsk_node->tsk, tflags, DSA_OPCODE_MEMMOVE);
		if (rc != ACCTEST_STATUS_OK)
			return rc;

		dsa_tsk_node = dsa_tsk_node->next;
	}
}

void *scan_submit(void *arg) {
	int rc = 0;
	rc = iaa_scan_multi_task_nodes(scan_iaa);
	if (rc != ACCTEST_STATUS_OK)
		pthread_exit((void *)(intptr_t)rc);
	pthread_exit((void *)ACCTEST_STATUS_OK);
}

void *scan_wait_select_submit(void *arg) {
    struct task_node *scan_tsk_node, *select_tsk_node;
    int rc;

    scan_tsk_node = scan_iaa->multi_task_node;
    select_tsk_node = select_iaa->multi_task_node;

    while (scan_tsk_node) {
        rc = iaa_wait_scan(scan_iaa, scan_tsk_node->tsk);
        if (rc != ACCTEST_STATUS_OK)
            pthread_exit((void *)(intptr_t)rc);
		select_tsk_node->tsk->src1 = scan_tsk_node->tsk->src1;
		select_tsk_node->tsk->xfer_size = scan_tsk_node->tsk->xfer_size;
		select_tsk_node->tsk->src2 = scan_tsk_node->tsk->dst1;
		select_tsk_node->tsk->iaa_num_inputs = (uint32_t)select_tsk_node->tsk->xfer_size / 24;
		// printf("scan output size: %u\n", scan_tsk_node->tsk->comp->iax_output_size);
        rc = iaa_select_prep_sub_tsk_node(select_iaa, select_tsk_node);
		if(print_contents) {
			printf("Filter source 1:\n");
			print_elements(select_tsk_node->tsk->src1, select_tsk_node->tsk->xfer_size);
			printf("Filter source 2:\n");
			print_elements(select_tsk_node->tsk->src2, scan_tsk_node->tsk->comp->iax_output_size);
		}
		if (rc != ACCTEST_STATUS_OK)
            pthread_exit((void *)(intptr_t)rc);
        scan_tsk_node = scan_tsk_node->next;
        select_tsk_node = select_tsk_node->next;
    }

    pthread_exit((void *)ACCTEST_STATUS_OK);
}

void *select_wait_memcpy_submit(void *arg) {
    struct task_node *select_tsk_node, *dsa_tsk_node;
    int rc;
	pthread_t host_thread;

    select_tsk_node = select_iaa->multi_task_node;
	dsa_tsk_node = dsa->multi_task_node;

    while (select_tsk_node) {
        rc = iaa_wait_select(select_iaa, select_tsk_node->tsk);
        if (rc != ACCTEST_STATUS_OK)
            pthread_exit((void *)(intptr_t)rc);
		shuffle_elements(select_tsk_node->tsk->dst1, select_tsk_node->tsk->comp->iax_output_size);
		dsa_tsk_node->tsk->src1 = select_tsk_node->tsk->dst1;
		dsa_tsk_node->tsk->xfer_size = select_tsk_node->tsk->comp->iax_output_size;
        rc = dsa_memcpy_prep_sub_task_node(dsa, dsa_tsk_node);
		if (rc != ACCTEST_STATUS_OK)
            pthread_exit((void *)(intptr_t)rc);
        select_tsk_node = select_tsk_node->next;
        dsa_tsk_node = dsa_tsk_node->next;
    }

    pthread_exit((void *)ACCTEST_STATUS_OK);
}

void *memcpy_wait(void *arg) {
    struct task_node *dsa_task_node = dsa->multi_task_node;
    int rc;

    while(dsa_task_node) {
        rc = dsa_wait_memcpy(dsa, dsa_task_node->tsk);
        if (rc != ACCTEST_STATUS_OK)
            pthread_exit((void *)(intptr_t)rc);
        dsa_task_node = dsa_task_node->next;
    }

    pthread_exit((void *)ACCTEST_STATUS_OK);
}



int main(int argc, char *argv[])
{
	int rc = 0;
	int wq_type = SHARED;
	int dsa_opcode = DSA_OPCODE_MEMMOVE;
	int opt;
	int tflags = TEST_FLAGS_BOF;
	int dsa_wq_id = ACCTEST_DEVICE_ID_NO_INPUT;
	int dsa_dev_id = ACCTEST_DEVICE_ID_NO_INPUT;
    int iaa_scan_wq_id = 1;
    int iaa_select_wq_id = 4;
	int iaa_dev_id = 3;
	int extra_flags_2 = 0x5c;
	struct timespec e2e_times[2];
	double e2e_time_s = 0;
	pthread_t scan_sub_thread, scan_wait_select_sub_thread;
	pthread_t select_wait_dsa_sub_thread, dsa_wait_thread;
	int rc0, rc1, rc2, rc3;


	while ((opt = getopt(argc, argv, "w:l:i:t:n:vh")) != -1) {
		switch (opt) {
		case 'w':
			wq_type = atoi(optarg);
			break;
		case 'i':
			num_images = strtoul(optarg, NULL, 0);
			break;
		case 't':
			ms_timeout = strtoul(optarg, NULL, 0);
			break;
		case 'n':
			num_desc = strtoul(optarg, NULL, 0);
			break;
		case 'v':
			debug_logging = 1;
			break;
		default:
			break;
		}
	}

	// iaa scan device setup
	scan_iaa = acctest_init(tflags);
	scan_iaa->dev_type = ACCFG_DEVICE_IAX;

	if (!scan_iaa)
		return -ENOMEM;

	rc = acctest_alloc(scan_iaa, wq_type, iaa_dev_id, iaa_scan_wq_id);
	if (rc < 0)
		return -ENOMEM;

    // iaa select device setup
    select_iaa = acctest_init(tflags);
	select_iaa->dev_type = ACCFG_DEVICE_IAX;

	if (!select_iaa)
		return -ENOMEM;

	rc = acctest_alloc(select_iaa, wq_type, iaa_dev_id, iaa_select_wq_id);
	if (rc < 0)
		return -ENOMEM;

	// DSA setup
	dsa = acctest_init(tflags);
	dsa->dev_type = ACCFG_DEVICE_DSA;

	if (!dsa)
		return -ENOMEM;

	rc = acctest_alloc(dsa, wq_type, dsa_dev_id, dsa_wq_id);
	if (rc < 0)
		return -ENOMEM;

	setup_scan();
	setup_select();
	setup_memcpy();

	clock_gettime(CLOCK_MONOTONIC, &e2e_times[0]);
	// start threads
	if (pthread_create(&scan_sub_thread, NULL, scan_submit, NULL) != 0)
    	perror("Failed to create scan submission thread");
	if(pthread_create(&scan_wait_select_sub_thread, NULL, scan_wait_select_submit, NULL) != 0) 
		perror("Failed to create scan wait - select sub thread");
    if(pthread_create(&select_wait_dsa_sub_thread, NULL, select_wait_memcpy_submit, NULL) != 0)
		perror("Failed to create select wait - memcpy sub sub thread");
	if(pthread_create(&dsa_wait_thread, NULL, memcpy_wait, NULL) != 0) 
		perror("Failed to create dsa wait thread");

	// Wait for threads to finish
	pthread_join(scan_sub_thread, (void **)&rc0);
    pthread_join(scan_wait_select_sub_thread, (void **)&rc1);
    pthread_join(select_wait_dsa_sub_thread, (void **)&rc2);
	pthread_join(dsa_wait_thread, (void **)&rc3);

	clock_gettime(CLOCK_MONOTONIC, &e2e_times[1]);
	e2e_time_s = (e2e_times[1].tv_sec - e2e_times[0].tv_sec) + 
                   (e2e_times[1].tv_nsec - e2e_times[0].tv_nsec) / 1000000000.0;
	if (rc0 != ACCTEST_STATUS_OK || rc1 != ACCTEST_STATUS_OK 
		|| rc2 != ACCTEST_STATUS_OK || rc3 != ACCTEST_STATUS_OK)
		goto error;
	
    // rc = task_result_verify_task_nodes(scan_iaa, 0);
	// if (rc != ACCTEST_STATUS_OK)
	// 	return rc;
	// rc = task_result_verify_task_nodes(select_iaa, 0);
	// if (rc != ACCTEST_STATUS_OK)
	// 	return rc;
	// rc = task_result_verify_task_nodes(dsa, 0);
	// if (rc != ACCTEST_STATUS_OK)
	// 	return rc;

	printf("Total Latency: %f s\n", e2e_time_s);
	
	acctest_free_task(scan_iaa);
	// acctest_free_task(select_iaa);
	acctest_free_task(dsa);
 error:
	acctest_free(scan_iaa);
	// acctest_free(select_iaa);
	acctest_free(dsa);
	return rc;
}