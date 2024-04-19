#include "algorithms/iaa_compress.h"

static inline int increment_comp_if_tsk_valid(struct task_node *tsk_node, struct completion_record *comp){
  if(tsk_node){
    if(comp->status){
      tsk_node = tsk_node->next;
      if(tsk_node){
        comp = tsk_node->tsk->comp;
      }
      return 1;
    } else {
      return 0;
    }
  }
  else{
    return 2;
  }
}


int multi_iaa_test(int num_iaas, int tflags, int wq_type, int dev_id, int wq_id, size_t buf_size, int num_desc)
{
  int rc;
  struct acctest_context **iaa;
  iaa = malloc(num_iaas * sizeof(struct acctest_context *));

  for(int i=0; i<num_iaas; i++){
    iaa[i] = acctest_init(tflags);
    iaa[i]->dev_type = ACCFG_DEVICE_IAX;
    if (!iaa[i])
      return -ENOMEM;

    rc = acctest_alloc(iaa[i], wq_type, dev_id, wq_id);
    if (rc < 0)
      return -ENOMEM;

    if (buf_size > iaa[i]->max_xfer_size) {
      err("invalid transfer size: %lu\n", buf_size);
      return -EINVAL;
    }
  }

  for(int i=0; i<num_iaas; i++){
    rc = init_iaa_task_nodes(iaa[i], buf_size, tflags, num_desc);
    if (rc != ACCTEST_STATUS_OK)
      return rc;
  }

  struct task_node *iaa_tsk_node[num_iaas];
  struct completion_record *next_iaa_comp[num_iaas];

  /* Submission / work distribution across all iaa instances*/
  for(int i=0; i<num_iaas; i++){
    iaa_tsk_node[i] = iaa[i]->multi_task_node;
    while(iaa_tsk_node[i]){
      iaa_prep_sub_task_node(iaa[i], iaa_tsk_node[i]);
      iaa_tsk_node[i] = iaa_tsk_node[i]->next;
    }
    iaa_tsk_node[i] = iaa[i]->multi_task_node;
    next_iaa_comp[i] = iaa_tsk_node[i]->tsk->comp;
  }

  /* Many IAAs to Poll, don't block on any one IAA */

  int complete = 0;
  while (!complete) {
    complete = 1;
    for(int i=0; i<num_iaas; i++){
      if(iaa_tsk_node[i]){
        complete = 0;
        if(next_iaa_comp[i]->status){
          iaa_tsk_node[i] = iaa_tsk_node[i]->next;
          if(iaa_tsk_node[i]){
            next_iaa_comp[i] = iaa_tsk_node[i]->tsk->comp;
          }
        }
      }
    }
  }

  clock_gettime(CLOCK_MONOTONIC, &times[1]);

}

typedef struct{
  int tflags;
  int wq_type;
  int dev_id;
  int wq_id;
  size_t buf_size;
  int num_desc;
} single_submitter_poller_args;

int single_iaa_test( void *arg){
  /* allocate this iaa's task nodes */
  int rc;
  struct acctest_context *iaa;
  single_submitter_poller_args *args = (single_submitter_poller_args *)arg;

  int tflags = args->tflags;
  int wq_type = args->wq_type;
  int dev_id = args->dev_id;
  int wq_id = args->wq_id;
  size_t buf_size = args->buf_size;
  int num_desc = args->num_desc;
  struct task_node *iaa_tsk_node;

  /* setup */
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
  init_iaa_task_nodes(iaa, buf_size, tflags, num_desc);
  iaa_tsk_node = iaa->multi_task_node;

  while(!test_started){}
  /* submit */
  while(iaa_tsk_node){
    iaa_prep_sub_task_node(iaa, iaa_tsk_node);
    iaa_tsk_node = iaa_tsk_node->next;
  }

  /* Reset and poll in order */
  iaa_tsk_node = iaa->multi_task_node;
  struct completion_record *next_iaa_comp = iaa_tsk_node->tsk->comp;
  while(iaa_tsk_node){
    if(next_iaa_comp->status){
      iaa_tsk_node = iaa_tsk_node->next;
      if(iaa_tsk_node){
        next_iaa_comp = iaa_tsk_node->tsk->comp;
      }
    }
  }
  pthread_exit((void *)ACCTEST_STATUS_OK);

}

int multi_iaa_bandwidth(int num_wqs, int num_descs, int buf_size){
  single_submitter_poller_args *args = malloc(num_wqs * sizeof(single_submitter_poller_args));
  pthread_t threads[num_wqs];
  for(int i=0; i<num_wqs; i++){
    args[i].tflags = 0;
    args[i].wq_type = 0;
    args[i].dev_id = 1;
    args[i].wq_id = i;
    args[i].buf_size = buf_size;
    args[i].num_desc = num_descs;
    pthread_create(&(threads[i]),NULL,single_iaa_test,(void *)&(args[i]));
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(i+1, &cpuset);
    pthread_setaffinity_np(threads[i], sizeof(cpu_set_t), &cpuset);
  }
  clock_gettime(CLOCK_MONOTONIC, &times[0]);
  test_started = 1;
  for(int i=0; i<num_wqs; i++){
    pthread_join(threads[i],NULL);
  }
  clock_gettime(CLOCK_MONOTONIC, &times[1]);

}

int reset_test_ctrs(){
  intermediate_host_ops_complete = 0;
  finalHostOpCtr = 0;
}


int single_dsa_test( void *arg){
  /* allocate this iaa's task nodes */
  int rc;
  struct acctest_context *dsa;
  single_submitter_poller_args *args = (single_submitter_poller_args *)arg;

  int tflags = args->tflags;
  int wq_type = args->wq_type;
  int dev_id = args->dev_id;
  int wq_id = args->wq_id;
  size_t buf_size = args->buf_size;
  int num_desc = args->num_desc;
  struct task_node *dsa_tsk_node;

  /* setup */
  dsa = acctest_init(tflags);
  dsa->dev_type = ACCFG_DEVICE_DSA;
  if (!dsa)
    return -ENOMEM;
  rc = acctest_alloc(dsa, wq_type, dev_id, wq_id);
  if (rc < 0){
    exit(-ENODEV);
  }
  if (buf_size > dsa->max_xfer_size) {
    err("invalid transfer size: %lu\n", buf_size);
    return -EINVAL;
  }
  init_dsa_task_nodes(dsa, buf_size, tflags, num_desc);
  dsa_tsk_node = dsa->multi_task_node;

  while(!test_started){}
  /* submit */
  while(dsa_tsk_node){
    dsa_memcpy_submit_task_nodes(dsa);
    dsa_tsk_node = dsa_tsk_node->next;
  }

  /* Reset and poll in order */
  dsa_tsk_node = dsa->multi_task_node;
  struct completion_record *next_dsa_comp = dsa_tsk_node->tsk->comp;
  while(dsa_tsk_node){
    if(next_dsa_comp->status){
      dsa_tsk_node = dsa_tsk_node->next;
      if(dsa_tsk_node){
        next_dsa_comp = dsa_tsk_node->tsk->comp;
      }
    }
  }
  pthread_exit((void *)dsa->multi_task_node);

}

int multi_dsa_bandwidth(int num_wqs, int num_descs, int buf_size){
  single_submitter_poller_args *args = malloc(num_wqs * sizeof(single_submitter_poller_args));
  pthread_t threads[num_wqs];
  for(int i=0; i<num_wqs; i++){
    args[i].tflags = 0;
    args[i].wq_type = 0;
    args[i].dev_id = 0;
    args[i].wq_id = i;
    args[i].buf_size = buf_size;
    args[i].num_desc = num_descs;
    pthread_create(&(threads[i]),NULL,single_dsa_test,(void *)&(args[i]));
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(i+1, &cpuset);
    pthread_setaffinity_np(threads[i], sizeof(cpu_set_t), &cpuset);
  }
  clock_gettime(CLOCK_MONOTONIC, &times[0]);
  test_started = 1;
  struct task_node *dsa_tsk_node[num_wqs];
  for(int i=0; i<num_wqs; i++){
    pthread_join(threads[i],&(dsa_tsk_node[i]));
  }
  clock_gettime(CLOCK_MONOTONIC, &times[1]);

  for(int i=0; i<num_wqs; i++){
    struct task_node *tsk_node = dsa_tsk_node[i];
    while(tsk_node){
      task_result_verify(tsk_node,0);
      tsk_node = tsk_node->next;
    }
  }
}

static inline void dsa_submit_and_wait(struct acctest_context *dsa, int wq_depth){
  int rc;
  struct task_node *tsk_node = dsa->multi_task_node;

  /* Prep all task nodes */
  while (tsk_node) {
		tsk_node->tsk->dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
		if ((tsk_node->tsk->test_flags & TEST_FLAGS_BOF) && dsa->bof)
			tsk_node->tsk->dflags |= IDXD_OP_FLAG_BOF;
		dsa_prep_memcpy(tsk_node->tsk);
		tsk_node = tsk_node->next;
	}

  tsk_node = dsa->multi_task_node;
  struct task_node *start_tsk_node = tsk_node;
  /* Submit up to the work queue depth and collect responses in a loop */
  int submitted = 0;
  int retrieved = 0;
  while(tsk_node && submitted < wq_depth){
    if (tsk_node->tsk->test_flags & TEST_FLAGS_CPFLT)
			madvise(tsk_node->tsk->comp, 4096, MADV_DONTNEED);
		acctest_desc_submit(dsa, tsk_node->tsk->desc);
    tsk_node = tsk_node->next;
    submitted++;
  }
  tsk_node = start_tsk_node;
  while(tsk_node && retrieved < wq_depth){
    dsa_wait_memcpy(dsa, tsk_node->tsk);
    // if (ACCTEST_STATUS_OK != task_result_verify_memcpy(tsk_node->tsk, 0)){
    //   printf("Fail\n");
    //   exit(-1);
    // }
    tsk_node = tsk_node->next;
    retrieved ++;
  }

  return 0;
}

static inline void iaa_submit_and_wait(struct acctest_context *iaa, int wq_depth){
  int rc;
  struct task_node *tsk_node = iaa->multi_task_node;

  /* Prep all task nodes */
  while (tsk_node) {
		tsk_node->tsk->dflags |= (IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR);
		if ((tsk_node->tsk->test_flags & TEST_FLAGS_BOF) && iaa->bof)
			tsk_node->tsk->dflags |= IDXD_OP_FLAG_BOF;

		tsk_node->tsk->dflags |= (IDXD_OP_FLAG_WR_SRC2_CMPL | IDXD_OP_FLAG_RD_SRC2_AECS);
		tsk_node->tsk->iaa_src2_xfer_size = IAA_COMPRESS_AECS_SIZE;

		memcpy(tsk_node->tsk->src2, (void *)iaa_compress_aecs, IAA_COMPRESS_AECS_SIZE);

		tsk_node->tsk->iaa_compr_flags = (IDXD_COMPRESS_FLAG_EOB_BFINAL |
						  IDXD_COMPRESS_FLAG_FLUSH_OUTPUT);
		tsk_node->tsk->iaa_max_dst_size = IAA_COMPRESS_MAX_DEST_SIZE;

		iaa_prep_compress(tsk_node->tsk);
		tsk_node = tsk_node->next;
	}

  tsk_node = iaa->multi_task_node;
  struct task_node *start_tsk_node = tsk_node;
  /* Submit up to the work queue depth and collect responses in a loop */
  int submitted = 0;
  int retrieved = 0;
  while(tsk_node && submitted < wq_depth){
		acctest_desc_submit(iaa, tsk_node->tsk->desc);
    tsk_node = tsk_node->next;
    submitted++;
  }
  tsk_node = start_tsk_node;
  while(tsk_node && retrieved < wq_depth){
    iaa_wait_compress(iaa, tsk_node->tsk);
    // if (ACCTEST_STATUS_OK != task_result_verify_memcpy(tsk_node->tsk, 0)){
    //   printf("Fail\n");
    //   exit(-1);
    // }
    tsk_node = tsk_node->next;
    retrieved ++;
  }

  return 0;
}

int dsa_single_thread_submit_and_collect(void *args) {
  ThreadArgs *threadArgs = (ThreadArgs *)args;
  int num_descs = threadArgs->num_descs;
  int buf_size = threadArgs->buf_size;
  int wq_id = threadArgs->wq_id;
  struct acctest_context *dsa;
  struct task_node *dsa_tsk_node;
	int rc = ACCTEST_STATUS_OK;
	int tflags = 0x1;
  int wq_depth = 32;

  dsa = acctest_init(tflags);
  rc = acctest_alloc(dsa, 0, 0, wq_id);
  if(ACCTEST_STATUS_OK != rc){
    printf("Failed to allocate DSA\n");
    exit(-1);
  }
  dsa->is_batch = 0;
  rc = acctest_alloc_multiple_tasks(dsa, wq_depth);
  if (rc != ACCTEST_STATUS_OK)
		return rc;
  printf("Allocated tasks\n");

  dsa_tsk_node = dsa->multi_task_node;
	while (dsa_tsk_node) {
		dsa_tsk_node->tsk->xfer_size = buf_size;

		rc = dsa_init_task(dsa_tsk_node->tsk, tflags, DSA_OPCODE_MEMMOVE, buf_size);
		if (rc != ACCTEST_STATUS_OK)
			return rc;
		dsa_tsk_node = dsa_tsk_node->next;
	}
  printf("Starting test\n");
  int iter = 1;
  if (num_descs > wq_depth){
    iter = num_descs / wq_depth;
  } else{
    wq_depth = num_descs;
  }
  printf("wq_depth: %d\n", wq_depth);
  printf("iter: %d\n", iter);

  clock_gettime(CLOCK_MONOTONIC, &times[0]);
  for(int i=0; i<num_iter; i++){
    for(int i=0; i<iter; i++)
      dsa_submit_and_wait(dsa,wq_depth);
  }
  clock_gettime(CLOCK_MONOTONIC, &times[1]);

  uint64_t nanos = (times[1].tv_sec - times[0].tv_sec) * 1000000000 + times[1].tv_nsec - times[0].tv_nsec;
  printf("WQ: %d NumDescs: %d BufSize: %d Throughput: %f GB/s\n",
    wq_id, num_descs, buf_size, (double)buf_size * num_descs * num_iter / nanos);
  acctest_free_task(dsa);
  acctest_free(dsa);
}

int iaa_single_thread_submit_and_collect(void *args) {
  ThreadArgs *threadArgs = (ThreadArgs *)args;
  int num_descs = threadArgs->num_descs;
  int buf_size = threadArgs->buf_size;
  int wq_id = threadArgs->wq_id;
  struct acctest_context *iaa;
  struct task_node *iaa_tsk_node;
	int rc = ACCTEST_STATUS_OK;
	int tflags = 0x1;
  int wq_depth = 32;
  int wq_type = DEDICATED;
  int dev_id = 1;

  iaa = acctest_init(tflags);
  iaa->dev_type = ACCFG_DEVICE_IAX;
  if (!iaa)
		return -ENOMEM;
  rc = acctest_alloc(iaa, wq_type, dev_id, wq_id);
  if (rc < 0)
		return -ENOMEM;
  rc = acctest_alloc_multiple_tasks(iaa, wq_depth);
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

  clock_gettime(CLOCK_MONOTONIC, &times[0]);
  int iter = 1;
  if (num_descs > wq_depth){
    iter = num_descs / wq_depth;
  } else{
    wq_depth = num_descs;
  }
  printf("wq_depth: %d\n", wq_depth);
  printf("iter: %d\n", iter);
  for(int i=0; i<num_iter; i++){
    for(int i=0; i<num_descs / wq_depth; i++)
      iaa_submit_and_wait(iaa,wq_depth);
  }
  clock_gettime(CLOCK_MONOTONIC, &times[1]);

  uint64_t nanos = (times[1].tv_sec - times[0].tv_sec) * 1000000000 + times[1].tv_nsec - times[0].tv_nsec;
  printf("WQ: %d NumDescs: %d BufSize: %d Throughput: %f GB/s\n",
    wq_id, num_descs, buf_size, (double)buf_size * num_descs * num_iter / nanos);
  acctest_free_task(iaa);
  acctest_free(iaa);
}