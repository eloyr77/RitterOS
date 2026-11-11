#!/bin/sh
# notes' test without a screen (41h-4): how a windowed program is tested
# on RitterOS, over SSH too, while the machine's own desktop runs:
#
#   cat test.sh | ssh HOST 'cat > /tmp/notes-test.sh'
#   ssh HOST sh /tmp/notes-test.sh
#
# A headless window server of its own (WINDOW_SOCKET: where it listens, and
# where notes, wsend and wshot find it), notes in it, a note typed and
# saved with wsend, the screen kept by wshot (/tmp/notes.png, to look at);
# ~/notes.txt must then hold the note.
export WINDOW_SOCKET=/tmp/notes-test
rm -f "$HOME/notes.txt"
windowd headless 640x480 &
server=$!
wsend wait || exit 1                    # (until the server answers)
notes &
app=$!
wsend wait notes || exit 1              # (until its window is open)
wsend click 250 200                     # into the editor (the window opens at 100, 100)
wsend type "milk, eggs, bread"
wsend click 140 360                     # Save
sleep 1                                 # (notes saves and shows its notification)
wshot /tmp/notes.png
kill $app $server
note=$(cat "$HOME/notes.txt")
if [ "$note" = "milk, eggs, bread" ]; then
    echo "notes test: ok"
else
    echo "notes test: FAILED: ~/notes.txt holds '$note'"
    exit 1
fi
