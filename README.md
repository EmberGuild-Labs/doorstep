# Doorstep (Flipper Zero)

A Flipper Zero app that tells you, after a long walk, **which camera brands were within Wi-Fi range of you along the way**, as a simple tally. It produces counts per brand plus the total length of the walk, and nothing else. No timestamps for individual detections, no locations, no addresses, no device details.

```
+------------------------------+
|######  WALK RECEIPT  ########|
| Duration         01:12:05   ^|
| Ring ................. 23   #|
| Blink ................. 4   :|
| Nest .................. 2   :|
| Arlo .................. 0   v|
|##  TOTAL ............ 147  ##|
+------------------------------+
```

## Status

v1.1. The app is written, builds cleanly with `ufbt` (Momentum SDK `mntm-012`, API 87.1), and the rules parser, line matcher, radio count and saved-walk parser pass host-side tests. **Verified on hardware (2026-10-13):** on the author's Flipper (Momentum firmware, official WiFi Dev Board, Marauder v1.9.0), Check board reports the version and "Walks scan ALL", and a walk past the author's own Vivint doorbell counted it under Vivint. Still untested: long walks, busy streets, and Marauder v1.11+ on hardware.

## What the numbers mean

Each number is the count of **distinct devices matching that brand's radio signature that transmitted within Wi-Fi range of you** during the walk. It is not a count of cameras that filmed you, and it does not mean the device pointed at the street. Treat it as a rough tally of camera-brand radios you walked past, and expect it to be imperfect in both directions (see [Limitations](#limitations)).

## Hardware

- Flipper Zero
- WiFi Developer Board (ESP32) running Marauder firmware, mounted on the GPIO header

The Flipper has no Wi-Fi radio of its own. Scanning is done by the Marauder board, and the app talks to it over the GPIO UART (USART, header pins 13 TX and 14 RX) at 115200 baud, sending commands and reading text lines back. The app pauses the Flipper's expansion-module service while it holds the UART and restores it afterwards. No GPS module is used or supported.

Bluetooth LE is out of scope for v1. Stock Flipper firmware gives external apps no general BLE scanner, and the original Dev Board (ESP32-S2) has no Bluetooth radio.

## Build and install

Toolchain is [ufbt](https://github.com/flipperdevices/flipperzero-ufbt). Build against the SDK that matches your Flipper's firmware (this was built against Momentum `mntm-012`; for stock firmware run `ufbt update --channel=release` first).

```bash
pip install ufbt
cd doorstep
ufbt            # builds dist/doorstep.fap
ufbt launch     # builds, installs and runs on a USB-connected Flipper
```

To keep it installed, copy `dist/doorstep.fap` to `/ext/apps/GPIO/`. Marauder firmware setup is covered by the WiFi Dev Board's own flashing guide.

On first launch the app copies its bundled `rules.txt` to `/ext/apps_data/doorstep/rules.txt`. After that, the SD-card copy is the one used, so your edits are kept.

## Usage

1. Seat the WiFi Dev Board, open **Apps → GPIO → Doorstep**.
2. Optional: **Settings → Check board** to confirm Marauder answers.
3. **Start Walk**, pocket the Flipper, walk.
4. **Hold OK** to pause or resume (one buzz confirms; the strip shows `PAUSED - hold OK`, the board stops scanning, and paused time isn't counted). Press **Back** → "End walk?" → **End** to print the receipt.
5. The receipt prints row by row (any key skips). **Up/Down** scroll, **OK** saves one line to `tallies.txt`, **Back** finishes (asks first if unsaved).
6. **Saved Walks** lists your saved receipts, newest first, under a **Lifetime totals** entry. OK on a saved walk offers to delete it.

## Screens

| Screen | What it shows |
|---|---|
| Menu | Start Walk, Saved Walks, Rules, Settings, About |
| Walk | Top bar: title, radio icon (pulses during a scan pass), elapsed time. Middle: running total in large digits. Bottom: progress bar for the current pass plus `STA`/`AP`. Problems replace the bar with an inverted alert: `! Board not responding`, `! UART in use`, `! Memory full`. Only Back does anything, so pocket presses can't end the walk. |
| Receipt | Inverted header, Duration row, Cameras/hour row (TOTAL ÷ duration, one decimal; `-` under a minute), Radios heard row, one row per brand with dotted leaders, `Other` row, TOTAL pinned in an inverted bar with a torn-paper edge, scrollbar when rows overflow. A fresh receipt "prints": rows appear 40 ms apart, then TOTAL rises into place over 3 decelerating frames (~280 ms total); any key skips it, and it doesn't replay when you come back from a dialog. |
| Saved Walks | **Lifetime totals** first: every saved walk added up (titled `ALL 12 WALKS`: total time, cameras/hour overall, per-brand sums, TOTAL; no radios row, since the same radio on two walks would count twice). Then the newest 50 saved walks as `#12  1:12:05  147` (walk number, duration, total). Opening one shows it as a receipt titled `WALK #12`, with no print animation. OK → "Delete walk #12?" → **Delete** removes that line from `tallies.txt` (other lines, including comments, are kept; the file is rewritten via a temp file). A small ▲/▼ beside a brand means its per-hour rate on that walk is more than 15% above/below your average across all saved walks (needs at least 2 saved walks with that brand). Back returns to the list. |
| Rules | Brands / prefixes / name patterns loaded; OK reloads from SD. |
| Settings | Scan pass (10/20/30/60 s, default 20), Mode (APs/All, default All), Range (Near/Street/Max, default Street), Buzz on new (default Off; at most one buzz every 5 s, so a burst of cameras gives one buzz), Check board. Saved to `settings.txt` on leaving. |
| About | Version and scope statement. |

## Range

The ESP32 dev board, with its small PCB antenna, listens passively on 2.4 GHz. Rough reach:

| Situation | Typical reach |
|---|---|
| Doorbell / porch camera, sidewalk to door | 5–20 m, reliable |
| Camera inside a house, through a wall | ~10–30 m |
| Outdoor line of sight, strong transmitter | 50–100 m+ |

Unfiltered, that would count cameras a block away. The **Range** setting drops weak signals:

| Setting | Signal floor | Roughly |
|---|---|---|
| Near | ≥ −65 dBm | the doorstep you're passing |
| Street (default) | ≥ −75 dBm | houses on both sides of the street |
| Max | no filter | everything the board hears |

These are approximate: signal strength only loosely tracks distance (walls, your pocket and the camera's transmit power all change it). The signal value is read from the line (either `RSSI: -60` or Marauder's AP format, which starts the line with the value: `-67 Ch: 6 …`), compared, and discarded; it is never stored.

**Client devices are never range-filtered.** Marauder's station lines (`ap: X -> sta: Y`) carry no signal value, so Range only affects devices seen as access points.

## How it works

1. **Start Walk.** A worker thread runs a short handshake: `stopscan`, clear the board's AP and station lists (`clearlist -a`, `clearlist -c`), then `info` to read the Marauder version (up to 1.5 s; the walk screen shows `...` meanwhile). Then it repeats scan passes, each followed by `stopscan` and a 0.6 s gap:
   - **APs** mode: `scanap`.
   - **All** mode: `scanall` (APs and client stations in one pass, Marauder v1.11+). The first `scanall` is a probe: if the board doesn't acknowledge it ("Scanning for APs and Stations") within 1.5 s, the firmware is treated as older and the walk alternates `scansta` / `scanap` instead. The walk screen labels each pass `ALL`, `STA` or `AP`.
 At End Walk it clears both lists again so network names don't linger on the board. (Clearing matters for counting too: Marauder never re-prints a station already in its list.)
2. **Parse each line** without depending on an exact format: every `xx:xx:xx:xx:xx:xx` on the line is a candidate device (up to 4 per line), `RSSI:` gives signal strength, and text after `ESSID:` is the network name.
3. **Match to a brand, in memory.** Multicast addresses are skipped. Locally administered (randomized) addresses are never prefix-matched, because their first three bytes carry no vendor. Otherwise the first 3 bytes are looked up in the sorted prefix table. Name patterns apply to the first address on the line (the BSSID). First matching rule (lowest line in the file) wins, so a device is only counted under one brand.
4. **Already counted?** A salted 32-bit hash (FNV-1a seeded with a random salt made at Start Walk, plus a murmur3 finalizer) is kept in a sorted array. The raw address is never kept.
5. **Tally.** A new matching device adds one to its brand. Separately, every distinct unicast address in range (any vendor, randomized ones included) adds one to **Radios heard**, the context number on the receipt ("147 cameras of 812 radios"). It uses its own hash domain, so it never stops a camera being counted. Nothing else about non-matching devices is kept.
6. **End walk.** The hash list and salt are wiped, and only the counts go to the receipt.

## Rules file

`/ext/apps_data/doorstep/rules.txt`, one brand per line:

```
Brand [other] = prefixes: <6-hex> <6-hex> ...   names: <substring> ...
```

- `prefixes:` 24-bit OUIs, written `286FB9`, `28:6F:B9` or `28-6F-B9`.
- `names:` case-insensitive substrings matched against network names (APs only).
- `[other]` rolls that brand into the receipt's single `Other` line.
- A brand can span several lines; they're merged. `#` starts a comment.
- Limits: 32 brands, 512 prefixes, 64 name patterns.

### Bundled rules (from IEEE `oui.csv`)

`tools/build_rules.py` generates the bundled file from the IEEE OUI listing, matching on the **Organization Name** column only:

```bash
python3 tools/build_rules.py oui.csv > doorstep/files/rules.txt
```

| Brand | Prefixes | IEEE organization(s) |
|---|---|---|
| Ring | 13 | Ring LLC |
| Blink | 6 | Blink by Amazon |
| Nest | 2 | Nest Labs Inc. |
| Arlo | 3 | Arlo Technology |
| Wyze | 5 | Wyze Labs |
| Eufy | 0 | none: Anker/Eufy has no OUI in the listing |
| Vivint | 4 | Vivint Inc / Vivint Smart Home / Vivint Wireless |
| SimpliSafe | 1 | SimpliSafe |
| Hikvision | 103 | Hikvision, Prama Hikvision, Hangzhou Ezviz, Hangzhou Microimage |
| Dahua | 39 | Zhejiang Dahua, Hangzhou Huacheng (Imou) |
| Reolink | 1 | Reolink Innovation |
| Other | 34 | SkyBell, Canary Connect, Lorex, Swann, Amcrest, Night Owl, Zmodo, Alarm.com, Shanghai Xiaoyi / Imilab (YI), Axis, Hanwha Vision / Samsung Techwin, Vivotek, Mobotix, Uniview, Verkada |

Total: 26 brands, 212 prefixes. To update, download a fresh `oui.csv` from the IEEE Registration Authority (<https://regauth.standards.ieee.org/>, bulk file commonly at `https://standards-oui.ieee.org/oui/oui.csv`), re-run the script, rebuild, and delete the SD-card copy so the new one installs (or edit the SD copy directly).

**Deliberately excluded:** parent companies whose prefixes mostly belong to non-camera products: Amazon Technologies (201 prefixes: Echo, Kindle, Fire TV…), Google (112: Pixel phones, Chromecast…), NETGEAR (routers; older Arlo), Xiaomi, Tuya, Espressif, TP-Link (Tapo), Logitech (Circle) and Ubiquiti. Including Google alone would count every passing Pixel phone as a "Nest". The trade-off: **Ring and Nest undercount** (many Ring units and most newer Nest cams use Amazon/Google prefixes). If you'd rather overcount, add those companies' prefixes to the relevant brand lines yourself.

Eufy needs `names:` patterns to ever match; add them once you've seen what Eufy devices broadcast.

## Data and storage

| File | Contents |
|---|---|
| `/ext/apps_data/doorstep/rules.txt` | Your rules (installed from the app's bundled copy on first run) |
| `/ext/apps_data/doorstep/settings.txt` | Scan pass, mode, range, buzz |
| `/ext/apps_data/doorstep/tallies.txt` | Only if you save: one line per walk, e.g. `duration=01:12:05 Ring=23 Blink=4 ... Other=3 Radios=812 Total=147`. No date, no clock time. `Radios=3500+` means the radio count hit its cap. Saved Walks reads this file; lines without `Radios=` (from v1.0) still load. |

The app never stores addresses, network names, signal strengths, per-detection timestamps, or positions. The hash list lives in RAM only, and it and the salt are zeroed at End Walk. Line buffers and the UART receive buffer, which can hold addresses and network names, are zeroed after use. The hash list holds up to 4,000 entries; the last 500 are reserved for cameras so a busy street full of phones can't crowd them out. When the radio budget runs out the radio count shows as `N+`; when the list is completely full, the walk screen shows `! Memory full` and new cameras stop being added (old ones are not evicted). Note that the SD card records `tallies.txt`'s last-modified time, so the file system knows when you last saved.

**Marauder board caveat:** on Marauder v1.11.0, `scanap` also writes a pcap capture to the *board's* SD card when its `SavePCAP` setting is on (the default). If your board has an SD card, that file holds raw frames. Doorstep doesn't change board settings; see [Open items](#open-items).

## Tested firmware

| Marauder | `scanall` | AP line format | Notes |
|---|---|---|---|
| v1.9.0 (author's board, official WiFi Dev Board) | yes | `RSSI: -67 Ch: 6 BSSID: <mac> ESSID: <name>` | Checked against the v1.9.0 source; Check board reports `v1.9.0` on hardware. |
| v1.11.0 | yes (`scansta` removed) | `-67 Ch: 6 <mac> ESSID: <name> …` | Checked against source only. |

Station lines are `N: ap: <mac> -> sta: <mac>` (or `sta: … -> ap: …`) in both, with no signal value.

## Verify on hardware

Marauder's output format differs between firmware versions, and the parser has only been tested against representative line shapes, not captured output. On first real use:

1. Run a short walk with **Range: Max** next to a camera you own and confirm it counts.
2. If counts stay at 0 while the board is clearly scanning, capture a few lines of `scanap`/`scansta` output (Flipper **GPIO → USB-UART Bridge** and a serial terminal at 115200 baud) and check that they contain `xx:xx:xx:xx:xx:xx` addresses and, for range filtering, an `RSSI` field. Blank out the addresses before sharing.
3. `Check board` sends `info` and passes if any byte comes back within 2 s; it shows the Marauder version if one is reported (e.g. `Marauder v1.11.0`).
4. On a v1.11+ board the walk screen should show `ALL` passes; `STA`/`AP` alternating means the `scanall` probe failed and the older commands are in use.

## Open items

Found in code review (2026-10-12), pending a decision:

- **Board-side pcap (`SavePCAP`)**: whether Doorstep should send `settings -s SavePCAP disable`, which persists on the board. Not urgent for the official Flipper WiFi Dev Board: it has no SD card slot, so Marauder has nowhere to write the capture. It only matters for ESP32 boards with an SD card.

## Project layout

```
README.md
oui.csv                      IEEE OUI listing (input to the generator)
tools/build_rules.py         oui.csv -> rules.txt
doorstep/
  application.fam            app manifest (appid doorstep, GPIO category)
  doorstep.c                 app: menu, walk/receipt flow, dialogs, settings, storage
  marauder.c/.h              UART worker: handshake (info, scanall probe), scan passes, board check
  tally.c/.h                 line parser, salted hash set, per-brand counts
  rules.c/.h                 rules.txt parser and prefix/name lookup
  history.c/.h               tallies.txt parser for Saved Walks
  chars.h                    ASCII helpers (firmware doesn't export libc ctype/qsort)
  views/walk_view.c/.h       walk screen
  views/receipt_view.c/.h    receipt screen
  files/rules.txt            bundled rules (installed to SD on first run)
  doorstep_10px.png          menu icon
```

## Decisions and notes

- **Mode APs/All (v1.1)**: Marauder v1.11.0 removed `scansta`, so the old Clients/Both modes scanned nothing half the time. `scanall` with an automatic fallback replaces them; settings files from v1.0 reset to defaults.
- **`stopscan` after `info`**: `info` leaves Marauder in its SHOW_INFO mode, and Marauder ignores every scan command (no error, no reply) until the mode is off. Found on hardware when the `scanall` probe kept failing on v1.9.0.
- **Memory full alarm**: when the device list is completely full, the Flipper gives three long buzzes with the red LED, once per walk, whatever the Buzz setting, since the screen is usually in a pocket.
- **Low-battery auto-end**: if the battery drops below 10% while not charging, the walk ends and saves itself (popup: "Battery low / Walk ended and saved"). If the walk started below 10%, it steps in at 3% instead.
- **Pause stops the board, not just the counting**: Marauder never re-prints a station already in its list, so ignoring lines during a pause would hide those devices afterwards. Pausing sends `stopscan`; resuming clears the board's lists so everything is reported again, and the walk's dedupe stops double counts.
- **Print animation only on fresh receipts**: it's the once-per-walk moment. Browsing saved walks happens often, so those open instantly.
- **Trend marks compare per-hour rates**, not raw counts, so a long walk doesn't look like a camera-heavy one.
- **Receipt UI**: chosen over a radar-style screen. During the walk only the running total is shown, by design; the per-brand breakdown appears only once the walk ends.
- **Strict prefixes over broad ones**: undercounting a brand is better than counting phones and smart speakers as cameras.
- **Randomized MACs skipped** for prefix matching. A device rotating its address mid-walk can still be counted twice if it matches a name pattern.
- **Range filter defaults to Street**, so the tally reflects what you walked past, not what the antenna could just about hear.
- **Buzz defaults off**: a pocketed Flipper vibrating every few seconds on a busy street is noise.
- **Back on the walk screen always confirms**, and other keys are swallowed so pocket presses can't end the walk.
- **Board check owns the UART** while it runs: its popup can't be dismissed, and Start Walk is blocked until it reports, so the walk and the check can never fight over the port.
- Popup timeouts are handed to the GUI thread as custom events tagged with a generation number, so a late timer can't close the wrong screen.
- The Flipper firmware doesn't export `qsort` or libc's ctype functions to apps, so the code uses an insertion sort (rules load once) and small local ASCII helpers.

## Limitations

- **Brand matching is a heuristic.** Companies reuse prefixes across product lines, and a camera using a prefix you did not list is missed.
- **It undercounts overall.** Wired and PoE cameras are invisible, and so are cameras on 5 GHz (the ESP32 only sees 2.4 GHz). Battery cameras sleep and may never transmit while you pass.
- **Randomized addresses** cannot be matched by prefix.
- **Range varies.** Walls, distance and your pocket change what is heard, so tallies from different routes are only loosely comparable.
- **In range is not in view.** The tally says nothing about what any camera can actually see.

## Scope and privacy

- Counts per brand plus one overall duration. No addresses, network names, locations, per-detection times, or per-device records are kept or shown.
- No GPS, no map output, and no per-detection timestamps, so a walk cannot be reconstructed into where any device is.
- It is meant for understanding your own exposure on your own walks, not for finding or cataloguing particular people's cameras.

## Name

Doorstep: counting what you pass on the way.
