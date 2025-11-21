#!/bin/bash
# Run three sync nodes in separate Terminal windows on macOS
SYNC_DIR="$(pwd)/build/Release"
TERMINAL_COLUMNS="${TERMINAL_COLUMNS:-160}"
TERMINAL_ROWS="${TERMINAL_ROWS:-20}"

WINDOW_Y_OFFSET=0

launch_terminal_mac() {
    local SYNC_DIR="$1"
    local CMD="$2"
    local TITLE="$3"

    # Call AppleScript, unchanged except for bounds logic
    local WINHEIGHT=$(osascript <<EOF
tell application "Terminal"
    activate
    set newTab to do script "cd \"$SYNC_DIR\"; $CMD; exit"

    -- Let Terminal actually create the window
    delay 0.25

    set w to front window
    set number of columns of w to ${TERMINAL_COLUMNS}
    set number of rows of w to ${TERMINAL_ROWS}

    -- Make sure Terminal recalculates the pixel geometry
    delay 0.1

    -- Read the actual size Terminal computed
    set {x1, y1, x2, y2} to bounds of w
    set winWidth to (x2 - x1)
    set winHeight to (y2 - y1)

    -- Stack windows vertically using offset from bash
    set bounds of w to {0, ${WINDOW_Y_OFFSET}, winWidth, ${WINDOW_Y_OFFSET} + winHeight}

    return winHeight
end tell
EOF
)

    # Increase offset for next window
    WINDOW_Y_OFFSET=$((WINDOW_Y_OFFSET + WINHEIGHT))
}

launch_terminal_mac "$SYNC_DIR/sandbox1" "../sync 9000 127.0.0.1 '' peerA --save true" "sync-peerA"
launch_terminal_mac "$SYNC_DIR/sandbox2" "../sync 9001 127.0.0.1 127.0.0.1:9000 peerB --save true" "sync-peerB"
launch_terminal_mac "$SYNC_DIR/sandbox3" "../sync 9002 127.0.0.1 127.0.0.1:9001 peerC --save true" "sync-peerC"

echo "Launched 3 sync nodes in Terminal windows."
