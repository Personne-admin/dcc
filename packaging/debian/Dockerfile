FROM debian:forky

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        devscripts \
        debhelper \
        dpkg-dev \
        fakeroot \
        ca-certificates \
        clang-21 \
        clang-tools-21 \
        llvm-21-dev \
        llvm-21 \
        llvm \
        lld-21 \
        lld \
        libc++-21-dev \
        libc++abi-21-dev \
        nasm \
        python3 \
        make && \
    rm -rf /var/lib/apt/lists/*

RUN for bin in /usr/bin/*-21; do \
        target="/usr/bin/$(basename "$bin" -21)"; \
        ln -sf "$bin" "$target"; \
    done

WORKDIR /src

COPY . .

RUN set -eux; \
    clang++-21 --version; \
    llvm-config-21 --version; \
    clang-scan-deps-21 --version; \
    llvm-as --version; \
    ld.lld --version; \
    find /usr -name std.cppm -print; \
    find /usr -name std.compat.cppm -print

RUN dpkg-buildpackage \
    --build=binary \
    --no-sign

RUN mkdir -p /artifacts && \
    cp /dcc_*.deb /artifacts/
