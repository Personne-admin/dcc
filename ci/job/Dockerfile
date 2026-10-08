# syntax=docker/dockerfile:1.7

ARG TOOLCHAIN_IMAGE
FROM ${TOOLCHAIN_IMAGE}

ARG NODE_VERSION=22.23.3
ARG NODE_SHA256=df450af89261115ef9f9e3830c3eeb2cc9213b63c720b1af623cb5dcbe2e02de
ARG WINE_VERSION=11.18~noble-1
ARG WINEHQ_KEY_FINGERPRINT=D43F640145369C51D786DDEA76F1A20FF987672F
ARG NASM_VERSION=3.02
ARG NASM_SHA256=87336eba53b4acfe917424ab5d500d2b0054d9f5148d35c2273ccf2cfb712f0d
ARG ACTIONLINT_VERSION=1.7.12
ARG ACTIONLINT_SHA256=8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8

ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8 LC_ALL=C.UTF-8

RUN set -eux; \
    dpkg --add-architecture i386; \
    curl -fsSL https://dl.winehq.org/wine-builds/winehq.key -o /tmp/winehq.key; \
    gpg --show-keys --with-colons /tmp/winehq.key | awk -F: '$1 == "fpr" { print $10 }' | grep -qx "${WINEHQ_KEY_FINGERPRINT}"; \
    gpg --dearmor -o /usr/share/keyrings/winehq.gpg /tmp/winehq.key; \
    rm /tmp/winehq.key; \
    echo "deb [arch=amd64,i386 signed-by=/usr/share/keyrings/winehq.gpg] https://dl.winehq.org/wine-builds/ubuntu/ noble main" > /etc/apt/sources.list.d/winehq.list; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
        git binutils libcmark-dev msitools wixl dpkg-dev fakeroot zstd unzip qemu-system-x86 \
        "winehq-devel=${WINE_VERSION}" "wine-devel=${WINE_VERSION}" "wine-devel-amd64=${WINE_VERSION}" "wine-devel-i386=${WINE_VERSION}"; \
    rm -rf /var/lib/apt/lists/*; \
    curl -fsSL -o /tmp/node.tar.xz "https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-x64.tar.xz"; \
    echo "${NODE_SHA256}  /tmp/node.tar.xz" | sha256sum -c -; \
    tar -xf /tmp/node.tar.xz -C /usr/local --strip-components=1 --exclude='*/CHANGELOG.md' --exclude='*/README.md' --exclude='*/LICENSE'; \
    rm /tmp/node.tar.xz; \
    curl -fsSL -o /tmp/nasm.tar.xz "https://www.nasm.us/pub/nasm/releasebuilds/${NASM_VERSION}/nasm-${NASM_VERSION}.tar.xz"; \
    echo "${NASM_SHA256}  /tmp/nasm.tar.xz" | sha256sum -c -; \
    mkdir /tmp/nasm; tar -xf /tmp/nasm.tar.xz -C /tmp/nasm --strip-components=1; \
    (cd /tmp/nasm && ./configure CC=clang --prefix=/usr/local >/dev/null && make -j"$(nproc)" nasm ndisasm >/dev/null && install -m 755 nasm ndisasm /usr/local/bin/); \
    rm -rf /tmp/nasm /tmp/nasm.tar.xz; \
    curl -fsSL -o /tmp/actionlint.tar.gz "https://github.com/rhysd/actionlint/releases/download/v${ACTIONLINT_VERSION}/actionlint_${ACTIONLINT_VERSION}_linux_amd64.tar.gz"; \
    echo "${ACTIONLINT_SHA256}  /tmp/actionlint.tar.gz" | sha256sum -c -; \
    tar -xzf /tmp/actionlint.tar.gz -C /usr/local/bin actionlint; \
    rm /tmp/actionlint.tar.gz; \
    node --version; npm --version; wine --version; wixl --version; nasm -v; qemu-system-i386 --version; actionlint --version
