# Build environment for the prebuilt FPP 10 adapter object. FROM debian:trixie
# because that is FalconChristmas/fpp's own Docker/Dockerfile base at the
# pinned FPP 10 commit (`FROM debian:trixie`, read directly from that file at
# the pinned commit): FPP 10's dlopen gate checks an API integer and four
# struct sizes and nothing about the C++ standard library, so matching the
# base image is what keeps libstdc++/STL ABI aligned with a real FPP 10 host.
# Built once per architecture via docker buildx --platform; arm64 and armv7
# run under QEMU emulation, amd64 runs native. The compile step below is the
# same `make -C native/adapters fpp10 FPP_SRC=...` the on-host install path
# runs, so this can never drift into a second, different build of the source.
FROM debian:trixie

ARG FPP_TAG
ARG FPP_COMMIT
ARG SOURCE_DATE_EPOCH
ARG PREBUILT_CXXFLAGS

RUN apt-get update -o Acquire::Retries=2 \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    g++ make pkg-config binutils git ca-certificates gawk \
    libjsoncpp-dev libcurl4-openssl-dev \
 && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --branch "$FPP_TAG" https://github.com/FalconChristmas/fpp.git /opt/fpp \
 && actual="$(git -C /opt/fpp rev-parse HEAD)" \
 && if [ "$actual" != "$FPP_COMMIT" ]; then \
      echo "FPP checkout resolved to $actual, expected $FPP_COMMIT" >&2; exit 1; \
    fi

COPY native /opt/showmesh-native

# SOURCE_DATE_EPOCH is honored by gcc/binutils for any embedded timestamp.
# PREBUILT_CXXFLAGS (defined once, in scripts/build-prebuilt-fpp10.sh) carries
# -ffile-prefix-map so this container's absolute build paths never land in
# the object, and -Wl,--build-id=none so the linker's build-id note is not a
# source of non-reproducibility across otherwise byte-identical builds.
# verify-fpp10 runs here so a bad architecture build fails the image build
# itself rather than shipping a `.so` nobody checked.
ENV SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH
RUN c++ --version > /opt/build-compiler.txt \
 && make -C /opt/showmesh-native/adapters fpp10 FPP_SRC=/opt/fpp/src CXXFLAGS="$PREBUILT_CXXFLAGS" \
 && make -C /opt/showmesh-native/adapters verify-fpp10 FPP_SRC=/opt/fpp/src
