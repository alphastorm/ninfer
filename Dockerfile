# syntax=docker/dockerfile:1

ARG NINFER_TARGET_PLATFORM=linux/amd64
FROM --platform=${NINFER_TARGET_PLATFORM} nvidia/cuda:12.8.1-devel-ubuntu24.04@sha256:4b9ed5fa8361736996499f64ecebf25d4ec37ff56e4d11323ccde10aa36e0c43 AS build

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

ARG NINFER_BUILD_PROFILE=omp-v0.2.0-rtx3090
ARG NINFER_UPSTREAM_BASE_SHA=ef6ecc3c139b43fc4d3e1b92df474305e8429544
ARG NINFER_PATCH_STACK_SHA
ARG NINFER_SOURCE_CLEAN_VERIFIED=OFF
RUN cmake -S . -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CUDA_ARCHITECTURES=86 \
        -DNINFER_BUILD_APPS=ON \
        -DBUILD_TESTING=OFF \
        -DNINFER_BUILD_BENCHMARKS=OFF \
        -DNINFER_BUILD_PROFILE="${NINFER_BUILD_PROFILE}" \
        -DNINFER_UPSTREAM_BASE_SHA="${NINFER_UPSTREAM_BASE_SHA}" \
        -DNINFER_PATCH_STACK_SHA="${NINFER_PATCH_STACK_SHA}" \
        -DNINFER_SOURCE_CLEAN_VERIFIED="${NINFER_SOURCE_CLEAN_VERIFIED}" \
    && cmake --build /build --parallel --target ninfer ninfer-serve

FROM --platform=${NINFER_TARGET_PLATFORM} nvidia/cuda:12.8.1-runtime-ubuntu24.04@sha256:828c4d878adcaa4265d80c95d8ec877149b49bb2419a4cf3bb6aa889bbb7ca2e

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

# GeForce cards cannot use the forward-compatibility libcuda shipped for datacenter GPUs. Remove
# it so the RTX 3090 uses the host driver through ordinary CUDA minor-version compatibility.
RUN rm -rf /usr/local/cuda-12.8/compat /usr/local/cuda-12/compat /usr/local/cuda/compat

COPY --from=build /build/apps/ninfer /usr/local/bin/ninfer
COPY --from=build /build/apps/ninfer-serve /usr/local/bin/ninfer-serve

WORKDIR /workspace
EXPOSE 8080
STOPSIGNAL SIGTERM

CMD ["ninfer-serve", "--help"]
