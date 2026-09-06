{
  description = "dc systems programming language compiler";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { nixpkgs, ... }:
    let
      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
    in {
      packages = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          lib = pkgs.lib;

          llvm = pkgs.llvmPackages;
          llvmDev = lib.getDev llvm.llvm;

          libcxx = llvm.libcxx;
          libcxxDev = lib.getDev libcxx;

          libcxxInclude = "${libcxxDev}/include/c++/v1";
          stdModule = "${libcxx}/share/libc++/v1/std.cppm";
          stdCompatModule = "${libcxx}/share/libc++/v1/std.compat.cppm";

          makeArgs = [
            "CXX=${llvm.clang}/bin/clang++"
            "CC=${llvm.clang}/bin/clang"
            "LLVM_CONFIG=${llvmDev}/bin/llvm-config"
            "SCAN_DEPS=${pkgs.clang-tools}/bin/clang-scan-deps"
            "STD_MODULE_SRC=${stdModule}"
            "STD_COMPAT_SRC=${stdCompatModule}"
          ];
        in {
          default = llvm.libcxxStdenv.mkDerivation {
            pname = "dcc";
            version = "unstable";

            src = ./.;

            hardeningDisable = [ "fortify" ];

            nativeBuildInputs = [
              pkgs.gnumake
              pkgs.python3
              llvm.clang
              pkgs.clang-tools
              llvm.llvm
            ];

            buildInputs = [
              libcxx
            ];

            patchPhase = ''
              runHook prePatch

              substituteInPlace mk/config.mk \
                --replace-fail \
                  'STDLIB_FLAGS := -stdlib=libc++' \
                  'STDLIB_FLAGS := -stdlib=libc++ -nostdinc++ -I${libcxxInclude} -Wno-unused-command-line-argument'

              runHook postPatch
            '';

            buildPhase = ''
              runHook preBuild

              test -f "${stdModule}"
              test -f "${stdCompatModule}"

              make -j"$NIX_BUILD_CORES" \
                BUILD_TYPE=release \
                PREFIX="$out" \
                ${lib.escapeShellArgs makeArgs}

              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall

              make \
                BUILD_TYPE=release \
                PREFIX="$out" \
                ${lib.escapeShellArgs makeArgs} \
                install

              runHook postInstall
            '';

            meta = {
              description = "dc systems programming language compiler";
              homepage = "https://github.com/Personne-admin/dcc";
              license = lib.licenses.gpl3Plus;
              platforms = lib.platforms.linux;
              mainProgram = "dcc";
            };
          };
        });

      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          lib = pkgs.lib;

          llvm = pkgs.llvmPackages;
          llvmDev = lib.getDev llvm.llvm;

          libcxx = llvm.libcxx;
          libcxxDev = lib.getDev libcxx;
        in {
          default = pkgs.mkShell.override {
            stdenv = llvm.libcxxStdenv;
          } {
            packages = [
              pkgs.gnumake
              pkgs.python3
              llvm.clang
              pkgs.clang-tools
              llvm.llvm
              libcxx
            ];

            hardeningDisable = [ "fortify" ];

            CXX = "${llvm.clang}/bin/clang++";
            CC = "${llvm.clang}/bin/clang";
            LLVM_CONFIG = "${llvmDev}/bin/llvm-config";
            SCAN_DEPS = "${pkgs.clang-tools}/bin/clang-scan-deps";

            STD_MODULE_SRC = "${libcxx}/share/libc++/v1/std.cppm";
            STD_COMPAT_SRC = "${libcxx}/share/libc++/v1/std.compat.cppm";

            STD_INCLUDE_DIR = "${libcxxDev}/include/c++/v1";
          };
        });
    };
}
