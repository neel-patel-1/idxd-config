#!/bin/bash

accel-config disable-device iax1
accel-config disable-wq iax1/wq1.4
accel-config disable-wq iax1/wq1.1
accel-config load-config -c scripts/iaa_config.conf
accel-config enable-device iax1
accel-config enable-wq iax1/wq1.4
accel-config enable-wq iax1/wq1.1