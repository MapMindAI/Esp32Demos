FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && \
    apt-get install -y tzdata && \
    ln -fs /usr/share/zoneinfo/Asia/Shanghai /etc/localtime && \
    dpkg-reconfigure --frontend noninteractive tzdata

RUN apt-get update && apt-get install -y \
    git wget flex bison gperf python3 python3-pip python3-setuptools \
    cmake ninja-build ccache libffi-dev libssl-dev dfu-util \
    libusb-1.0-0 libusb-1.0-0-dev \
    build-essential curl xz-utils \
    python3.10-venv zip \
    && apt-get clean

# install ESP-IDF  v5.5.2
ENV IDF_VERSION 30aaf64524299d3bde422ca9a2848090d1bc5d0f
ENV IDF_PATH /opt/esp/esp-idf
ENV ARDUINO_ESP_VERSION 3b7500ac47d60b22b942a0ebcac5a312821a55fc

RUN mkdir -p /opt/esp && \
    cd /opt/esp && \
    git clone https://github.com/espressif/esp-idf.git && \
    cd /opt/esp/esp-idf && \
    git checkout ${IDF_VERSION} && \
    git submodule update --init --recursive

# add arduino-esp32
RUN cd /opt/esp && \
    git clone https://github.com/espressif/arduino-esp32.git && \
    cd /opt/esp/arduino-esp32 && \
    git checkout ${ARDUINO_ESP_VERSION} && \
    git submodule update --init --recursive

# add env
# https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/linux-macos-setup.html
ENV PATH="$IDF_PATH/tools:$PATH"
ENV IDF_TOOLS_PATH="/opt/esp/required_idf_tools_path"

RUN /opt/esp/esp-idf/install.sh
RUN /opt/esp/esp-idf/install.sh esp32,esp32s3

RUN git config --global --add safe.directory '*'

RUN echo "source $IDF_PATH/export.sh" >> /etc/bash.bashrc

# set workdir
WORKDIR /work

CMD [ "bash" ]
