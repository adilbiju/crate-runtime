#!/bin/sh
set -eu

image="crate-integration"
docker build -t "$image" .

docker run --rm --privileged --cgroupns=host \
  -v /sys/fs/cgroup:/sys/fs/cgroup \
  --entrypoint /bin/sh "$image" -c '
set -eu
mkdir -p /rootfs
for directory in bin etc lib opt root sbin srv usr var; do cp -a "/$directory" /rootfs/; done
mkdir -p /rootfs/dev /rootfs/proc /rootfs/sys/fs/cgroup /rootfs/tmp
mkdir -p /escape
ln -s /escape /rootfs/escape-link
printf source > /bind-source
if crate run --name symlink-escape --rootfs /rootfs \
  --bind /bind-source:/escape-link/created -- /bin/true; then
  echo "symlink bind destination unexpectedly succeeded" >&2
  exit 1
fi
test ! -e /escape/created

mkdir -p /readonly-source/nested
mount -t tmpfs tmpfs /readonly-source/nested
if ! crate run --name recursive-readonly --rootfs /rootfs \
  --bind /readonly-source:/data:ro -- /bin/sh -c \
  "! touch /data/nested/probe"; then
  echo "nested mount remained writable through a read-only bind" >&2
  exit 1
fi
test ! -e /readonly-source/nested/probe

set +e
crate run --name missing-foreground --rootfs /rootfs -- /missing-command
missing_status=$?
set -e
test "$missing_status" -eq 127

printf "#!/bin/sh\nexit 0\n" > /rootfs/not-executable
chmod 0644 /rootfs/not-executable
set +e
crate run --name denied-foreground --rootfs /rootfs -- /not-executable
denied_status=$?
set -e
test "$denied_status" -eq 126

if crate run -d --name missing-exec --rootfs /rootfs -- /missing-command; then
  echo "detached missing executable unexpectedly succeeded" >&2
  exit 1
fi

crate run -d --name log-follow --rootfs /rootfs -- /bin/sh -c \
  "echo first; sleep 1; echo second"
followed="$(crate logs log-follow --follow)"
printf "%s\n" "$followed" | grep first
printf "%s\n" "$followed" | grep second

crate run -d --name integration --rootfs /rootfs --memory 64M --cpu 25 --pids 16 \
  --bind /sys/fs/cgroup:/sys/fs/cgroup:ro -- /bin/sh -c \
  "trap \"echo stopped; exit 0\" TERM; echo started; while :; do sleep 1; done"
sleep 1
crate ps | grep integration
crate logs integration | grep started
test "$(cat /sys/fs/cgroup/crate/integration/memory.max)" = 67108864
crate stop integration --timeout 3
sleep 1
crate ps --all | grep exited
crate logs integration | grep stopped
'

docker run --rm --privileged --entrypoint /bin/sh "$image" -c '
set -eu
mkdir -p /rootfs
for directory in bin etc lib opt root sbin srv usr var; do cp -a "/$directory" /rootfs/; done
mkdir -p /rootfs/dev /rootfs/proc /rootfs/sys /rootfs/tmp
nc -l -p 23456 > /tmp/received &
listener=$!
crate run --name network-integration --rootfs /rootfs --network bridge \
  --bridge crate-test0 --ip 10.203.0.2/24 --gateway 10.203.0.1 -- \
  /bin/sh -c "printf container-to-host | nc -w 2 10.203.0.1 23456"
wait "$listener"
test "$(cat /tmp/received)" = container-to-host
'

echo "Crate Docker integration tests passed"
