#!/bin/bash
# This script expects to be invoked from the base fio directory.
set -e

N_RPMA_SRC=$(find -name "*rpma*.c" | wc -l)
N_RPMA_OBJ=$(find -name "*rpma*.o" | wc -l)

echo N_RPMA_SRC=$N_RPMA_SRC
echo N_RPMA_OBJ=$N_RPMA_OBJ

if [ $N_RPMA_OBJ -eq 0 -o $N_RPMA_OBJ -ne $N_RPMA_SRC ]; then
	./configure | grep -e rpma -e pmem -e verbs -e rdma -e protobuf
	echo "RPMA engine has NOT been built!"
	exit 1
fi

# verify if required libraries (libpmem or/and libpmem2) are installed
LIB=$1
[ "$LIB" != "libpmem" -a "$LIB" != "libpmem2" ] && exit 0

./configure | grep -v -e engine | grep -e libpmem | \
	while IFS='' read -r line || [[ -n "$line" ]]; do
		COL1=$(echo $line | awk '{print $1}') # column #1
		COL2=$(echo $line | awk '{print $2}') # column #2
		case $LIB in
		"libpmem")
			if [ "$COL1" == "libpmem" -a "$COL2" != "yes" ]; then
				echo "Error: libpmem is NOT installed!"
				exit 1
			fi
			if [ "$COL1" == "libpmem2" -a "$COL2" == "yes" ]; then
				echo "Error: libpmem2 should NOT be installed!"
				exit 1
			fi
			;;
		"libpmem2")
			if [ "$COL1" == "libpmem" -a "$COL2" != "yes" ]; then
				echo "Error: libpmem is NOT installed!"
				exit 1
			fi
			if [ "$COL1" == "libpmem2" -a "$COL2" != "yes" ]; then
				echo "Error: libpmem2 is NOT installed!"
				exit 1
			fi
			;;
		esac
	done
