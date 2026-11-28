# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

FROM ubuntu:24.04

RUN apt-get update \
    && apt-get install -y --no-install-recommends autoconf automake build-essential ca-certificates \
      cmake git libgflags-dev liblz4-dev libsnappy-dev libtool libzstd-dev ninja-build python3 \
      python3-pip python3-redis qemu-user qemu-user-static wget xz-utils zlib1g-dev zstd \
    && rm -rf /var/lib/apt/lists/*

RUN --mount=type=bind,source=install-redis.sh,target=/tmp/install-redis.sh \
    sh /tmp/install-redis.sh

ENV RISCV_PATH=/opt/riscv
RUN set -eu; \
    release=2025.07.03; \
    archive="riscv64-glibc-ubuntu-24.04-gcc-nightly-${release}-nightly.tar.xz"; \
    wget -q "https://github.com/riscv-collab/riscv-gnu-toolchain/releases/download/${release}/${archive}" -O "/tmp/${archive}"; \
    echo "89003a188bb51e941588d5f913c87df08cb8c5170d779d9f48b95aeaa617afac  /tmp/${archive}" | sha256sum -c -; \
    mkdir -p "$RISCV_PATH"; \
    tar -xJf "/tmp/${archive}" -C "$RISCV_PATH" --strip-components=1; \
    rm "/tmp/${archive}"; \
    sed -i "s|/mnt/riscv/riscv64-unknown-linux-gnu/lib|$RISCV_PATH/riscv64-unknown-linux-gnu/lib|" \
      "$RISCV_PATH/riscv64-unknown-linux-gnu/lib/libatomic.la"

ENV PATH="/opt/riscv/bin:/usr/local/bin:${PATH}" \
    LD_LIBRARY_PATH="/opt/riscv/lib" \
    QEMU_LD_PREFIX="/opt/riscv/sysroot"
