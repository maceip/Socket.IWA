#!/bin/bash
# download_icons.sh — Extract semantically relevant Streamline Emoji SVGs
#                      from streamline-emojis.json for the Socket.IWA project.
#
# Icons are selected for: networking, server, security, status, communication,
# tools, technology, and UI feedback contexts.
#
# Usage: ./download_icons.sh
#
set -e
cd "$(dirname "$0")"

JSON="$HOME/projects/stare-network/streamline-emojis.json"
OUTDIR="./iwa-sink/iwa-sink/public/icons"

mkdir -p "$OUTDIR"

# Semantically relevant icons for a QUIC echo server / networking IWA:
#
#   Networking & Communication:
#     satellite-antenna   — server broadcast / antenna
#     electric-plug       — connection established
#     telephone           — communication channel
#     e-mail-1            — message / packet
#     e-mail-2            — message variant
#     speaking-head       — echo (voice bouncing back)
#     right-anger-bubble  — message bubble
#     thought-balloon     — thinking / processing
#
#   Server & Technology:
#     robot-face-1        — automation / server process
#     robot-face-2        — server variant
#     computer-disk       — storage / data
#     floppy-disk         — save / disk
#     television          — display / monitor
#     fax-machine         — legacy network device
#     videocassette       — recording / logging
#     microscope          — inspect / debug
#     telescope           — observe / monitor
#     flashlight          — search / discover
#     crystal-ball-1      — predict / status
#
#   Security & Auth:
#     locked-with-key     — TLS / encryption / certificate
#     detective-1         — security audit
#     guard-1             — firewall / protection
#     crown               — admin / root
#     graduation-cap      — certificate (academic → TLS cert pun)
#
#   Status & Feedback:
#     ballot-box-with-check — success / check
#     direct-hit           — target / connected
#     high-voltage          — active / power
#     fire                  — hot / error / active
#     sparkles              — new / fresh / initialized
#     collision             — error / crash
#     bomb                  — fatal error
#     bell                  — notification / alert
#     bell-with-slash       — muted / disabled
#     zzz                   — idle / sleeping
#     hourglass-not-done-1  — loading / waiting
#     hourglass-done        — complete / timeout
#     bar-chart             — metrics / stats
#     watch                 — timing / latency
#
#   Actions & Gestures:
#     thumbs-up-1          — OK / success
#     thumbs-down-1        — fail
#     raised-hand-1        — stop / handshake
#     waving-hand-1        — hello / greeting (connection init)
#     clapping-hands-1     — success celebration
#     oncoming-fist-1      — bump / connect
#     victory-hand-1       — v2 / peace / success
#     flexed-biceps-1      — strong / healthy
#
#   Transport (for QUIC/packet metaphors):
#     rocket               — launch / fast
#     speedboat            — fast transport
#     delivery-truck       — packet delivery
#     package              — data packet
#     sailboat             — transport / protocol
#
#   Nature (for status metaphors):
#     water-wave           — stream / flow
#     droplet              — single packet
#     snowflake            — frozen / stale
#     rainbow              — spectrum / multi-protocol
#     four-leaf-clover     — lucky / healthy
#
#   Faces (for status display):
#     slightly-smiling-face — all good
#     grinning-face         — success
#     nerd-face             — technical
#     thinking-face         — processing
#     star-struck-1         — amazing / milestone
#     exploding-head        — mind blown / overload
#     face-with-monocle     — inspecting
#     sleeping-face         — idle
#     nauseated-face-1      — sick / unhealthy
#     skull                 — dead / crashed
#     alien                 — unknown / foreign connection
#     ghost                 — phantom / disconnected

ICONS=(
  satellite-antenna
  electric-plug
  telephone
  e-mail-1
  e-mail-2
  speaking-head
  right-anger-bubble
  thought-balloon
  robot-face-1
  robot-face-2
  computer-disk
  floppy-disk
  television
  fax-machine
  videocassette
  microscope
  telescope
  flashlight
  crystal-ball-1
  locked-with-key
  detective-1
  guard-1
  crown
  graduation-cap
  ballot-box-with-check
  direct-hit
  high-voltage
  fire
  sparkles
  collision
  bomb
  bell
  bell-with-slash
  zzz
  hourglass-not-done-1
  hourglass-done
  bar-chart
  watch
  thumbs-up-1
  thumbs-down-1
  raised-hand-1
  waving-hand-1
  clapping-hands-1
  oncoming-fist-1
  victory-hand-1
  flexed-biceps-1
  rocket
  speedboat
  delivery-truck
  package
  sailboat
  water-wave
  droplet
  snowflake
  rainbow
  four-leaf-clover
  slightly-smiling-face
  grinning-face
  nerd-face
  thinking-face
  star-struck-1
  exploding-head
  face-with-monocle
  sleeping-face
  nauseated-face-1
  skull
  alien
  ghost
)

echo "Extracting ${#ICONS[@]} icons from streamline-emojis.json..."
echo ""

python3 -c "
import json, sys, os

with open('$JSON') as f:
    data = json.load(f)

icons = data['icons']
prefix = data.get('prefix', 'streamline-emojis')
height = data['info']['height']
outdir = '$OUTDIR'
requested = sys.argv[1:]
found = 0
missing = []

for name in requested:
    if name in icons:
        body = icons[name]['body']
        # Wrap in SVG with proper viewBox (Streamline emojis use 0 0 48 48 typically)
        svg = f'''<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 48 48\" width=\"48\" height=\"48\">{body}</svg>'''
        path = os.path.join(outdir, f'{name}.svg')
        with open(path, 'w') as f:
            f.write(svg)
        found += 1
    else:
        missing.append(name)

print(f'  Extracted {found}/{len(requested)} icons to {outdir}/')
if missing:
    sep = ', '
    print(f'  Missing: {sep.join(missing)}')
" "${ICONS[@]}"

echo ""
echo "Done. Icons saved to $OUTDIR/"
ls "$OUTDIR" | wc -l | xargs -I{} echo "  {} SVG files"
