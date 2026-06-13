FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends cmake g++ ninja-build && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel && ctest --test-dir build --output-on-failure

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends iproute2 iputils-ping netcat-openbsd && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/crate /usr/local/bin/crate
ENTRYPOINT ["crate"]
