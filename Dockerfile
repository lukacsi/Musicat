FROM archlinux:base as init

RUN pacman -Sy --needed --noconfirm openssl reflector && reflector --save /etc/pacman.d/mirrorlist && \
      pacman -Syu --needed --noconfirm libc++ postgresql-libs libsodium opus ffmpeg

FROM init as build

# Build dependencies
WORKDIR /app

# Copy source files
COPY include ./include
COPY src ./src
COPY libs ./libs
COPY CMakeLists.txt ./

# Install dependencies
RUN pacman -Syu --needed --noconfirm base-devel libc++ git cmake libsodium opus postgresql-libs clang

RUN mkdir -p build && cd build && \
      export CC=clang && \
      export CXX=clang++ && \
      export LDFLAGS='-flto -stdlib=libc++ -lc++' && \
      export CFLAGS='-flto' && \
      export CXXFLAGS='-flto -stdlib=libc++' && \
      cmake .. -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
            -DDPP_BUILD_TEST=OFF -DRUN_LDCONFIG=OFF -DDPP_NO_VCPKG=ON -DDPP_USE_EXTERNAL_JSON=ON && make all -j12
      # cmake -DCOMPILE_GNUPLOT=ON .. && make all -j12

FROM init as deploy

RUN groupadd musicat && useradd -m -g musicat musicat

USER musicat

WORKDIR /home/musicat

# create music directory with proper permissions
RUN mkdir -p /home/musicat/music && \
    chown musicat:musicat /home/musicat/music

COPY --chown=musicat:musicat --from=build \
             /app/build/Shasha \
             /app/build/libs/DPP/library/libdpp.so* \
             /app/libs/curlpp/build/libcurlpp.so* \
             /app/libs/icu/usr/local/lib/lib* \
             /app/build/libs/ogg/libogg.so* \
             /app/src/yt-dlp/ytdlp.py \
             /home/musicat/

# /app/libs/gnuplot-*/build/bin/gnuplot \

COPY --chown=musicat:musicat --from=build \
             /app/libs/yt-dlp \
             /home/musicat/yt-dlp/

COPY --chown=musicat:musicat --from=build \
             /app/src/yt-dlp/utils \
             /home/musicat/utils/

ENV LD_LIBRARY_PATH=/home/musicat

VOLUME ["/home/musicat/music"]

# config for container create `-v` switch: /home/musicat/sha_conf.json

CMD ./Shasha
