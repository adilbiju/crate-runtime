# Crate

Crate is a small Linux container runtime I built in C++ to learn how containers
work under the hood. It uses namespaces, `pivot_root`, cgroups v2, and a tiny
PID 1—no daemon required.

It supports:

- PID, hostname, mount, IPC, user, and network namespaces
- Isolated root filesystems, `/proc`, `/dev`, and `/tmp`
- Memory, CPU, and PID limits with cgroups v2
- Host, isolated, and bridge/veth networking
- Read-only roots and bind mounts
- Detached containers, logs, `ps`, and `stop`
- Simple `key=value` config files

This is a side project, not a production security boundary. If you need to run
real workloads, use Docker, or runc.

## Build

You need Linux, CMake 3.20+, a C++20 compiler, and `iproute2` for bridge
networking.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

You can also build it with Docker:

```sh
docker build -t crate .
```

## Usage

Crate needs an extracted Linux root filesystem. Once you have one:

```sh
sudo ./build/crate run \
  --name demo \
  --rootfs ./rootfs \
  --memory 256M \
  --cpu 50 \
  --pids 64 \
  -- /bin/sh
```

Run something in the background:

```sh
sudo ./build/crate run -d --name worker --rootfs ./rootfs -- /bin/sh -c \
  'while true; do echo working; sleep 2; done'

sudo ./build/crate ps
sudo ./build/crate logs worker --follow
sudo ./build/crate stop worker
```

Bridge networking creates a bridge and veth pair so the container can reach the
host and other containers on the same bridge:

```sh
sudo ./build/crate run --name web --rootfs ./rootfs \
  --network bridge --bridge crate0 \
  --ip 10.88.0.2/24 --gateway 10.88.0.1 \
  -- /bin/sh
```


## Tests

The regular tests run through CTest. There is also a privileged Docker test that
covers namespaces, cgroups, detached containers, logs, and bridge networking:

```sh
./tests/docker_integration.sh
```

Configuration examples in [`examples/crate.conf`](examples/crate.conf).
