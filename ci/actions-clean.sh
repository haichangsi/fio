#!/bin/bash
# This script expects to be invoked from the base fio directory.

make -j clean || true
rm -f $(find -name "*.o") || true

