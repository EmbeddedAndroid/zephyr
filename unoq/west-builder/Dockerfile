# Host-side (aarch64) Zephyr build environment: docker-wrapped west.
# Carries only the build tooling + west + python deps. The Zephyr workspace
# and the Zephyr SDK are BIND-MOUNTED at runtime (they total ~12G; baking them
# would bloat the image and duplicate disk). See build.sh.
FROM debian:trixie-slim

# Zephyr build dependencies (mirrors the getting-started apt list, trimmed).
RUN apt-get update -qq \
 && apt-get install -y --no-install-recommends -qq \
        git cmake ninja-build gperf ccache dfu-util \
        device-tree-compiler wget xz-utils file make \
        gcc g++ \
        python3 python3-dev python3-pip python3-venv \
        python3-setuptools python3-wheel \
        libmagic1 util-linux adduser pkg-config \
 && rm -rf /var/lib/apt/lists/*

# west + the core Zephyr python deps (the ones `west build` actually needs).
# Installed into a venv to dodge PEP668 "externally managed" on trixie.
ENV VIRTUAL_ENV=/opt/zephyr-venv
RUN python3 -m venv "$VIRTUAL_ENV"
ENV PATH="$VIRTUAL_ENV/bin:$PATH"
RUN pip install --no-cache-dir \
        west \
        pyelftools PyYAML pykwalify canopen packaging progress \
        psutil pylink-square pyserial requests anytree intelhex \
        cryptography intelhex click cbor2 jsonschema \
        natsort pytest tabulate ply colorama junitparser pyserial

# The SDK and workspace mount here at runtime (see build.sh).
ENV ZEPHYR_SDK_INSTALL_DIR=/sdk
ENV ZEPHYR_TOOLCHAIN_VARIANT=zephyr
WORKDIR /work

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
