#!/bin/bash
# Close sync Terminal windows by title

pkill -9 -f "./sync"

# for TITLE in "sync-peerA" "sync-peerB" "sync-peerC"; do
# osascript <<EOF
# tell application "iTerm"
#     set allWindows to windows
#     repeat with w in allWindows
#         repeat with t in tabs of w
#             if name of t is "sync-peerA" then
#                 close t
#             end if
#         end repeat
#     end repeat
# end tell
# EOF
# done

echo "Closed all sync Terminal windows."
