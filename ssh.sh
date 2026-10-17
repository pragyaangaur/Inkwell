#!/bin/sh
# SSH into Inkwell on the Kindle over USB. KINDLE_IP overrides the address.
# Commands get the full PATH, since dropbear only gives /usr/bin:/bin.
# The first run makes a key in keys/ and says where to put its public half.
cd "$(dirname "$0")"
if [ ! -f keys/kindle_ed25519 ]; then
    mkdir -p keys
    ssh-keygen -q -t ed25519 -N '' -C inkwell -f keys/kindle_ed25519 || exit 1
    cat <<EOF
Made a new SSH key. To let it in, connect the Kindle in stock mode (or hold
power at the Inkwell splash), copy this file to the Kindle's drive as
linux/authorized_keys, eject, and boot Inkwell again:

    $(pwd)/keys/kindle_ed25519.pub

EOF
    exit 0
fi
OPTS="-i keys/kindle_ed25519 -p 2222 -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=keys/known_hosts -o ConnectTimeout=8 -o ServerAliveInterval=15"
HOST=root@"${KINDLE_IP:-192.168.15.244}"
while [ $# -gt 0 ] && [ "${1#-}" != "$1" ]; do OPTS="$OPTS $1"; shift; done
if [ $# -gt 0 ]; then
    exec ssh $OPTS "$HOST" "export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; $*"
fi
exec ssh $OPTS "$HOST"
