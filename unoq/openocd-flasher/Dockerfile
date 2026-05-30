# Slim aarch64 openocd image for flashing the UNO Q STM32U585 over SWD
# (QCM2290 linuxgpiod adapter -> /dev/gpiochip1). Multi-stage so the runtime
# image carries only the openocd binary + libgpiod + tcl scripts, not the
# whole build toolchain. Stays well under the UNO Q ~300MB image panic limit.

FROM debian:trixie-slim AS build
RUN apt-get update -qq \
 && apt-get install -y --no-install-recommends -qq \
        build-essential autoconf automake libtool pkg-config \
        libgpiod-dev libgpiod3 \
        libjim-dev libjaylink-dev \
        git ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
RUN git clone --depth 1 https://github.com/openocd-org/openocd.git
WORKDIR /src/openocd
RUN ./bootstrap nosubmodule
RUN ./configure --enable-linuxgpiod --disable-werror \
        --disable-doxygen-html --disable-doxygen-pdf \
        --disable-internal-jimtcl \
        --disable-internal-libjaylink \
        --prefix=/opt/openocd
RUN make -j"$(nproc)"
RUN make install
RUN /opt/openocd/bin/openocd -c "adapter list" 2>&1 | head -3

# ---- runtime ----
FROM debian:trixie-slim
RUN apt-get update -qq \
 && apt-get install -y --no-install-recommends -qq \
        libgpiod3 libjim0.83 libjaylink0 gpiod \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/openocd /opt/openocd
ENV PATH=/opt/openocd/bin:$PATH
COPY unoq-swd.cfg /cfg/unoq-swd.cfg
COPY flash.sh /usr/local/bin/flash.sh
RUN chmod +x /usr/local/bin/flash.sh
ENTRYPOINT ["/usr/local/bin/flash.sh"]
