

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
  for(int i=0; i<num_iaas; i++){
    iaa_tsk_node[i] = iaa[i]->multi_task_node;
  }

  clock_gettime(CLOCK_MONOTONIC, &times[0]);
  /* Submission / work distribution scheme -- round robin requests across all iaa instances*/
  while(iaa_tsk_node[num_iaas-1]){
    for(int i=0; i<num_iaas; i++){
      iaa_prep_sub_task_node(iaa[i], iaa_tsk_node[i]);
      iaa_tsk_node[i] = iaa_tsk_node[i]->next;
    }
  }


  /*Reset tsk nodes for polling phase */
  for(int i=0; i<num_iaas; i++){
    iaa_tsk_node[i] = iaa[i]->multi_task_node;
  }
  while(iaa_tsk_node[num_iaas-1]){
    for(int i=0; i<num_iaas; i++){
      iaa_wait_compress(iaa[i], iaa_tsk_node[i]->tsk);
      iaa_tsk_node[i] = iaa_tsk_node[i]->next;
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &times[1]);

}

int reset_test_ctrs(){
  intermediate_host_ops_complete = 0;
  finalHostOpCtr = 0;
}