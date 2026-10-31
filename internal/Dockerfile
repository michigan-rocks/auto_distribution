FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    cmake \
    curl \
    g++ \
    git \
    make \
    pkg-config \
    tar \
    unzip \
    zip \
    && rm -rf /var/lib/apt/lists/*

# Устанавливаем vcpkg
RUN git clone --depth 1 \
    https://github.com/microsoft/vcpkg.git /opt/vcpkg

RUN /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

# Только RabbitMQ-клиент и его зависимости
RUN /opt/vcpkg/vcpkg install \
    simpleamqpclient:x64-linux-dynamic

WORKDIR /src
COPY . .

# Собираем только internal
RUN cmake -S . -B build \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_TARGET_TRIPLET=x64-linux-dynamic \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_PRODUCER=OFF \
    && cmake --build build --target internal -j"$(nproc)"

FROM ubuntu:24.04 AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/internal /usr/local/bin/internal

COPY --from=build \
    /opt/vcpkg/installed/x64-linux-dynamic/lib/*.so* \
    /usr/local/lib/

RUN ldconfig

WORKDIR /app

CMD ["/usr/local/bin/internal"]
