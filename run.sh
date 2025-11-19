#!/bin/bash
# Run three sync nodes in separate Terminal windows on macOS
SYNC_DIR="$(pwd)/build/Release"

launch_terminal_mac() {
    local SYNC_DIR="$1"
    local CMD="$2"
    local TITLE="$3"

    osascript <<EOF
tell application "Terminal"
    activate
    do script "cd $SYNC_DIR; $CMD; exit"
    delay 0.5
    set custom title of front window to "$TITLE"
end tell
EOF
}

launch_terminal_mac "$SYNC_DIR/sandbox1" "../sync 9000 127.0.0.1 '' peerA --save true" "sync-peerA"
launch_terminal_mac "$SYNC_DIR/sandbox2" "../sync 9001 127.0.0.1 127.0.0.1:9000 peerB --save true" "sync-peerB"
launch_terminal_mac "$SYNC_DIR/sandbox3" "../sync 9002 127.0.0.1 127.0.0.1:9001 peerC --save true" "sync-peerC"

echo "Launched 3 sync nodes in Terminal windows."
