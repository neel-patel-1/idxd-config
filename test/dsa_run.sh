#!/bin/bash
SIZES=(1024 2048 4096 8192 16384 32768 65536 131072 262144 524288 1048576)
DESCS=(1024 512 256 128 64 32 16 8 4 2 1)
AXS=(4 4 4 4 4 4 4 4 4 1 1)
ctr=0
for i in "${SIZES[@]}"; do
  echo "sudo .//spt_spinup -t7 -a${AXS[$ctr]} -n4 -p $i -n ${DESCS[$ctr]} -i100 | tee -a dsa.log"
  sudo .//spt_spinup -t7 -a${AXS[$ctr]} -n4 -p $i -n ${DESCS[$ctr]} -i100 | tee dsa.log.$i
  while [ "$( grep Failed dsa.log.$i )" ]; do
   sudo .//spt_spinup -t7 -a${AXS[$ctr]} -n4 -p $i -n ${DESCS[$ctr]} -i100 | tee dsa.log.$i
  done
  ctr=$((ctr+1))
done

for i in $( ls -1 dsa.log.* | sort -V); do echo $i; awk '/GB/{sum+=$8} END{print sum}' $i; done