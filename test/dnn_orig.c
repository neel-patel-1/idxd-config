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

void *memcpy_src1;
uint64_t memcpy_size;



static int init_scan_from_image(struct task *tsk, int tflags, int opcode, const char *image_path) {
	int status;
    tsk->opcode = opcode;
    tsk->test_flags = tflags;

    // Reading the BMP file into src1
    status = read_jpeg_to_buffer(image_path, &tsk->src1, &tsk->xfer_size);
    if (status != ACCTEST_STATUS_OK) {
        // Handle error
        return status;
    }

	tsk->input = aligned_alloc(32, tsk->xfer_size);
	if (!tsk->input)
		return -ENOMEM;

	memcpy(tsk->input, tsk->src1, tsk->xfer_size);

    tsk->src2 = aligned_alloc(32, IAA_FILTER_MAX_SRC2_SIZE);
	if (!tsk->src2)
		return -ENOMEM;
	memset_pattern(tsk->src2, 0, IAA_FILTER_AECS_SIZE);
	iaa_filter_aecs.low_filter_param = 0x000000;
	iaa_filter_aecs.high_filter_param = 0xFFFFFF;
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

static int memcpy_init(struct task *tsk, int tflags, int opcode, unsigned long xfer_size) {
	unsigned long force_align = PAGE_SIZE;

	tsk->pattern = 0x0123456789abcdef;
	tsk->pattern2 = 0xfedcba9876543210;
	tsk->opcode = opcode;
	tsk->test_flags = tflags;
	tsk->xfer_size = xfer_size;

	tsk->src1 = memcpy_src1;

	tsk->dst1 = aligned_alloc(force_align, xfer_size);
	if (!tsk->dst1)
		return -ENOMEM;
	memset_pattern(tsk->dst1, 0, xfer_size);

	return ACCTEST_STATUS_OK;
}

static int test_filter(struct acctest_context *ctx, int tflags, int extra_flags_2,  
						uint32_t opcode, int num_desc, const char *image_path)
{
	struct task_node *tsk_node;
	int rc = ACCTEST_STATUS_OK;
	int itr = num_desc, i = 0, range = 0;

	info("test filter: opcode %d tflags %#x num_desc %ld\n",
	     opcode, tflags, num_desc);

	ctx->is_batch = 0;

	if (ctx->dedicated == ACCFG_WQ_SHARED)
		range = ctx->threshold;
	else
		range = ctx->wq_size;

	while (itr > 0 && rc == ACCTEST_STATUS_OK) {
		i = (itr < range) ? itr : range;
		/* Allocate memory to all the task nodes, desc, completion record*/
		rc = acctest_alloc_multiple_tasks(ctx, i);
		if (rc != ACCTEST_STATUS_OK)
			return rc;
		/* allocate memory to src and dest buffers and fill in the desc for all the nodes*/
		tsk_node = ctx->multi_task_node;
		while (tsk_node) {
			tsk_node->tsk->iaa_filter_flags = (uint32_t)extra_flags_2;
			rc = init_scan_from_image(tsk_node->tsk, tflags, opcode, image_path);
			tsk_node->tsk->iaa_num_inputs = (uint32_t)tsk_node->tsk->xfer_size/24;
			if (rc != ACCTEST_STATUS_OK)
				return rc;
			if(print_contents) {
				printf("Filter source:\n");
				print_elements(tsk_node->tsk->src1, tsk_node->tsk->xfer_size);
			}
			tsk_node = tsk_node->next;
		}
		clock_gettime(CLOCK_MONOTONIC, &times[0]);
		rc = iaa_scan_multi_task_nodes(ctx);
		clock_gettime(CLOCK_MONOTONIC, &times[1]);
		// scan_lat += ((times[1].tv_nsec) + (times[1].tv_sec * 1000000000))  -
		// 	((times[0].tv_nsec) + (times[0].tv_sec * 1000000000));
		scan_lat += (times[1].tv_sec - times[0].tv_sec) + 
                     (times[1].tv_nsec - times[0].tv_nsec) / 1000000000.0;
		if (rc != ACCTEST_STATUS_OK)
			return rc;
		tsk_node = ctx->multi_task_node;
		if(print_contents) {
			printf("Scan destination:\n");
			print_elements(tsk_node->tsk->dst1, tsk_node->tsk->comp->iax_output_size);
		}
		if(verify_data) {
			/* Verification of all the nodes*/
			rc = iaa_task_result_verify_task_nodes(ctx, 0);
			if (rc != ACCTEST_STATUS_OK){
				return rc;
			}
		}

		//init select
		tsk_node = ctx->multi_task_node;
		while(tsk_node) {
			memset_pattern(tsk_node->tsk->src2, 0, IAA_FILTER_MAX_SRC2_SIZE);
			memset_pattern(tsk_node->tsk->output, 0, IAA_FILTER_MAX_SRC2_SIZE);
			memcpy(tsk_node->tsk->src2, tsk_node->tsk->dst1,
		       tsk_node->tsk->comp->iax_output_size);
			tsk_node->tsk->iaa_src2_xfer_size = IAA_FILTER_MAX_SRC2_SIZE;
			memset_pattern(tsk_node->tsk->dst1, 0, IAA_FILTER_MAX_DEST_SIZE);
			tsk_node->tsk->dflags &= ~IDXD_OP_FLAG_RD_SRC2_AECS;
			tsk_node->tsk->opcode = IAX_OPCODE_SELECT;
			tsk_node = tsk_node->next;
		}
		clock_gettime(CLOCK_MONOTONIC, &times[0]);
		rc = iaa_select_multi_task_nodes(ctx);
		clock_gettime(CLOCK_MONOTONIC, &times[1]);
		select_lat += (times[1].tv_sec - times[0].tv_sec) + 
                     (times[1].tv_nsec - times[0].tv_nsec) / 1000000000.0;
		if (rc != ACCTEST_STATUS_OK)
			return rc;		
		tsk_node = ctx->multi_task_node;
		while(tsk_node) {
			if(print_contents) {
				printf("Filter destination:\n");
				print_elements(tsk_node->tsk->dst1, tsk_node->tsk->comp->iax_output_size);
			}
			memcpy_src1 = tsk_node->tsk->dst1;
			memcpy_size = tsk_node->tsk->comp->iax_output_size;
			// printf("memcpy_size: %d\n", memcpy_size);
			tsk_node = tsk_node->next;
		}

		/* Verification of all the nodes*/
		if(verify_data) {
			rc = iaa_task_result_verify_task_nodes(ctx, 0);
			if (rc != ACCTEST_STATUS_OK)
				return rc;
		}

		//filter_free_task(ctx);
		ctx->multi_task_node = NULL;
		itr = itr - range;
	}
	return rc;
}

static int test_memcpy(struct acctest_context *dsa, int tflags, 
						uint32_t dsa_opcode, int num_desc)
{
	struct task_node *dsa_tsk_node;
	int rc = ACCTEST_STATUS_OK;
	int itr = num_desc, i = 0, range = 0;

	info("testmemory: opcode %d tflags %#x num_desc %ld\n",
	     dsa_opcode, tflags, num_desc);

	dsa->is_batch = 0;

	if (dsa->dedicated == ACCFG_WQ_SHARED)
		range = dsa->threshold;
	else
		range = dsa->wq_size;

	while (itr > 0 && rc == ACCTEST_STATUS_OK) {
		i = (itr < range) ? itr : range;
		/*DSA: Allocate memory to all the task nodes, desc, completion record*/
		rc = acctest_alloc_multiple_tasks(dsa, i);
		if (rc != ACCTEST_STATUS_OK)
			return rc;

		/* DSA: allocate memory to src and dest buffers and fill in the desc for all the nodes*/
		dsa_tsk_node = dsa->multi_task_node;

		while (dsa_tsk_node) {
			dsa_tsk_node->tsk->xfer_size = memcpy_size;
			rc = memcpy_init(dsa_tsk_node->tsk, tflags, dsa_opcode, memcpy_size);
			if (rc != ACCTEST_STATUS_OK)
				return rc;

			dsa_tsk_node = dsa_tsk_node->next;
		}
		dsa_tsk_node = dsa->multi_task_node;

		clock_gettime(CLOCK_MONOTONIC, &times[0]);
		rc = dsa_memcpy_multi_task_nodes(dsa);
		clock_gettime(CLOCK_MONOTONIC, &times[1]);
		memcpy_lat += (times[1].tv_sec - times[0].tv_sec) + 
                     (times[1].tv_nsec - times[0].tv_nsec) / 1000000000.0;
		if (rc != ACCTEST_STATUS_OK)
			return rc;

		if(verify_data) {
			/* Verification of all the nodes*/
			rc = task_result_verify_task_nodes(dsa, 0);
			if (rc != ACCTEST_STATUS_OK)
				return rc;
		}


		dsa_tsk_node = dsa->multi_task_node;
		while(dsa_tsk_node) {
			if(print_contents) {
				printf("Memcpy destination:\n");
				print_elements(dsa_tsk_node->tsk->dst1, memcpy_size);
			}
			dsa_tsk_node = dsa_tsk_node->next;
		}

		acctest_free_task(dsa);
		itr = itr - range;
	}

	return rc;
}


int main(int argc, char *argv[])
{
	struct acctest_context *dsa, *iaa;
	int rc = 0;
	int wq_type = DEDICATED;
	int dsa_opcode = DSA_OPCODE_MEMMOVE;
	int iaa_opcode = IAX_OPCODE_SCAN;
	int opt;
	int tflags = TEST_FLAGS_BOF;
	int wq_id = ACCTEST_DEVICE_ID_NO_INPUT;
	int dev_id = ACCTEST_DEVICE_ID_NO_INPUT;
	unsigned int num_desc = 1;
	unsigned int num_iter = 1;
	int extra_flags_2 = 0x5c;
	DIR *dir;
    struct dirent *ent;
    char filepath[1024];
	const char* directory_path = "./test/images";
	int pos = 0;
	struct timespec e2e_times[2];
	double e2e_time_s = 0;


	while ((opt = getopt(argc, argv, "w:l:i:t:vh")) != -1) {
		switch (opt) {
		case 'w':
			wq_type = atoi(optarg);
			break;
		case 'i':
			num_iter = strtoul(optarg, NULL, 0);
			break;
		case 't':
			ms_timeout = strtoul(optarg, NULL, 0);
			break;
		case 'v':
			debug_logging = 1;
			break;
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

	// if (buf_size > iaa->max_xfer_size) {
	// 	err("invalid transfer size: %lu\n", buf_size);
	// 	return -EINVAL;
	// }

	// DSA setup
	dsa = acctest_init(tflags);
	dsa->dev_type = ACCFG_DEVICE_DSA;

	if (!dsa)
		return -ENOMEM;

	rc = acctest_alloc(dsa, wq_type, dev_id, wq_id);
	if (rc < 0)
		return -ENOMEM;

	// if (buf_size > dsa->max_xfer_size) {
	// 	err("invalid transfer size: %lu\n", buf_size);
	// 	return -EINVAL;
	// }

	dir = opendir(directory_path);
    if (dir == NULL) {
        perror("Could not open directory");
        return -1;
    }
	clock_gettime(CLOCK_MONOTONIC, &e2e_times[0]);
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_type == DT_REG && ent->d_name[0] != '.') {
			snprintf(filepath, sizeof(filepath), "%s/%s", directory_path, ent->d_name);
			printf("Testing image: %s, index: %d\n", filepath, pos++);
			rc = test_filter(iaa, tflags, extra_flags_2, iaa_opcode, num_desc, filepath);
			if (rc != ACCTEST_STATUS_OK)
				goto error;
			clock_gettime(CLOCK_MONOTONIC, &times[0]);
			shuffle_elements(memcpy_src1, memcpy_size);
			clock_gettime(CLOCK_MONOTONIC, &times[1]);
			shuffle_lat += (times[1].tv_sec - times[0].tv_sec) + 
                     (times[1].tv_nsec - times[0].tv_nsec) / 1000000000.0;
			rc = test_memcpy(dsa, tflags, dsa_opcode, num_desc);
			if (rc != ACCTEST_STATUS_OK)
				goto error;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &e2e_times[1]);
	e2e_time_s = (e2e_times[1].tv_sec - e2e_times[0].tv_sec) + 
                   (e2e_times[1].tv_nsec - e2e_times[0].tv_nsec) / 1000000000.0;
	
	printf("Scan latency: %fms\n", scan_lat* 1000.0);
	printf("Select latency: %fms\n", select_lat * 1000.0);
	printf("Shuffle latency: %fms\n", shuffle_lat*1000.0);
	printf("Memcpy latency: %fms\n", memcpy_lat*1000.0);
	printf("E2E time: %fs\n", e2e_time_s);


 error:
	acctest_free(dsa);
	acctest_free(iaa);
	return rc;
}