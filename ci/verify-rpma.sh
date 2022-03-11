#!/bin/bash
# This script expects to be invoked from the base fio directory.
set -eux

N_RPMA_SRC=$(find -name "*rpma*.c" | wc -l)
N_RPMA_OBJ=$(find -name "*rpma*.o" | wc -l)

if [ $N_RPMA_OBJ -eq 0 -o $N_RPMA_OBJ -ne $N_RPMA_SRC ]; then
	./configure | grep -e rpma -e pmem -e verbs -e rdma -e protobuf
	set +x
	echo "RPMA engine has NOT been built!"
	exit 1
fi
