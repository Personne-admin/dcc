# syntax=docker/dockerfile:1.7
ARG UBUNTU_IMAGE

FROM ${UBUNTU_IMAGE} AS base
ARG APT_LLVM_MAJOR
ARG APT_LLVM_KEY_FINGERPRINT
ENV DEBIAN_FRONTEND=noninteractive
RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends ca-certificates curl gnupg xz-utils make cmake ninja-build python3 file; \
    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /tmp/llvm.key; \
    gpg --show-keys --with-colons /tmp/llvm.key | awk -F: '$1 == "fpr" { print $10 }' | grep -qx "${APT_LLVM_KEY_FINGERPRINT}"; \
    gpg --dearmor -o /usr/share/keyrings/apt.llvm.org.gpg /tmp/llvm.key; \
    rm /tmp/llvm.key; \
    echo "deb [signed-by=/usr/share/keyrings/apt.llvm.org.gpg] https://apt.llvm.org/noble/ llvm-toolchain-noble-${APT_LLVM_MAJOR} main" > /etc/apt/sources.list.d/apt.llvm.org.list; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
        clang-${APT_LLVM_MAJOR} lld-${APT_LLVM_MAJOR} clang-tools-${APT_LLVM_MAJOR} llvm-${APT_LLVM_MAJOR} \
        libc++-${APT_LLVM_MAJOR}-dev libc++abi-${APT_LLVM_MAJOR}-dev libclang-rt-${APT_LLVM_MAJOR}-dev; \
    rm -rf /var/lib/apt/lists/*; \
    for tool in clang clang++ clang-scan-deps ld.lld lld-link llvm-ar llvm-as llvm-nm llvm-objdump llvm-readobj llvm-ranlib; do \
        ln -sf /usr/bin/${tool}-${APT_LLVM_MAJOR} /usr/local/bin/${tool}; \
    done; \
    clang++ --version; clang-scan-deps --version; ld.lld --version

FROM base AS llvm-source
ARG LLVM_VERSION
ARG LLVM_SHA256
RUN set -eux; \
    mkdir -p /src; cd /src; \
    curl -fsSL -o llvm.tar.xz "https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/llvm-project-${LLVM_VERSION}.src.tar.xz"; \
    echo "${LLVM_SHA256}  llvm.tar.xz" | sha256sum -c -; \
    tar -xf llvm.tar.xz --strip-components=1 \
        "llvm-project-${LLVM_VERSION}.src/llvm" "llvm-project-${LLVM_VERSION}.src/cmake" "llvm-project-${LLVM_VERSION}.src/third-party"; \
    rm llvm.tar.xz

FROM llvm-source AS llvm-linux
RUN set -eux; \
    cmake -G Ninja -S /src/llvm -B /work \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/llvm-static \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DLLVM_ENABLE_LIBCXX=ON \
        -DLLVM_TARGETS_TO_BUILD=X86 \
        -DLLVM_BUILD_LLVM_DYLIB=OFF -DLLVM_LINK_LLVM_DYLIB=OFF -DBUILD_SHARED_LIBS=OFF \
        -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_RTTI=ON \
        -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_DOCS=OFF \
        -DLLVM_BUILD_TOOLS=OFF -DLLVM_INCLUDE_UTILS=ON -DLLVM_ENABLE_BINDINGS=OFF \
        -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_LIBEDIT=OFF \
        -DLLVM_ENABLE_FFI=OFF -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_CURL=OFF -DLLVM_ENABLE_HTTPLIB=OFF; \
    ninja -C /work install llvm-config; \
    test -x /opt/llvm-static/bin/llvm-config || install -m 755 /work/bin/llvm-config /opt/llvm-static/bin/llvm-config; \
    mkdir -p /opt/llvm-native-tools; \
    cp /work/bin/*tblgen /opt/llvm-native-tools/; \
    rm -rf /work; \
    /opt/llvm-static/bin/llvm-config --version --shared-mode --system-libs

FROM llvm-source AS llvm-mingw
ARG LLVM_MINGW_RELEASE
ARG LLVM_MINGW_SHA256
COPY --from=llvm-linux /opt/llvm-native-tools /opt/llvm-native-tools
RUN set -eux; \
    name="llvm-mingw-${LLVM_MINGW_RELEASE}-ucrt-ubuntu-22.04-x86_64"; \
    curl -fsSL -o /tmp/llvm-mingw.tar.xz "https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_RELEASE}/${name}.tar.xz"; \
    echo "${LLVM_MINGW_SHA256}  /tmp/llvm-mingw.tar.xz" | sha256sum -c -; \
    mkdir -p /opt/llvm-mingw; \
    tar -xf /tmp/llvm-mingw.tar.xz -C /opt/llvm-mingw --strip-components=1; \
    rm /tmp/llvm-mingw.tar.xz; \
    rm -rf /opt/llvm-mingw/aarch64-w64-mingw32 /opt/llvm-mingw/armv7-w64-mingw32 /opt/llvm-mingw/arm64ec-w64-mingw32 /opt/llvm-mingw/i686-w64-mingw32; \
    /opt/llvm-mingw/bin/x86_64-w64-mingw32-clang++ --version; \
    test -f /opt/llvm-mingw/share/libc++/v1/std.cppm
RUN set -eux; \
    m=/opt/llvm-mingw/bin/x86_64-w64-mingw32; \
    cmake -G Ninja -S /src/llvm -B /work \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/llvm-mingw-target \
        -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=AMD64 \
        -DCMAKE_C_COMPILER=$m-clang -DCMAKE_CXX_COMPILER=$m-clang++ \
        -DCMAKE_RC_COMPILER=$m-windres -DCMAKE_AR=/opt/llvm-mingw/bin/llvm-ar -DCMAKE_RANLIB=/opt/llvm-mingw/bin/llvm-ranlib \
        -DLLVM_HOST_TRIPLE=x86_64-w64-mingw32 \
        -DLLVM_NATIVE_TOOL_DIR=/opt/llvm-native-tools \
        -DLLVM_TARGETS_TO_BUILD=X86 \
        -DLLVM_BUILD_LLVM_DYLIB=OFF -DLLVM_LINK_LLVM_DYLIB=OFF -DBUILD_SHARED_LIBS=OFF \
        -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_RTTI=ON \
        -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_DOCS=OFF \
        -DLLVM_BUILD_TOOLS=OFF -DLLVM_INCLUDE_UTILS=OFF -DLLVM_ENABLE_BINDINGS=OFF \
        -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_LIBEDIT=OFF \
        -DLLVM_ENABLE_FFI=OFF -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_CURL=OFF -DLLVM_ENABLE_HTTPLIB=OFF; \
    ninja -C /work install-llvm-libraries install-llvm-headers; \
    rm -rf /work; \
    ls /opt/llvm-mingw-target/lib/libLLVMCore.a /opt/llvm-mingw-target/lib/libLLVMX86CodeGen.a

FROM base AS toolchain
ARG LLVM_VERSION
ARG LLVM_MINGW_RELEASE
ARG APT_LLVM_MAJOR
COPY --from=llvm-linux /opt/llvm-static /opt/llvm-static
COPY --from=llvm-mingw /opt/llvm-mingw /opt/llvm-mingw
COPY --from=llvm-mingw /opt/llvm-mingw-target /opt/llvm-mingw-target
ENV LLVM_CONFIG=/opt/llvm-static/bin/llvm-config \
    MINGW_SYSROOT=/opt/llvm-mingw \
    LLVM_MINGW_TARGET_PREFIX=/opt/llvm-mingw-target
RUN set -eux; \
    { echo "llvm-static ${LLVM_VERSION}"; echo "llvm-mingw ${LLVM_MINGW_RELEASE}"; \
      echo "llvm-mingw-target ${LLVM_VERSION}"; \
      dpkg-query -W -f '${Package} ${Version}\n' "clang-${APT_LLVM_MAJOR}" "libc++-${APT_LLVM_MAJOR}-dev" "lld-${APT_LLVM_MAJOR}"; } > /opt/toolchain-manifest.txt; \
    cat /opt/toolchain-manifest.txt
