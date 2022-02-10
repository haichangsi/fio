#!/bin/bash -e

# Feb 08 2022 Merge pull request #1560 from ldorau/common-0.11.0-release
# common: 0.11.0 release
LIBRPMA_VERSION=0.11.0

WORKDIR=$(pwd)
ZIP_FILE=rpma.zip

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
