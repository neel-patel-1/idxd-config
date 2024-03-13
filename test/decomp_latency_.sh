#!/bin/bash
log=comp_log.log
size=$1
sudo ./iaa_test -s 10000 -w 0 -l $size -f 0x1 -n 1 -o0x42 > comp_log.log
ratio=$( grep Ratio $log | grep Uncompressed | awk -F, '{print $3}' | sed 's/\[.*//g'  | awk '{sum+=$2} END{print sum/NR}')
grep decompress comp_log.log  | tail -n 4 	| awk -F: '{str[NR]=$1; val[NR]=$2} END{for(i=1;i<=NR;i++){printf("%s,",str[i]);}  printf("Ratio,Size\n"); for(i=1;i<=NR;i++) {printf("%s,",val[i]);} }'
echo -n "${ratio},"
