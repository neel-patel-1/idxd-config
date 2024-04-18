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

static inline void submit_and_wait(struct acctest_context *dsa){
  int rc;
  struct task_node *tsk_node = dsa->multi_task_node;
  while(tsk_node){
    dsa_memcpy_submit_task_nodes(dsa);
    tsk_node = tsk_node->next;
  }

  return 0;
}

typedef struct{
  int num_descs;
  int buf_size;
  int wq_id;
} test_params;

int single_thread_submit_and_collect(void *arg){
  test_params *params = (test_params *)arg;
  int num_descs = params->num_descs, buf_size = params->buf_size, wq_id = params->wq_id;
  struct acctest_context *dsa;
  struct task_node *dsa_tsk_node;
  struct timespec single_thread_time[2];
	int rc = ACCTEST_STATUS_OK;
	int tflags = 0x1;
  int wq_depth = 32;

  dsa = acctest_init(tflags);
  rc = acctest_alloc(dsa, 0, 0, wq_id);
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

  while(!test_started){}
  clock_gettime(CLOCK_MONOTONIC, &single_thread_time[0]);
  for(int i=0; i<num_iter; i++){
    for(int i=0; i<num_descs / wq_depth; i++)
      submit_and_wait(dsa);
  }
  clock_gettime(CLOCK_MONOTONIC, &single_thread_time[1]);

  uint64_t nanos = (single_thread_time[1].tv_sec - single_thread_time[0].tv_sec) * 1000000000 + single_thread_time[1].tv_nsec - single_thread_time[0].tv_nsec;
  printf("WQ: %d NumDescs: %d BufSize: %d Throughput: %f GB/s\n",
    wq_id, num_descs, buf_size, ((double)buf_size * num_descs * num_iter) / nanos);
  acctest_free_task(dsa);
  acctest_free(dsa);
}

int multiple_single_submitter_poller_dsas(){
  int num_wqs = 4;
  int num_descs;
  pthread_t threads[num_wqs];
  test_started = 1;
  for(int buf_size = 1024 * 1024; buf_size >= 1024; buf_size /= 2){
    printf("BufSize: %d NumDescs: %d\n", buf_size, num_descs);
    num_descs = (1024 * 1024) / buf_size;
    for(int i=0; i< num_wqs; i++){
      test_params *params = malloc(sizeof(test_params));
      params->num_descs = num_descs;
      params->buf_size = buf_size;
      params->wq_id = i;
      single_thread_submit_and_collect((void *)params);
      return;
    }
  }


  // for(int i=0; i<num_wqs; i++){
  //   test_params *params = malloc(sizeof(test_params));
  //   params->num_descs = num_descs;
  //   params->buf_size = buf_size;
  //   params->wq_id = i;
  //   pthread_create(&(threads[i]),NULL,single_thread_submit_and_collect,(void *)params);
  //   cpu_set_t cpuset;
  //   CPU_ZERO(&cpuset);
  //   CPU_SET(i+1, &cpuset);
  //   pthread_setaffinity_np(threads[i], sizeof(cpu_set_t), &cpuset);
  // }
  // for(int i=0; i<num_wqs; i++){
  //   pthread_join(threads[i],NULL);
  // }
}