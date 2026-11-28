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
      cmake curl git gnupg libtool lsb-release ninja-build python3 python3-pip wget zstd \
    && wget -q -O /etc/apt/keyrings/llvm.asc https://apt.llvm.org/llvm-snapshot.gpg.key \
    && echo 'deb [signed-by=/etc/apt/keyrings/llvm.asc] https://apt.llvm.org/noble/ llvm-toolchain-noble-18 main' \
      > /etc/apt/sources.list.d/llvm.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends clang-format-18 clang-tidy-18 \
    && rm -rf /var/lib/apt/lists/*

ARG GO_VERSION
ARG TARGETARCH
RUN set -eu; \
    archive="go${GO_VERSION}.linux-${TARGETARCH}.tar.gz"; \
    wget -q "https://dl.google.com/go/${archive}" -O "/tmp/${archive}"; \
    wget -q "https://dl.google.com/go/${archive}.sha256" -O "/tmp/${archive}.sha256"; \
    echo "$(cat "/tmp/${archive}.sha256")  /tmp/${archive}" | sha256sum -c -; \
    tar -xzf "/tmp/${archive}" -C /usr/local; \
    rm -f "/tmp/${archive}" "/tmp/${archive}.sha256"

ENV PATH="/usr/local/go/bin:${PATH}"

ARG GOLANGCI_LINT_VERSION
RUN set -eu; \
    cd /tmp; \
    release="golangci-lint-${GOLANGCI_LINT_VERSION}"; \
    bundle="${release}-linux-${TARGETARCH}"; \
    url="https://github.com/golangci/golangci-lint/releases/download/v${GOLANGCI_LINT_VERSION}"; \
    wget -q "${url}/${bundle}.tar.gz" "${url}/${release}-checksums.txt"; \
    sha256sum --check --ignore-missing "${release}-checksums.txt"; \
    tar -xzf "${bundle}.tar.gz" -C /usr/local/bin --no-same-owner --strip-components=1 "${bundle}/golangci-lint"; \
    rm "${bundle}.tar.gz" "${release}-checksums.txt"
