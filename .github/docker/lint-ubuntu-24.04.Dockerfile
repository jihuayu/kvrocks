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
      clang-format-18 clang-tidy-18 cmake curl git gnupg libtool lsb-release ninja-build python3 \
      python3-pip wget zstd \
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
