# syntax=docker/dockerfile:1

FROM nvidia/cuda:13.1.2-devel-ubuntu24.04 AS build-env

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        ccache \
        cmake \
        git \
        libavcodec-dev \
        libavformat-dev \
        libavutil-dev \
        libcurl4-openssl-dev \
        libhwloc-dev \
        libjpeg-dev \
        libswscale-dev \
        liburing-dev \
        ninja-build \
        pkg-config \
        python3 \
        python3-jinja2 \
        python3-jsonschema \
    && rm -rf /var/lib/apt/lists/*

# Keep the build-only stub out of every default/runtime loader search path.
RUN mkdir -p /opt/ninfer-driver-stub \
    && ln -s /usr/local/cuda/lib64/stubs/libcuda.so /opt/ninfer-driver-stub/libcuda.so.1

FROM build-env AS build

ENV LD_LIBRARY_PATH=/opt/ninfer-driver-stub:${LD_LIBRARY_PATH}

ARG NINFER_BUILD_PROFILE=rtx5090
ARG NINFER_UPSTREAM_BASE_SHA
ARG NINFER_PATCH_STACK_SHA

WORKDIR /src
COPY . .

RUN cmake -S . -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DNINFER_BUILD_APPS=ON \
        -DBUILD_TESTING=OFF \
        -DNINFER_BUILD_BENCHMARKS=OFF \
        -DNINFER_BUILD_PROFILE="${NINFER_BUILD_PROFILE}" \
        -DNINFER_UPSTREAM_BASE_SHA="${NINFER_UPSTREAM_BASE_SHA}" \
        -DNINFER_PATCH_STACK_SHA="${NINFER_PATCH_STACK_SHA}" \
        -DNINFER_SOURCE_DIRTY=auto \
    && cmake --build /build --parallel --target ninfer ninfer-serve

FROM nvidia/cuda:13.1.2-runtime-ubuntu24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates \
        libavcodec60 \
        libavformat60 \
        libavutil58 \
        libcurl4t64 \
        libhwloc15 \
        libjpeg-turbo8 \
        libswscale7 \
        liburing2 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /build/apps/ninfer /usr/local/bin/ninfer
COPY --from=build /build/apps/ninfer-serve /usr/local/bin/ninfer-serve

WORKDIR /workspace
EXPOSE 8080
STOPSIGNAL SIGTERM

CMD ["ninfer-serve", "--help"]
