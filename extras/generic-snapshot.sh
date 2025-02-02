#!/bin/sh
#
# Add snapshot functionality for arbitrary file systems to glusterd
# Currently supported filesystems: btrfs
#
# Note: this script is run by glusterd (as root)
#
function fail {
    echo "Error: $1" >&2
    exit 1
}

function assert_numargs {
    [[ $1 -eq $2 ]] || fail "Wrong number of arguments: $1 vs $2"
}

showHelp=0
if [[ $# -eq 0 ]]; then
    showHelp=1
fi

cmd="$1"
shift
case "$cmd" in
  --help|-h)
    showHelp=1
    ;;
esac

if [[ $showHelp -eq 1 ]]; then
  fail "This is an internal helper script used by glusterfs. Do not use directly."
fi

# All commands receive at least 3 arguments: <fs> <brickPath> <mntPath>

# fstype (e.g., btrfs)
fs="$1"
case "$fs" in
  btrfs)
    ;;
  *)
    fail "Unsupported file system: $fs"
esac

# path to brick (e.g., /mnt/xyz/brick1)
brickPath="$2"
# path to mount-path above brick (e.g., /mnt/xyz)
mntPath="$3"

case "$cmd" in
  probe)
    # Check if filesystem/mountpoint is supported
    # args: ...
    assert_numargs $# 3

    # no-op: we already checked fs compatibility above
    ;;
  snapshot)
    # Create a snapshot of a brick file system
    # args: ... <snap-id>
    assert_numargs $# 4
    snapId="$4"

    mkdir -p "$mntPath/.snapshots"
    btrfs subvolume snapshot -r "$mntPath" "$mntPath/.snapshots/$snapId"
    ;;
  clone)
    # Create a clone volume from a snapshot
    # args: ... <snap-id> <clone-id>
    assert_numargs $# 5
    snapId="$4"
    cloneId="$5"

    btrfs subvolume snapshot "$mntPath/.snapshots/$snapId" "$mntPath/$cloneId"
    ;;
  snapshot-path)
    # Return the path to a snapshot's brick
    # args: ... <origPath> <snap-id> <brickPath>
    assert_numargs $# 6

    origPath="$4"
    snapId="$5"
    brickPath="$6"

    echo "$origPath/.snapshots/$snapId$brickPath"
    ;;
  clone-path)
    # Return the path to a clone's brick
    # args: ... <origPath> <clone-id> <brickPath>
    assert_numargs $# 6

    origPath="$4"
    cloneId="$5"
    brickPath="$6"

    echo "$origPath/$cloneId$brickPath"
    ;;
  remove)
    # Remove snapshot
    # args: ... <snap-id>
    assert_numargs $# 4
    snapId="$4"

    btrfs subvolume delete "$mntPath/.snapshots/$snapId"
    ;;
  activate)
    # Activate a snapshot
    # args: ... <snap-id>
    assert_numargs $# 4
    snapId="$4"

    # Nothing to do for btrfs
    ;;
  deactivate)
    # Dectivate a snapshot
    # args: ... <snap-id>
    assert_numargs $# 4
    snapId="$4"

    # Nothing to do for btrfs
    ;;
  *)
    fail "Unknown command"
esac
