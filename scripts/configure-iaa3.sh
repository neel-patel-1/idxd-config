#!/bin/bash

accel-config disable-device iax3
accel-config disable-wq iax3/wq3.4
accel-config disable-wq iax3/wq3.1
accel-config load-config -c scripts/iaa3_config.conf
accel-config enable-device iax3
accel-config enable-wq iax3/wq3.4
accel-config enable-wq iax3/wq3.1