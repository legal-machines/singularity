FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
# python3 runs the build's Python steps, such as the Native API contract check.
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential ca-certificates cmake curl git libz-mingw-w64-dev mingw-w64 ninja-build nsis \
    pkg-config python3 unzip && rm -rf /var/lib/apt/lists/*

RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
    | sh -s -- -y --profile minimal \
    && /root/.cargo/bin/rustup target add x86_64-pc-windows-gnu
ENV PATH=/root/.cargo/bin:$PATH
WORKDIR /workspace
