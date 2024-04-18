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

int single_dsa_poller(struct task_node * dsa_tsk_node){
  /* generate a full queue length set of tasks */
  struct completion_record *next_dsa_comp = dsa_tsk_node->tsk->comp;
  while(dsa_tsk_node){
    if(next_dsa_comp->status){
      dsa_tsk_node = dsa_tsk_node->next;
      if(dsa_tsk_node){
        next_dsa_comp = dsa_tsk_node->tsk->comp;
      }
    }
    if (ACCTEST_STATUS_OK != task_result_verify(dsa_tsk_node, 0)){
      printf("Error");
      return ACCTEST_STATUS_FAIL;
    }
  }
}

int single_dsa_submitter( void *arg){
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
}

int multi_dsa_dedicated_poller_bandwidth(int num_wqs, int num_descs, int buf_size){
  single_submitter_poller_args *args = malloc(num_wqs * sizeof(single_submitter_poller_args));
  pthread_t submitter_threads[num_wqs];
  pthread_t poller_threads[num_wqs];
  for(int i=0; i<num_wqs; i++){
    args[i].tflags = 0;
    args[i].wq_type = 0;
    args[i].dev_id = 0;
    args[i].wq_id = i;
    args[i].buf_size = buf_size;
    args[i].num_desc = num_descs;
    cpu_set_t cpuset;

    pthread_create(&(submitter_threads[i]),NULL,single_dsa_submitter,(void *)&(args[i]));
    pthread_setaffinity_np(submitter_threads[i], sizeof(cpu_set_t), &cpuset);
    CPU_ZERO(&cpuset);
    CPU_SET(i+1, &cpuset);

    pthread_create(&(poller_threads[i]), NULL, single_dsa_poller, (void *)&(args[i]));
    pthread_setaffinity_np(poller_threads[i], sizeof(cpu_set_t), &cpuset);
    CPU_ZERO(&cpuset);
    CPU_SET(i+1, &cpuset);
  }
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