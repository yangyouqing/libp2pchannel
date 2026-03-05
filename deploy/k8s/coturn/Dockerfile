FROM ubuntu:22.04

RUN apt-get update \
    && apt-get install -y --no-install-recommends coturn \
    && rm -rf /var/lib/apt/lists/*

EXPOSE 3478/udp 3478/tcp 5349/tcp
EXPOSE 49152-50200/udp

ENTRYPOINT ["turnserver"]
CMD ["-c", "/etc/turnserver.conf"]
