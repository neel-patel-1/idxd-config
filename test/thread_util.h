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

static int init_scan_from_image(struct task *tsk, int tflags, int opcode, const char *image_path, uint64_t frag_start, uint64_t frag_len) {
    int status;
    uint64_t total_size;
    void *full_image;

    tsk->opcode = opcode;
    tsk->test_flags = tflags;

    status = read_jpeg_to_buffer(image_path, &full_image, &total_size);
    if (status != ACCTEST_STATUS_OK) {
        return status;
    }

    // Default to processing the whole image if no fragment size is specified
    if ((frag_len == 0) || (frag_start == 0 && frag_len == 0)) {
        frag_len = total_size;
    }
    tsk->xfer_size = frag_len;
    if (frag_start + frag_len > total_size) {
        tsk->xfer_size = total_size - frag_start; // adjust last fragment size
    }

    tsk->src1 = aligned_alloc(32, tsk->xfer_size);
    if (!tsk->src1)
        return -ENOMEM;

    memcpy(tsk->src1, (unsigned char*)full_image + frag_start, tsk->xfer_size);

    tsk->input = aligned_alloc(32, tsk->xfer_size);
	if (!tsk->input)
		return -ENOMEM;
    memcpy(tsk->input, tsk->src1, tsk->xfer_size);

    tsk->src2 = aligned_alloc(32, IAA_FILTER_AECS_SIZE);
	if (!tsk->src2)
		return -ENOMEM;
	memset_pattern(tsk->src2, 0, IAA_FILTER_AECS_SIZE);
	iaa_filter_aecs.low_filter_param = 0x000000;
	iaa_filter_aecs.high_filter_param = 0xFFFFFF;
	memcpy(tsk->src2, (void *)&iaa_filter_aecs, IAA_FILTER_AECS_SIZE);
	tsk->iaa_src2_xfer_size = IAA_FILTER_AECS_SIZE;

	tsk->dst1 = aligned_alloc(ADDR_ALIGNMENT, tsk->xfer_size);
	if (!tsk->dst1)
		return -ENOMEM;
	memset_pattern(tsk->dst1, 0, tsk->xfer_size);

	tsk->iaa_max_dst_size = tsk->xfer_size;

	tsk->output = aligned_alloc(ADDR_ALIGNMENT, tsk->xfer_size);
	if (!tsk->output)
		return -ENOMEM;
	memset_pattern(tsk->output, 0, tsk->xfer_size);

	return ACCTEST_STATUS_OK;
}

static int memcpy_init(struct task *tsk, int tflags, 
                int opcode, unsigned long src1_xfer_size) {
	unsigned long force_align = PAGE_SIZE;
	tsk->opcode = opcode;
	tsk->test_flags = tflags;

	tsk->dst1 = aligned_alloc(force_align, src1_xfer_size);
	if (!tsk->dst1)
		return -ENOMEM;
	memset_pattern(tsk->dst1, 0, src1_xfer_size);

	return ACCTEST_STATUS_OK;
}

static int select_init(struct task *tsk, int tflags,
		       int opcode, unsigned long src1_xfer_size)
{
	tsk->opcode = opcode;
	tsk->test_flags = tflags;
	tsk->iaa_src2_xfer_size = src1_xfer_size;

	tsk->dst1 = aligned_alloc(ADDR_ALIGNMENT, src1_xfer_size);
	if (!tsk->dst1)
		return -ENOMEM;
	memset_pattern(tsk->dst1, 0, src1_xfer_size);

	tsk->iaa_max_dst_size = src1_xfer_size;

	tsk->output = aligned_alloc(ADDR_ALIGNMENT, src1_xfer_size);
	if (!tsk->output)
		return -ENOMEM;
	memset_pattern(tsk->output, 0, src1_xfer_size);

	return ACCTEST_STATUS_OK;
}