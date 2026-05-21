# Minimal PlatformIO build container for the claude-desktop-buddy port.
# Contains only the toolchain — the source tree is mounted from the host.
FROM python:3.12-slim

ENV PIP_NO_CACHE_DIR=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1 \
    PLATFORMIO_CORE_DIR=/pio/.platformio \
    PLATFORMIO_GLOBALLIB_DIR=/pio/.platformio/lib \
    PLATFORMIO_PACKAGES_DIR=/pio/.platformio/packages \
    PLATFORMIO_PLATFORMS_DIR=/pio/.platformio/platforms \
    DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        git \
        ca-certificates \
        curl \
        build-essential \
        srecord \
        udev \
        usbutils \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --upgrade pip \
    && pip install platformio==6.1.16 esptool==4.8.1

RUN mkdir -p /pio /workspace
WORKDIR /workspace

CMD ["pio", "--help"]
