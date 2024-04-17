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

static void usage(void)
{
	printf("<dnn> [options]\n"
	"-w <wq_type>        ; 0=dedicated, 1=shared\n"
	"-i <num_images>     ; Number of images to process from dataset\n"
	"-t <ms timeout>     ; ms to wait for descs to complete\n"
	"-s <frag_size>      ; Size of each fragment\n"
    "-a <num_accel_grp>  ; Number of accel groups Note: Group = Scan IAA -> Select IAA -> DSA\n"
    "-c <config>    ; 0: asynchronous, dedicated poller\n"
	"               ; 1: asynchronous, single thread per accel group\n"
	"               ; 2: Synchronous\n"
	"-f                  ; enable fragmentation (no argument)\n"
    "-v                  ; enable operation verification (no argument)\n"
	"-h                  ; print this message (no argument)\n");
}

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
struct acctest_context **dsas, **scan_iaas, **select_iaas;
int num_accel = 1;
unsigned int num_desc = 0;
unsigned int num_images = 1;
const char* directory_path = "./test/images2";
uint64_t frag_size = 4096; 

void *async_thread(void *arg);
void *scan_submit(void *arg);
void *scan_wait_select_submit(void *arg);
void *select_wait_memcpy_submit(void *arg);
void *memcpy_wait(void *arg);

typedef struct {
    struct acctest_context *scan_iaa;
    struct acctest_context *select_iaa;
    struct acctest_context *dsa;
} PThreadArgs;



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

static int setup_non_fragmented_scan(void) {
    DIR *dir;
    struct dirent *ent;
    char image_path[1024];
    int rc = ACCTEST_STATUS_OK;
    int tflags = 0x1;
    int extra_flags_2 = 0x5c;
    unsigned int image_count, iaa_index;
    struct task_node **current_nodes = calloc(num_accel, sizeof(struct task_node*));

    dir = opendir(directory_path);
    if (dir == NULL) {
        perror("Failed to open directory");
        return -1;
    }
    
    info("setup scan: opcode %d tflags %#x num_desc %ld\n", IAX_OPCODE_SCAN, tflags, num_desc);

    num_desc = num_images;

    // Allocate tasks to each accelerator
    int base_tasks = num_images / num_accel;
    int extra_tasks = num_images % num_accel;
    for (int i = 0; i < num_accel; i++) {
        int current_tsk_count = base_tasks + (i < extra_tasks ? 1 : 0);
        rc = acctest_alloc_multiple_tasks(scan_iaas[i], current_tsk_count);
        if (rc != ACCTEST_STATUS_OK) {
            closedir(dir);
            return rc;
        }
        current_nodes[i] = scan_iaas[i]->multi_task_node;
    }

    iaa_index = 0; // Index to assign IAAs in round-robin fashion
    image_count = 0; 
    while ((ent = readdir(dir)) != NULL && image_count < num_images) {
        if (ent->d_type == DT_REG && strncmp(ent->d_name, "ILSVRC2017_test_", 16) == 0) {
            snprintf(image_path, sizeof(image_path), "%s/%s", directory_path, ent->d_name);
            struct task_node *tsk_node = current_nodes[iaa_index];
            tsk_node->tsk->iaa_filter_flags = (uint32_t)extra_flags_2;
            rc = init_scan_from_image(tsk_node->tsk, tflags, IAX_OPCODE_SCAN, image_path, 0, 0);
            printf("Setup node with image: %s, image size: %lu, scan_iaa: %d\n",
                    image_path, tsk_node->tsk->xfer_size, iaa_index);
            tsk_node->tsk->iaa_num_inputs = (uint32_t)tsk_node->tsk->xfer_size / 24;
            if (rc != ACCTEST_STATUS_OK) {
                closedir(dir);
                return rc;
            }
            current_nodes[iaa_index] = tsk_node->next;
            iaa_index = (iaa_index + 1) % num_accel;
            image_count++;
        }
    }

    closedir(dir);
    return ACCTEST_STATUS_OK;
}

static int setup_fragmented_scan(void) {
    DIR *dir;
    struct dirent *ent;
    char image_path[1024];
    uint64_t total_size;
    unsigned int image_count = 0; 
    int tflags = 0x1;
    int extra_flags_2 = 0x5c;
    int rc;
    struct task_node *current_node;
    uint64_t num_frags;
    uint64_t frag_start, frag_end;
    void *buffer;
    int base_tasks, extra_tasks, current_tsk_count;
    struct task_node **current_nodes = calloc(num_accel, sizeof(struct task_node*));

    dir = opendir(directory_path);
    if (dir == NULL) {
        perror("Failed to open directory");
        return -1;
    }

    info("setup scan: opcode %d tflags %#x num_desc %ld\n",
        IAX_OPCODE_SCAN, tflags, num_desc);

    // First Pass: Calculate total number of fragments for the specified number of images
    while ((ent = readdir(dir)) != NULL && image_count < num_images) {
        if (ent->d_type == DT_REG && strncmp(ent->d_name, "ILSVRC2017_test_", 16) == 0) {
            snprintf(image_path, sizeof(image_path), "%s/%s", directory_path, ent->d_name);
            read_jpeg_to_buffer(image_path, &buffer, &total_size);
            free(buffer); 
            num_desc += (total_size + frag_size - 1) / frag_size; 
            image_count++;
        }
    }
    rewinddir(dir); // Reset directory stream for the second pass

    // Allocate memory to all the task nodes, desc, completion record based on calculated num_desc
    base_tasks = num_desc / num_accel;
    extra_tasks = num_desc % num_accel;

    for (int i = 0; i < num_accel; i++) {
        current_tsk_count = base_tasks + (i < extra_tasks ? 1 : 0);
        rc = acctest_alloc_multiple_tasks(scan_iaas[i], current_tsk_count);
        if (rc != ACCTEST_STATUS_OK) {
            closedir(dir);
            return rc;
        }
    }

    for (int i = 0; i < num_accel; i++) {
        current_nodes[i] = scan_iaas[i]->multi_task_node;
    }
    image_count = 0; 
    int iaa_index = 0; // Index to assign IAAs in round-robin fashion

    // Allocate and initialize each task for all fragments
    while ((ent = readdir(dir)) != NULL && image_count < num_images) {
        if (ent->d_type == DT_REG && strncmp(ent->d_name, "ILSVRC2017_test_", 16) == 0) {
            snprintf(image_path, sizeof(image_path), "%s/%s", directory_path, ent->d_name);
            read_jpeg_to_buffer(image_path, &buffer, &total_size);
            num_frags = (total_size + frag_size - 1) / frag_size;

            for (uint64_t i = 0; i < num_frags; i++) {
                struct task_node *current_node = current_nodes[iaa_index];

                current_node->tsk->iaa_filter_flags = (uint32_t)extra_flags_2;
                frag_start = i * frag_size;
                frag_end = frag_start + frag_size;
                if (frag_end > total_size) frag_end = total_size;

                init_scan_from_image(current_node->tsk, tflags, IAX_OPCODE_SCAN, image_path, frag_start, frag_end - frag_start);
                printf("Setup node with image: %s, total size: %lu, frag_size: %lu frag: %lu, scan_iaa: %d\n",
                         image_path, total_size, current_node->tsk->xfer_size, i,iaa_index);

                current_node->tsk->iaa_num_inputs = (uint32_t)current_node->tsk->xfer_size / 24;
                current_nodes[iaa_index] = current_node->next;
                iaa_index = (iaa_index + 1) % num_accel;
            }
            free(buffer);
            image_count++; 
        }
    }
    printf("Total number of fragmented descriptors: %d\n", num_desc);

    closedir(dir);
    return ACCTEST_STATUS_OK;
}

static int setup_select(void) {
    int rc = ACCTEST_STATUS_OK;
    int tflags = 0x1;
    int extra_flags_2 = 0x5c;
    struct task_node *select_tsk_node, *scan_tsk_node;
    int base_tasks = num_desc / num_accel;
    int extra_tasks = num_desc % num_accel;
    int tasks_for_this_iaa;
    
    info("setup select: opcode %d tflags %#x num_desc %ld\n",
         IAX_OPCODE_SELECT, tflags, num_desc);

    for (int i = 0; i < num_accel; i++) {
        tasks_for_this_iaa = base_tasks + (i < extra_tasks ? 1 : 0);
        rc = acctest_alloc_multiple_tasks(select_iaas[i], tasks_for_this_iaa);
        if (rc != ACCTEST_STATUS_OK) {
            return rc; 
        }
    }

    for (int i = 0; i < num_accel; i++) {
        select_tsk_node = select_iaas[i]->multi_task_node;
        scan_tsk_node = scan_iaas[i]->multi_task_node;

        while (select_tsk_node && scan_tsk_node) {
            select_tsk_node->tsk->iaa_filter_flags = (uint32_t)extra_flags_2;
            rc = select_init(select_tsk_node->tsk, tflags, IAX_OPCODE_SELECT, scan_tsk_node->tsk->xfer_size);
            if (rc != ACCTEST_STATUS_OK)
                return rc;

            select_tsk_node = select_tsk_node->next;
            scan_tsk_node = scan_tsk_node->next;
        }
    }
    return ACCTEST_STATUS_OK;
}

static int setup_memcpy(void) {
    struct task_node *dsa_tsk_node, *scan_tsk_node;
    int rc = ACCTEST_STATUS_OK;
    int tflags = 0x1;

    int base_tasks = num_desc / num_accel;
    int extra_tasks = num_desc % num_accel;
    int tasks_for_this_dsa;

    info("setup memcpy: opcode %d tflags %#x num_desc %ld\n",
         DSA_OPCODE_MEMMOVE, tflags, num_desc);

    for (int i = 0; i < num_accel; i++) {
        dsas[i]->is_batch = 0;  
        tasks_for_this_dsa = base_tasks + (i < extra_tasks ? 1 : 0);
        rc = acctest_alloc_multiple_tasks(dsas[i], tasks_for_this_dsa);
        if (rc != ACCTEST_STATUS_OK)
            return rc;
    }

    for (int i = 0; i < num_accel; i++) {
        dsa_tsk_node = dsas[i]->multi_task_node;
        scan_tsk_node = scan_iaas[i]->multi_task_node;

        while (dsa_tsk_node && scan_tsk_node) {
            rc = memcpy_init(dsa_tsk_node->tsk, tflags, DSA_OPCODE_MEMMOVE, scan_tsk_node->tsk->xfer_size);
            if (rc != ACCTEST_STATUS_OK)
                return rc;
            dsa_tsk_node = dsa_tsk_node->next;
            scan_tsk_node = scan_tsk_node->next;
        }
    }
    return rc;
}

// Note, this uses a single accelerator, -n <num accel> must be 1
static int sync_all_ops(void) {
    struct task_node *scan_tsk_node, *select_tsk_node, *dsa_tsk_node;
    int rc = ACCTEST_STATUS_OK;

    scan_tsk_node = scan_iaas[0]->multi_task_node;
    select_tsk_node = select_iaas[0]->multi_task_node;
    dsa_tsk_node = dsas[0]->multi_task_node;

    while (scan_tsk_node) {
        // Prep Scan
        rc = iaa_scan_prep_sub_task_node(scan_iaas[0], scan_tsk_node);
        if (rc != ACCTEST_STATUS_OK)
            return rc;
        rc = iaa_wait_scan(scan_iaas[0], scan_tsk_node->tsk);
        if (rc != ACCTEST_STATUS_OK)
            return rc;
        
        // Prep select
        select_tsk_node->tsk->src1 = scan_tsk_node->tsk->src1;
		select_tsk_node->tsk->xfer_size = scan_tsk_node->tsk->xfer_size;
		select_tsk_node->tsk->src2 = scan_tsk_node->tsk->dst1;
		select_tsk_node->tsk->iaa_num_inputs = (uint32_t)select_tsk_node->tsk->xfer_size / 24;
		// printf("scan output size: %u\n", scan_tsk_node->tsk->comp->iax_output_size);
        rc = iaa_select_prep_sub_tsk_node(select_iaas[0], select_tsk_node);
        if (rc != ACCTEST_STATUS_OK)
            return rc;
        rc = iaa_wait_select(select_iaas[0], select_tsk_node->tsk);
        if (rc != ACCTEST_STATUS_OK)
            return rc;
        
        // Perform Shuffle
        shuffle_elements(select_tsk_node->tsk->dst1, select_tsk_node->tsk->comp->iax_output_size);
		
        // Prep memcpy
        dsa_tsk_node->tsk->src1 = select_tsk_node->tsk->dst1;
		dsa_tsk_node->tsk->xfer_size = select_tsk_node->tsk->comp->iax_output_size;
        rc = dsa_memcpy_prep_sub_task_node(dsas[0], dsa_tsk_node);
		if (rc != ACCTEST_STATUS_OK)
            return rc;
        scan_tsk_node = scan_tsk_node->next;
        select_tsk_node = select_tsk_node->next;
        dsa_tsk_node = dsa_tsk_node->next;
    }
    return rc;

}

void *async_thread(void *arg) {
    PThreadArgs *args = (PThreadArgs *)arg;
    struct task_node *scan_tsk_node, *select_tsk_node;
    struct task_node *dsa_tsk_node;
    int rc = iaa_scan_multi_task_nodes(args->scan_iaa);
    if (rc != ACCTEST_STATUS_OK)
        pthread_exit((void *)(intptr_t)rc);
    scan_tsk_node = args->scan_iaa->multi_task_node;
    select_tsk_node = args->select_iaa->multi_task_node;
    while (scan_tsk_node) {
        rc = iaa_wait_scan(args->scan_iaa, scan_tsk_node->tsk);
        if (rc != ACCTEST_STATUS_OK)
            pthread_exit((void *)(intptr_t)rc);
        select_tsk_node->tsk->src1 = scan_tsk_node->tsk->src1;
        select_tsk_node->tsk->xfer_size = scan_tsk_node->tsk->xfer_size;
        select_tsk_node->tsk->src2 = scan_tsk_node->tsk->dst1;
        select_tsk_node->tsk->iaa_num_inputs = (uint32_t)select_tsk_node->tsk->xfer_size / 24;
        rc = iaa_select_prep_sub_tsk_node(args->select_iaa, select_tsk_node);
        if (rc != ACCTEST_STATUS_OK)
            pthread_exit((void *)(intptr_t)rc);
        scan_tsk_node = scan_tsk_node->next;
        select_tsk_node = select_tsk_node->next;
    }

    select_tsk_node = args->select_iaa->multi_task_node;
    dsa_tsk_node = args->dsa->multi_task_node;
    while (select_tsk_node) {
        rc = iaa_wait_select(args->select_iaa, select_tsk_node->tsk);
        if (rc != ACCTEST_STATUS_OK)
            pthread_exit((void *)(intptr_t)rc);
        shuffle_elements(select_tsk_node->tsk->dst1, select_tsk_node->tsk->comp->iax_output_size);
        dsa_tsk_node->tsk->src1 = select_tsk_node->tsk->dst1;
        dsa_tsk_node->tsk->xfer_size = select_tsk_node->tsk->comp->iax_output_size;
        rc = dsa_memcpy_prep_sub_task_node(args->dsa, dsa_tsk_node);
        if (rc != ACCTEST_STATUS_OK) {
            pthread_exit((void *)(intptr_t)rc);
        }
        select_tsk_node = select_tsk_node->next;
        dsa_tsk_node = dsa_tsk_node->next;
    }
    dsa_tsk_node = args->dsa->multi_task_node;
    while(dsa_tsk_node) {
        rc = dsa_wait_memcpy(args->dsa, dsa_tsk_node->tsk);
        if (rc != ACCTEST_STATUS_OK) {
            pthread_exit((void *)(intptr_t)rc);
        }
        dsa_tsk_node = dsa_tsk_node->next;
    }
    pthread_exit((void *)ACCTEST_STATUS_OK);
}

void *scan_submit(void *arg) {
    PThreadArgs *args = (PThreadArgs *)arg;
    int rc = iaa_sub_scan_multi_task_nodes(args->scan_iaa);
    if (rc != ACCTEST_STATUS_OK)
        pthread_exit((void *)(intptr_t)rc);
    pthread_exit((void *)ACCTEST_STATUS_OK);
}

void *scan_wait_select_submit(void *arg) {
    PThreadArgs *args = (PThreadArgs *)arg;
    struct task_node *scan_tsk_node = args->scan_iaa->multi_task_node;
    struct task_node *select_tsk_node = args->select_iaa->multi_task_node;
    int rc;

    while (scan_tsk_node) {
        rc = iaa_wait_scan(args->scan_iaa, scan_tsk_node->tsk);
        if (rc != ACCTEST_STATUS_OK)
            pthread_exit((void *)(intptr_t)rc);
        select_tsk_node->tsk->src1 = scan_tsk_node->tsk->src1;
        select_tsk_node->tsk->xfer_size = scan_tsk_node->tsk->xfer_size;
        select_tsk_node->tsk->src2 = scan_tsk_node->tsk->dst1;
        select_tsk_node->tsk->iaa_num_inputs = (uint32_t)select_tsk_node->tsk->xfer_size / 24;
        rc = iaa_select_prep_sub_tsk_node(args->select_iaa, select_tsk_node);
        if (rc != ACCTEST_STATUS_OK)
            pthread_exit((void *)(intptr_t)rc);
        scan_tsk_node = scan_tsk_node->next;
        select_tsk_node = select_tsk_node->next;
    }

    pthread_exit((void *)ACCTEST_STATUS_OK);
}

void *select_wait_memcpy_submit(void *arg) {
    PThreadArgs *args = (PThreadArgs *)arg;
    struct task_node *select_tsk_node = args->select_iaa->multi_task_node;
    struct task_node *dsa_tsk_node = args->dsa->multi_task_node;
    int rc;

    while (select_tsk_node) {
        rc = iaa_wait_select(args->select_iaa, select_tsk_node->tsk);
        if (rc != ACCTEST_STATUS_OK)
            pthread_exit((void *)(intptr_t)rc);
        shuffle_elements(select_tsk_node->tsk->dst1, select_tsk_node->tsk->comp->iax_output_size);
        dsa_tsk_node->tsk->src1 = select_tsk_node->tsk->dst1;
        dsa_tsk_node->tsk->xfer_size = select_tsk_node->tsk->comp->iax_output_size;
        rc = dsa_memcpy_prep_sub_task_node(args->dsa, dsa_tsk_node);
        if (rc != ACCTEST_STATUS_OK) {
            pthread_exit((void *)(intptr_t)rc);
        }
        select_tsk_node = select_tsk_node->next;
        dsa_tsk_node = dsa_tsk_node->next;
    }

    pthread_exit((void *)ACCTEST_STATUS_OK);
}


void *memcpy_wait(void *arg) {
    PThreadArgs *args = (PThreadArgs *)arg;
    struct task_node *dsa_task_node = args->dsa->multi_task_node;
    int rc;
    while(dsa_task_node) {
        rc = dsa_wait_memcpy(args->dsa, dsa_task_node->tsk);
        if (rc != ACCTEST_STATUS_OK) {
            pthread_exit((void *)(intptr_t)rc);
        }
        dsa_task_node = dsa_task_node->next;
    }
    pthread_exit((void *)ACCTEST_STATUS_OK);
}



int main(int argc, char *argv[])
{
	int rc = 0;
	int wq_type = SHARED;
	int opt;
	int tflags = TEST_FLAGS_BOF;
	int dsa_dev_id = 2;
	int scan_iaa_dev_id = 1;
    int select_iaa_dev_id = 3;
	struct timespec e2e_times[2];
	double e2e_time_s = 0;
	// int rc0, rc1, rc2, rc3;
    int is_frag = 0;
    int verify = 0;
    pthread_t *scanSubmitThreads;
    pthread_t *scanSelectThreads;
    pthread_t *selectDsaThreads;
    pthread_t *memcpyThreads;
    pthread_t *asyncThreads;
    PThreadArgs *scanArgs;
    PThreadArgs *scanSelectArgs;
    PThreadArgs *selectDsaArgs;
    PThreadArgs *memcpyArgs;
    PThreadArgs *asyncArgs;
    int config = 0;

	while ((opt = getopt(argc, argv, "w:i:t:s:a:c:fvh")) != -1) {
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
        case 's':
			frag_size = strtoul(optarg, NULL, 0);
			break;
        case 'a':
			num_accel = strtoul(optarg, NULL, 0);
			break;
        case 'c':
			config = strtoul(optarg, NULL, 0);
			break;
        case 'f':
			is_frag = 1;
			break;
        case 'v':
			verify = 1;
			break;
        case 'h':
			usage();
			exit(0);
		default:
			break;
		}
	}

    // Allocate memory for device context arrays and thread management
    scan_iaas = malloc(num_accel * sizeof(struct acctest_context*));
    select_iaas = malloc(num_accel * sizeof(struct acctest_context*));
    dsas = malloc(num_accel * sizeof(struct acctest_context*));

    // iaa scan device setup
    for(int i = 0; i < num_accel; i++) {
        scan_iaas[i] = acctest_init(tflags);
        scan_iaas[i]->dev_type = ACCFG_DEVICE_IAX;

        if (!scan_iaas[i])
            return -ENOMEM;

        rc = acctest_alloc(scan_iaas[i], wq_type, scan_iaa_dev_id, i);
        if (rc < 0)
            return -ENOMEM;
    }
    
    // iaa select device setup
    for(int i = 0; i < num_accel; i++) {
        select_iaas[i] = acctest_init(tflags);
        select_iaas[i]->dev_type = ACCFG_DEVICE_IAX;

        if (!select_iaas[i])
            return -ENOMEM;

        rc = acctest_alloc(select_iaas[i], wq_type, select_iaa_dev_id, i);
        if (rc < 0)
            return -ENOMEM;
    }

    for(int i = 0; i < num_accel; i++) {
        // DSA setup
        dsas[i] = acctest_init(tflags);
        dsas[i]->dev_type = ACCFG_DEVICE_DSA;

        if (!dsas[i])
            return -ENOMEM;

        rc = acctest_alloc(dsas[i], wq_type, dsa_dev_id, i);
        if (rc < 0)
            return -ENOMEM;
    }

    if(is_frag) {
        rc = setup_fragmented_scan();
    } else {
        rc = setup_non_fragmented_scan();
    }
    if (rc != ACCTEST_STATUS_OK) {
        goto error;
    }
	rc = setup_select();
    if (rc != ACCTEST_STATUS_OK) {
        goto error;
    }
	rc = setup_memcpy();
    if (rc != ACCTEST_STATUS_OK) {
        goto error;
    }

    printf("Starting operations\n");
    printf("Num accel: %d\n", num_accel);

    switch(config) {
        case 0:
            scanSubmitThreads = malloc(num_accel * sizeof(pthread_t));
            scanSelectThreads = malloc(num_accel * sizeof(pthread_t));
            selectDsaThreads = malloc(num_accel * sizeof(pthread_t));
            memcpyThreads = malloc(num_accel * sizeof(pthread_t));
            scanArgs = malloc(num_accel * sizeof(PThreadArgs));
            scanSelectArgs = malloc(num_accel * sizeof(PThreadArgs));
            selectDsaArgs = malloc(num_accel * sizeof(PThreadArgs));
            memcpyArgs = malloc(num_accel * sizeof(PThreadArgs));

            clock_gettime(CLOCK_MONOTONIC, &e2e_times[0]);
            // Thread creation loop
            for (int i = 0; i < num_accel; i++) {
                scanArgs[i].scan_iaa = scan_iaas[i];
                pthread_create(&scanSubmitThreads[i], NULL, scan_submit, &scanArgs[i]);

                scanSelectArgs[i].scan_iaa = scan_iaas[i];
                scanSelectArgs[i].select_iaa = select_iaas[i];
                pthread_create(&scanSelectThreads[i], NULL, scan_wait_select_submit, &scanSelectArgs[i]);

                selectDsaArgs[i].select_iaa = select_iaas[i];
                selectDsaArgs[i].dsa = dsas[i];
                pthread_create(&selectDsaThreads[i], NULL, select_wait_memcpy_submit, &selectDsaArgs[i]);

                memcpyArgs[i].dsa = dsas[i];
                pthread_create(&memcpyThreads[i], NULL, memcpy_wait, &memcpyArgs[i]);
            }

            // Thread joining loop
            for (int i = 0; i < num_accel; i++) {
                pthread_join(scanSubmitThreads[i], NULL);
                pthread_join(scanSelectThreads[i], NULL);
                pthread_join(selectDsaThreads[i], NULL);
                pthread_join(memcpyThreads[i], NULL);
            }
            break;
        case 1:
            asyncThreads = malloc(num_accel * sizeof(pthread_t));
            asyncArgs = malloc(num_accel * sizeof(PThreadArgs));
            clock_gettime(CLOCK_MONOTONIC, &e2e_times[0]);
            for(int i = 0; i < num_accel; i++) {
                asyncArgs[i].scan_iaa = scan_iaas[i];
                asyncArgs[i].select_iaa = select_iaas[i];
                asyncArgs[i].dsa = dsas[i];
                pthread_create(&asyncThreads[i], NULL, async_thread, &asyncArgs[i]);
            }
            for (int i = 0; i < num_accel; i++) {
                pthread_join(asyncThreads[i], NULL);
            }
            break;
        case 2:
            clock_gettime(CLOCK_MONOTONIC, &e2e_times[0]);
            rc = sync_all_ops();
            if (rc != ACCTEST_STATUS_OK)
                goto error;
            break;
        default:
            goto error;

    }
	clock_gettime(CLOCK_MONOTONIC, &e2e_times[1]);
	e2e_time_s = (e2e_times[1].tv_sec - e2e_times[0].tv_sec) + 
                   (e2e_times[1].tv_nsec - e2e_times[0].tv_nsec) / 1000000000.0;
	// if (rc0 != ACCTEST_STATUS_OK || rc1 != ACCTEST_STATUS_OK 
	// 	|| rc2 != ACCTEST_STATUS_OK || rc3 != ACCTEST_STATUS_OK)
	// 	goto error;

	if (verify) {
        for (int idx = 0; idx < num_accel; idx++) {
            rc = task_result_verify_task_nodes(scan_iaas[idx], 0);
            if (rc != ACCTEST_STATUS_OK) {
                fprintf(stderr, "Verification failed for scan_iaa[%d]\n", idx);
                goto error;
            }
            rc = task_result_verify_task_nodes(select_iaas[idx], 0);
            if (rc != ACCTEST_STATUS_OK) {
                fprintf(stderr, "Verification failed for select_iaa[%d]\n", idx);
                goto error;
            }
            rc = task_result_verify_task_nodes(dsas[idx], 0);
            if (rc != ACCTEST_STATUS_OK) {
                fprintf(stderr, "Verification failed for dsa[%d]\n", idx);
                goto error;
            }
        }
        printf("All ops verified successfully.\n");
    }

	printf("Total Latency: %f s\n", e2e_time_s);
	
	// acctest_free_task(scan_iaa);
	// acctest_free_task(dsa);
 error:
	// acctest_free(scan_iaa);
	// acctest_free(dsa);
	return rc;
}