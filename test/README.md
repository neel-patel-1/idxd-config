Latency Breakdown
=====
https://vscode.dev/github/neel-patel-1/idxd-config/blob/decomp_calgary_latency/test/accel_test.c#L208
- alloc work desc

https://vscode.dev/github/neel-patel-1/idxd-config/blob/decomp_calgary_latency/test/iaa.c#L1483
- populate decomp work desc

https://vscode.dev/github/neel-patel-1/idxd-config/blob/decomp_calgary_latency/test/iaa.c#L1494
- submit work desc

https://vscode.dev/github/neel-patel-1/idxd-config/blob/decomp_calgary_latency/test/iaa.c#L1504
- wait for completion
-   calls umonitor (https://vscode.dev/github/neel-patel-1/idxd-config/blob/decomp_calgary_latency/test/accel_test.c#L322) on the address of the completion record
-   calls umwait (https://vscode.dev/github/neel-patel-1/idxd-config/blob/decomp_calgary_latency/test/accel_test.c#L327) on the address of the completion record

To Test:
- chaining hw vs. sw support: https://vscode.dev/github/neel-patel-1/idxd-config/blob/decomp_calgary_latency/test/iaa_test.c#L374
- support for decompress and scan: https://vscode.dev/github/neel-patel-1/idxd-config/blob/decomp_calgary_latency/test/iaa.c#L1618


Run
=====
#/home/n869p538/spr-accel-profiling/interrupt_qat_dc/idxd-config/test

#async 10000 desc job (p. 14 of IAA Spec)
 sudo ./iaa_test -w 0 -l 4096 -f 0x1 -n 10000 -o0x42 | tee log.3
grep -v -e'info' -e'Start' log.* | grep -e decomp | awk -F: '{printf("%s,%s\n",$2,$3);}'

#sync 10000 iterations

Source 2 purpose? https://vscode.dev/github/neel-patel-1/idxd-config/blob/decomp_calgary_latency/test/iaa.c#L293
- is it used for decomp test

How many times run?
1000

Num Descs parameter impact?


compress, get ratio, decompress get latency

block_on_fault?

#accel-config test



Old:
=====
The test command is an option to test all the library code of accel-config,
including set and get libaccfg functions for all components in dsa device, set
large wq to exceed max total size in dsa.

Build
=====
To enable test in the accel-config utility, building steps are following:

```
./autogen.sh
./configure CFLAGS='-g -O2' --prefix=/usr --sysconfdir=/etc --libdir=/usr/lib64
--enable-test=yes
make
sudo make install
```

Option
======
'accel-config test' [<options>]

Options can be specified to set the log level (default is LOG DEBUG).

-l::
--log-level=::
	set the log level, by default it is LOG_DEBUG.

Examples
========
The following shows an example of using "accel-config test".

```
# accel-config test
run test libaccfg
configure device 0
configure group0.0
configure wq0.0
configure engine0.0
configure engine0.1
configure group0.1
configure wq0.1
configure wq0.2
configure wq0.3
configure engine0.2
configure engine0.3
check device0
check group0.0
check group0.1
check wq0.0
check wq0.1
check wq0.2
check wq0.3
check engine0.0
check engine0.1
check engine0.2
check engine0.3
test 0: test the set and get libaccfg functions for components passed successfully
configure device 1
configure group1.3
configure wq1.2
configure wq1.3
configure wq1.4
test 1: set large wq to exceed max total size in dsa passed successfully
test-libaccfg: PASS
SUCCESS!
```
