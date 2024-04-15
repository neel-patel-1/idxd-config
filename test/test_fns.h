

int multi_iaa_test(int tflags, int wq_type, int dev_id, int wq_id, size_t buf_size)
{
  int rc;
  int num_iaas = 4;
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
}