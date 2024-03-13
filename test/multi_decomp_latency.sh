#!/bin/bash
log=comp_log.log
make
SIZES=( 256  512 1024 4096 )
echo > parsed_decomp_log.log
for size in ${SIZES[@]}; do
	./decomp_latency_.sh $size
	echo  "$size"
done
