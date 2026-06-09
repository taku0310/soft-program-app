FROM ubuntu:22.04 AS build

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential cmake git pkg-config \
    libmosquitto-dev libssl-dev libpthread-stubs0-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j

FROM ubuntu:22.04 AS runtime

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
    libmosquitto1 libssl3 \
    rt-tests \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/plc_runtime /usr/local/bin/plc_runtime
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
COPY config /etc/softplc

RUN chmod +x /usr/local/bin/entrypoint.sh

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["/usr/local/bin/plc_runtime", "--config", "/etc/softplc/plc_config.json"]
