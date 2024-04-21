#!/bin/bash
SIZES=(1024 2048 4096 8192 16384 32768 65536 131072 262144 524288 1048576)
DESCS=(256 128 64 32 16 8 4 2 1 1 1)
AXS=(4 4 4 4 4 4 4 4 4 2 1)
ctr=0
sudo ./../setup_dsa.sh shared-engine.cfg

for i in "${SIZES[@]}"; do
	wq_depth=$(( 128 / ${AXS[$ctr]} ))

  sudo .//spt_spinup -t11 -a${AXS[$ctr]} -p $i -n ${DESCS[$ctr]} -i100 -l $wq_depth | tee dsa_streaming_parallel.log.$i
  while [ "$( grep Failed dsa_streaming_parallel.log.$i )" ]; do
		sudo .//spt_spinup -t11 -a${AXS[$ctr]} -p $i -n ${DESCS[$ctr]} -i100 -l $wq_depth | tee dsa_streaming_parallel.log.$i
  done
  ctr=$((ctr+1))
done

for i in $( ls -1 dsa_streaming_parallel.log.* | sort -V); do echo $i; awk '/GB/{sum+=$8} END{print sum}' $i; done | grep -v dsa
