FROM debian:trixie-slim
LABEL maintainer="Nick Zhuravlev <nikolay.zhuravlev@elpitech.ru>"

RUN sed -i 's/deb.debian.org/mirror.docker.ru/g' /etc/apt/sources.list.d/debian.sources

RUN dpkg --add-architecture arm64
RUN apt-get update -y
RUN apt-get -y install libssl-dev:arm64 crossbuild-essential-arm64 \
      build-essential bsdextrautils bison flex bc libssl-dev rsync kmod cpio \
      lsb-release git zstd libelf-dev:native debhelper-compat libncurses5-dev \
      dwarves fakeroot tar curl

RUN useradd -ms /bin/bash user
RUN mkdir /home/user/workspace
RUN echo "user ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers

### USER ###
# USER user
# WORKDIR /home/user/workspace
