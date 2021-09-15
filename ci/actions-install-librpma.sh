#!/bin/bash -e

# Sep 15 2021 Merge pull request #1285 from ldorau/rpma-add-rpma_mr_advise-to-src-librpma.map-file
LIBRPMA_VERSION=68bb428463f7121789a01007e95cea224496e29f
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
