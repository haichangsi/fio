#!/bin/bash -e

# Feb 02 2022 Merge pull request #1525 from haichangsi/deprecated_cq
# rpma: mark as deprecated rpma_cq_get_completion(), struct rpma_completion and enum rpma_op
LIBRPMA_VERSION=78c5dde32d6ea38b18485d72f87ff758a320a0a6
ZIP_FILE=rpma.zip

WORKDIR=$(pwd)

# install librpma
wget -O $ZIP_FILE https://github.com/pmem/rpma/archive/${LIBRPMA_VERSION}.zip
unzip $ZIP_FILE
mkdir -p rpma-${LIBRPMA_VERSION}/build
cd rpma-${LIBRPMA_VERSION}/build
cmake .. -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DBUILD_DOC=OFF \
	-DBUILD_EXAMPLES=OFF \
	-DTEST_PYTHON_TOOLS=OFF \
	-DBUILD_TESTS=OFF
make -j$(nproc)
sudo make -j$(nproc) install
cd $WORKDIR
rm -rf $ZIP_FILE rpma-${LIBRPMA_VERSION}
