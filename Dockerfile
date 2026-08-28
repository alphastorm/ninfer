# syntax=docker/dockerfile:1

FROM nvidia/cuda:13.1.2-devel-ubuntu24.04 AS build

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        cmake \
        libavcodec-dev \
        libavformat-dev \
        libavutil-dev \
        libcurl4-openssl-dev \
        libswscale-dev \
        ninja-build \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

ARG NINFER_BUILD_PROFILE=docker-release
ARG NINFER_UPSTREAM_BASE_SHA=unknown
ARG NINFER_PATCH_STACK_SHA=unknown
ARG NINFER_SOURCE_CLEAN_VERIFIED=OFF
RUN cmake -S . -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CUDA_ARCHITECTURES=120a \
        -DNINFER_BUILD_APPS=ON \
        -DBUILD_TESTING=OFF \
        -DNINFER_BUILD_BENCHMARKS=OFF \
        -DNINFER_BUILD_PROFILE="${NINFER_BUILD_PROFILE}" \
        -DNINFER_UPSTREAM_BASE_SHA="${NINFER_UPSTREAM_BASE_SHA}" \
        -DNINFER_PATCH_STACK_SHA="${NINFER_PATCH_STACK_SHA}" \
        -DNINFER_SOURCE_CLEAN_VERIFIED="${NINFER_SOURCE_CLEAN_VERIFIED}" \
    && cmake --build /build -j --target ninfer ninfer-serve

FROM nvidia/cuda:13.1.2-runtime-ubuntu24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates \
        libavcodec60 \
        libavformat60 \
        libavutil58 \
        libcurl4t64 \
        libswscale7 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /build/apps/ninfer /usr/local/bin/ninfer
COPY --from=build /build/apps/ninfer-serve /usr/local/bin/ninfer-serve

WORKDIR /workspace
EXPOSE 8080
STOPSIGNAL SIGTERM

CMD ["ninfer-serve", "--help"]
