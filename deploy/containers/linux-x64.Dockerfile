FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
# xauth and xvfb give by-hand WebKitGTK probes a display (xvfb-run), such as
# window_navigation_webkitgtk_probe. python3 runs the build's Python steps,
# such as the Native API contract check, as in the Windows image.
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential ca-certificates cmake curl dpkg-dev fakeroot file git ninja-build \
    pkg-config libcurl4-openssl-dev libgtk-3-dev libssl-dev libwebkit2gtk-4.1-dev \
    python3 xauth xvfb zlib1g-dev && rm -rf /var/lib/apt/lists/*

# Rust lives outside /root and is readable by everyone: the release build runs
# as the invoking user (deploy/scripts/singularity.py), who cannot enter
# /root, with CARGO_HOME pointed at a folder that user can write.
ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
    | sh -s -- -y --profile minimal --no-modify-path \
    && chmod -R a+rX /opt/rustup /opt/cargo
ENV PATH=/opt/cargo/bin:$PATH

# The provider key store (Provider/CredentialStore/CredentialStoreLinux.cpp)
# keeps keys in the desktop keyring through libsecret; its test,
# beta1_credential_store, runs a private D-Bus session bus with GNOME Keyring.
RUN apt-get update && apt-get install -y --no-install-recommends \
    dbus-daemon gnome-keyring libsecret-1-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /workspace
