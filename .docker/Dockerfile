FROM --platform=linux/arm64 debian:bookworm-slim
LABEL maintainer="Nick Zhuravlev <nikolay.zhuravlev@elpitech.ru>"

RUN sed -i 's/deb.debian.org/mirror.docker.ru/g' /etc/apt/sources.list.d/debian.sources
RUN sed -i 's/security.debian.org/mirror.docker.ru/g' /etc/apt/sources.list.d/debian.sources
RUN sed -i 's/Types: deb/Types: deb deb-src/g' /etc/apt/sources.list.d/debian.sources

RUN apt-get update -y 
RUN apt-get -y install debhelper-compat directx-headers-dev glslang-tools \
meson quilt pkg-config libdrm-dev libx11-dev libxxf86vm-dev libexpat1-dev \
libsensors-dev libxfixes-dev libxext-dev libva-dev libvulkan-dev \
libx11-xcb-dev libxcb-dri2-0-dev libvdpau-dev libxcb-glx0-dev \
libxcb-xfixes0-dev libxcb-dri3-dev libxcb-present-dev libxcb-randr0-dev \
libxcb-shm0-dev libxcb-sync-dev libxrandr-dev libxshmfence-dev libzstd-dev \
python3-mako python3-ply flex bison libelf-dev libwayland-egl-backend-dev \
llvm-15-dev libclang-15-dev libclang-cpp15-dev libclc-15-dev \
wayland-protocols zlib1g-dev libglvnd-core-dev valgrind rustc bindgen \
llvm-spirv-15 libclc-15 libllvmspirvlib-15-dev zstd build-essential \
git fakeroot libncurses-dev libssl-dev vim device-tree-compiler sloccount \
cppcheck debootstrap sudo nasm iasl python3-distutils texinfo xxd \
librsvg2-bin uuid-dev libgmp3-dev libmpfr-dev libarchive-zip-perl \
curl acpica-tools xz-utils lsb-release bc rsync kmod cpio bsdextrautils
