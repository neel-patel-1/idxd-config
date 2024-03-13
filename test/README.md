Run
=====
#from:/home/n869p538/spr-accel-profiling/interrupt_qat_dc/idxd-config/test
make

#decomp latency
./multi_decomp_latency.sh

#async 10000 desc job (p. 14 of IAA Spec)
``
sudo ./iaa_test -w 0 -l 4096 -f 0x1 -n 10000 -o0x42 | tee log.3
grep -v -e'info' -e'Start' log.* | grep -e decomp | awk -F: '{printf("%s,%s\n",$2,$3);}'
``

#sync 10000 iterations
sudo ./iaa_test -s 10000 -w 0 -l 4096 -f 0x1 -n 1 -o0x42 | tee sync_10000.log

#Whole calgary corpus gets comp'd
(base) n869p538@sapphire:test$ grep Calgary sync_10000.log | wc -l
13
(base) n869p538@sapphire:test$ echo $(( 4096 * 10000 / 13 ))
3150769

Interpret Results
=====
#async
[ info] test with op 66 passed
Average decompress alloc time: 167
Average decompress prep time: 575
Average decompress sub time: 156 <-- how long to fill full wq cap (16 for our test)
#[ info] TestCompress Iterations:1,16[ info] preparing descriptor for compress
Average decompress wait time: 1400
Average filter alloc time: 0
Average filter prep time: 0
Average filter sub time: 0
Average filter wait time: 0

#sync
[ info] test with op 66 passed
Average decompress alloc time: 250
Average decompress prep time: 186
Average decompress sub time: 31 <-- how long to fill one wq ent
Average decompress wait time: 2068
Average filter alloc time: 0
Average filter prep time: 0
Average filter sub time: 0
Average filter wait time: 0

Todo
===
When do we accelerate sync-decomp ?: 256, 1K is faster than ISA-L decomp, but comp ratio is lower.
Is iaa-inflate on isal-compressed obj's faster than isal-inflate on same obj's?
Latency for decomp on different ratios:1.0,2.0,3.0

Does IAA support variable compression levels?


Latency Overhead of indexed compression?
Compression ratio losses for indexed compression?
Indexed Decompression Speed improvement for offset 25%,50%,75%?

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



- why/how does comp compress_test get called 625 times?
https://vscode.dev/github/neel-patel-1/idxd-config/blob/num_iter_sync/test/iaa_test.c#L255
<- https://vscode.dev/github/neel-patel-1/idxd-config/blob/num_iter_sync/test/iaa_test.c#L308

(base) n869p538@sapphire:test$ sudo ./iaa_test -w 0 -l 4096 -f 0x1 -n 10000 -o0x42 > log.async
(base) n869p538@sapphire:test$ grep Prep log.async  | wc -l
625
(base) n869p538@sapphire:test$ sudo ./iaa_test -w 0 -l 4096 -f 0x1 -n 100 -o0x42 > log.async.1
(base) n869p538@sapphire:test$ grep Prep log.async  | wc -l
625
- regardless of num_descs?

depends on wq capacity or "threshold" when a shared wq is used:
https://vscode.dev/github/neel-patel-1/idxd-config/blob/num_iter_sync/test/iaa_test.c#L270
- we are using dedicated so we don't flood wqs by setting to 625




Source 2 purpose? https://vscode.dev/github/neel-patel-1/idxd-config/blob/decomp_calgary_latency/test/iaa.c#L293
- is it used for decomp test

```
Decompression can be performed on a single buffer, where the entire stream is contained in a single buffer, or on multiple
buffers, where the stream spans more than one buffer. In the latter case, a separate descriptor is submitted for each buffer.
This is called a job. That is, a job is a series of descriptors that operate on one logical stream. The descriptors in a job are
tied together by the use of a common AECS. The AECS written by each descriptor in the job is read by the next descriptor.
```

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
