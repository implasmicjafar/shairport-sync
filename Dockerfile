FROM crazymax/alpine-s6:3.17-3.1.1.2

ENV S6_CMD_WAIT_FOR_SERVICES=1
ENV S6_CMD_WAIT_FOR_SERVICES_MAXTIME=0

# Check required arguments exist. These will be provided by the Github Action
# Workflow and are required to ensure the correct branches are being used.
#ARG SHAIRPORT_SYNC_BRANCH
#RUN test -n "$SHAIRPORT_SYNC_BRANCH"
#ARG NQPTP_BRANCH
#RUN test -n "$NQPTP_BRANCH"

RUN apk -U add \
        alsa-lib-dev \
        autoconf \
        automake \
        avahi-dev \
        avahi-tools \
        glib \
        less \
        build-base \
        dbus \
        ffmpeg-dev \
        git \
        libconfig-dev \
        libgcrypt-dev \
        libplist-dev \
        libressl-dev \
        libsndfile-dev \
        libsodium-dev \
        libtool \
        mosquitto-dev \
        popt-dev \
        pulseaudio-dev \
        soxr-dev \
        xxd \
        gdb \
        nano

##### ALAC #####
RUN git clone https://github.com/mikebrady/alac
WORKDIR /alac
RUN autoreconf -i
RUN ./configure
RUN make
RUN make install
WORKDIR /
##### ALAC END #####

##### NQPTP #####
RUN git clone https://github.com/mikebrady/nqptp
WORKDIR /nqptp
RUN git checkout main
RUN autoreconf -i
RUN ./configure
RUN make
WORKDIR /
##### NQPTP END #####

##### SPS #####
RUN git clone https://github.com/implasmicjafar/shairport-sync.git
WORKDIR /shairport-sync
COPY . .
RUN git checkout master
WORKDIR /shairport-sync/build
RUN autoreconf -i ../
RUN ../configure --sysconfdir=/etc --with-alsa --with-pa --with-soxr --with-avahi --with-ssl=openssl \
        --with-airplay-2 --with-metadata --with-dummy --with-pipe --with-dbus-interface \
        --with-stdout --with-mpris-interface --with-mqtt-client \
        --with-apple-alac --with-convolution
RUN make -j $(nproc)
RUN DESTDIR=install make install
WORKDIR /
##### SPS END #####

RUN ln -s /shairport-sync/build/install/usr/local/bin/shairport-sync /usr/local/bin/shairport-sync
RUN mkdir -p /usr/share/man/man7
RUN cp -R /shairport-sync/build/install/usr/local/share/man/man7/. /usr/share/man/man7
RUN ln -s /nqptp/nqptp /usr/local/bin/nqptp
RUN ln -s /shairport-sync/build/install/etc/shairport-sync.conf /etc/shairport-sync.conf
RUN ln -s /shairport-sync/build/install/etc/shairport-sync.conf.sample /etc/shairport-sync.conf.sample
RUN mkdir -p /etc/dbus-1/system.d
RUN cp /shairport-sync/build/install/etc/dbus-1/system.d/shairport-sync-dbus.conf /etc/dbus-1/system.d/shairport-sync-dbus.conf
RUN cp /shairport-sync/build/install/etc/dbus-1/system.d/shairport-sync-mpris.conf /etc/dbus-1/system.d/shairport-sync-mpris.conf

RUN mkdir -p /etc/s6-overlay
RUN cp -R /shairport-sync/docker/etc/s6-overlay/. /etc/s6-overlay
RUN cp -R /shairport-sync/docker/etc/pulse/. /etc/pulse
RUN chmod +x /etc/s6-overlay/s6-rc.d/01-startup/script.sh

RUN addgroup shairport-sync
RUN adduser -D shairport-sync -G shairport-sync

RUN addgroup -g 29 docker_audio && addgroup shairport-sync docker_audio && addgroup shairport-sync audio

RUN ln -s /shairport-sync/docker/run.sh /run.sh
RUN ln -s /shairport-sync/runstart.sh /runstart.sh
RUN chmod +x /run.sh
RUN chmod +x /runstart.sh

Entrypoint ["/init","./runstart.sh"]
