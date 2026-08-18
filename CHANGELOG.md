# Changelog

Notable changes to [mod-circadian-bots](https://github.com/muntedpissmole/mod-circadian-bots).

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

## [2026-08-18]

### Added

- Startup seed: once most random bots are online, teleport to snap each capital to the current hourly crowd. Extras from overfull hubs (Dalaran) fill underfilled ones (Stormwind). `CircadianBot.CityHangout.SeedOnStart` (default on).
- Hourly targets blend toward the next hour through the 60 minutes, so realm pop and city hangouts ramp during the hour instead of jumping at `:00`.
- Inbound walks scale with how far a hub is behind the curve (up to 8× `NudgesPerTick`).
- Surplus city bots are sent out to grind even when the realm is at cap. Playerbots will keep walking extras into capitals; this module walks them back out.

### Changed

- Conf file is `mod_circadian_bots.conf` (plural, matches the module name).
- Hangouts cluster on the auction-house floor, with the fountain / mailbox square as overflow — not the whole city and not the teller desks.
- `CircadianBot.CityHangout.MajorHub` default is 100 (Stormwind / Orgrimmar at 100% hour). Mid hubs get half; smaller capitals get 30%.
- `.circadian status` and the log print per-hub `have/want (N square)`.
