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
  clock_gettime(CLOCK_MONOTONIC, &times[0]);
  for(int i=0; i<num_iaas; i++){
    iaa_tsk_node[i] = iaa[i]->multi_task_node;
    while(iaa_tsk_node[i]){
      iaa_prep_sub_task_node(iaa[i], iaa_tsk_node[i]);
      iaa_tsk_node[i] = iaa_tsk_node[i]->next;

    }
  }



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

int reset_test_ctrs(){
  intermediate_host_ops_complete = 0;
  finalHostOpCtr = 0;
}