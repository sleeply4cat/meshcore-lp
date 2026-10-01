# Changes to the MeshCore sources

`src/meshcore/` and `src/repeater/` are copies of MeshCore `v1.17.1` (`src/`, `src/helpers/` and
`examples/simple_repeater/`). Protocol behaviour is unchanged; every edit is marked `[LP]` in the code
(`grep -rn '\[LP\]' src/`). To move to a newer MeshCore, copy the new upstream files over and re-apply
the marked edits.

| File | Change | Why |
|---|---|---|
| `Dispatcher.h/.cpp` | `PacketManager::getNextOutboundTime/InboundTime()`, `Dispatcher::getNextWakeMillis()` | the main loop sleeps until the next timed job instead of polling |
| `Dispatcher.h/.cpp` | `PacketManager::peekNextOutbound()`, `Radio::setNextTxUrgent()`; checkSend marks direct-routed packets urgent | replies are not held back by CAD |
| `Mesh.cpp` | `getCADFailRetryDelay()` random 120..360 ms (was 120/240/360) | nodes deferring on the same busy channel do not retry in lock-step (from upstream PR #2916) |
| `helpers/StaticPoolPacketManager.*` | `PacketQueue::earliest()`, `PacketQueue::peek()` + overrides | the two items above |
| `repeater/MyMesh.cpp` | packet log (`log start`) goes to the USB console instead of a flash file | flash logging erases a page per packet |
| `repeater/MyMesh.cpp` | `lp …`, `set lp.*`, `get cad`, `start ota …` command handlers, `/lp_prefs` file | low-power receiver settings and diagnostics |
| `repeater/MyMesh.cpp` | defaults: `LP_QUIET_DEFAULTS` (repeat off, adverts off), CAD on, AGC reset 8 s, optional `LP_DEFAULT_PATH_HASH_MODE` | quiet new nodes, channel access |
| `repeater/MyMesh.cpp` | `getNextWakeMillis()` adds advert / radio-revert / ACL-save timers | sleep scheduling |
| `repeater/main.cpp` | rewritten main loop (sleep), hardware RNG for the identity, boot advert only when adverts are enabled | bare metal |

Not taken from MeshCore: RadioLib wrappers (replaced by `repeater/LoRaRadio.*` + `hal/sx1262.c`),
`NRF52Board`/`PromicroBoard` (replaced by `repeater/target.*`), Arduino core, Adafruit LittleFS
(replaced by `compat/flashfs.cpp`), Bluefruit/SoftDevice, sensors, display, bridges.
