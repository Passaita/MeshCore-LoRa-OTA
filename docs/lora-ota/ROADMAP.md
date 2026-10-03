# LoRa-OTA - roadmap and known issues

## Before the v1.17.28 release
* Finish the rc6 soak; act on the private review's findings (possibly another release candidate).
* **C15** - a trailing space breaks `fw` commands (`fw pull-status ` -> `ERR - unknown peer`; phone keyboards add it). Fix: trim
  the argument before dispatch.
* **C16** - text fixes: `fw duty` help (1.0 is ~30 % of the channel, 0.3 ~9 %); `fw bl` says "no valid identity descriptor" when the
  real reason is "no passphrase"; `fw pull-status` shows `0.0c/s` at LoRa speeds (show chunks/min); the serving node's status keeps
  the highest chunk of an earlier pull instead of the current puller's position.

## Next feature: the circle as a swarm (designed, not built)
Pull a **version**, not a host:
* **Phase 1 - image list:** `fw peers` lists image versions available in the circle (newest first, `fw-290`, `bl-rev-7`, ...);
  `fw pull fw-290` picks the best holder: fewest hops / strongest signal, ranked by expected rate = rate / (active pulls + 1) - a
  holder already serving another puller has half the speed to offer.
* **Phase 2 - automatic failover:** the pull keeps up to 4 holders of the same image and switches when its holder stays silent for
  about 6 missed windows (scaled to the measured speed and duty, never below 2 min), keeping its chunks. Re-discovery asks "who has
  image X". A node that finishes a pull announces itself once on the circle channel.
* **Phase 2b - fleet fill:** chunks encrypted with an image key derived from the circle key, so every circle node on the path can keep
  the chunks it relays or overhears; one pull to the far end of a chain fills every node with an **empty** slot along the way (~65 %
  less transfer traffic for a 4-node chain). Any circle member may start it; nothing is installed automatically (`fw apply` per node).
* Several holders at once / partial seeding: dropped (one shared LoRa channel - little gain, much complexity).

## Later
* **External SPI flash (RAK15001, 2 MB)** as extra storage: first a rollback copy of the previous app (the bootloader restores it if a
  new app does not come up - the biggest safety gain for remote nodes), then more image slots. Every function must keep working
  without the module.
* Other nRF52840 boards (RAK4631 class).

## Decided against
* **Signed images / signature check in the bootloader** - nodes must always be able to return to stock firmware.
* **Persisted route memory** across pulls - routes go stale between rare updates, and it costs flash writes.
