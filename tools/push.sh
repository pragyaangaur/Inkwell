#!/bin/sh
# Pushes files to the running system over SSH without rebuilding the image.
# Each file goes to a temporary name and is moved into place, so the mode and
# any running copy survive. Usage: tools/push.sh local:remote[:mode] ...
cd "$(dirname "$0")/.."
for spec in "$@"; do
    src=${spec%%:*}; rest=${spec#*:}; dst=${rest%%:*}; mode=${rest#*:}
    [ "$mode" = "$rest" ] && mode=644
    ./ssh.sh "cat > '$dst.new' && chmod $mode '$dst.new' && mv '$dst.new' '$dst'" < "$src" || exit 1
    echo "pushed $dst"
done
