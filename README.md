# ![logo](https://raw.githubusercontent.com/azerothcore/azerothcore.github.io/master/images/logo-github.png) AzerothCore

## mod-circadian-bots

A module for the [Playerbots AzerothCore fork](https://github.com/mod-playerbots/azerothcore-wotlk/tree/Playerbot) and [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots).

## Description

This module makes your realm feel alive at the busy times of day instead of having a constant flat bot count. Back in the glory days, a realm's population was quiet before dawn, filled through the afternoon, peaked in the evening, and went quiet again after midnight — this module reproduces that so a player logging in at 3am finds a quiet, sleepy realm and a player logging in at 8pm sees a busy server.

It works by logging bots in or out to hit an hourly target you define as a percentage of your max bot count — for example, 30% online at 9am and 100% online at 7pm. You set one schedule for weekdays and a busier one for weekends, and the module gradually brings the population up or down to suit. During peak hours bots gather in major player hubs like Stormwind and Orgrimmar so those cities feel busy the way they used to. If you have bots in your guild this adds to the realism by varying the "players online" count.

When lowering the population cap a bot that's grouped, in combat, flying, or in a battleground does not get logged out.

## Requirements

- [Playerbots AzerothCore fork](https://github.com/mod-playerbots/azerothcore-wotlk/tree/Playerbot)
- [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots)

## Installation

1. Place the module under the `modules` folder of your Playerbots AzerothCore source.
2. Compile and rebuild:
   ```sh
   ./acore.sh compiler all
   ```
## Playerbots configuration

Set `AiPlayerbot.MaxRandomBots` in `playerbots.conf` to the peak bot population for your realm. This module treats that value as 100% for the hourly schedule.

Playerbots keeps bots logged in for up to a year, so they will not log out when the cap lowers unless you change the following in `playerbots.conf`:

```ini
AiPlayerbot.EnablePeriodicOnlineOffline = 1
AiPlayerbot.MinRandomBotInWorldTime = 7200
AiPlayerbot.MaxRandomBotInWorldTime = 14400
```

| Option | Function |
| --- | --- |
| `EnablePeriodicOnlineOffline` | If `0`, Playerbots tells bots to stay for a year. This module still logs surplus bots out when the cap drops. |
| `MinRandomBotInWorldTime` | Soonest a bot may log out on its own, in seconds. `3600` = 1 hour. |
| `MaxRandomBotInWorldTime` | Latest a bot may log out on its own, in seconds. `14400` = 4 hours. |

Also leave `AiPlayerbot.PeriodicOnlineOfflineRatio` at `2.0` or higher (the default) so there are enough offline bots to rotate in.

## Module configuration

Customise to suit your hardware.

`playerbots.conf`:

```ini
AiPlayerbot.MaxRandomBots = 500
```

`mod_circadian_bot.conf`:

```ini
CircadianBot.Enable = 1
CircadianBot.CheckInterval = 30000
CircadianBot.LogoutsPerTick = 40
CircadianBot.Logging = 1
CircadianBot.LogInterval = 300000
CircadianBot.LogFile = "CircadianBot.log"
CircadianBot.CityHangout.Enable = 1
```

### Logging

Enable logging to see how many bots are currently online vs. the target to help you fine tune your levels and confirm everything is working correctly.

Set `Logging = 1` to turn it on, and `LogInterval` to control how often it writes (in milliseconds — the default is every 5 minutes). Each entry reports:

| Value | Meaning |
| --- | --- |
| `target` | How many bots the schedule wants online |
| `online` | How many bots are currently online |
| `world` | Everyone online — bots plus real players |

Entries go to `Server.log`. If you set `LogFile` they're saved to their own file as well (`CircadianBot.log`).

### Hourly population schedule

`Hour00`–`Hour23` are a percentage of `AiPlayerbot.MaxRandomBots` (0–100) on weekdays. Defaults follow a work/school day: quiet overnight, a slow climb throughout the morning and day, and an evening peak from 6pm to 9pm. Weekends stay busier in the morning and sit near peak from midday through late night, following usual MMO patterns.

| Hour | Weekday | Weekend |
| --- | --- | --- |
| 9am | 28% | 58% |
| 12pm | 48% | 90% |
| 4pm | 80% | 100% |
| 7pm | 100% | 100% |
| 11pm | 68% | 78% |

### City hangouts

While hangouts are enabled (default), a set number of bots hang around the capitals. A fixed number, not percentage.

Set `CircadianBot.CityHangout.Enable = 0` to turn this off.

| Faction | Hubs |
| --- | --- |
| Alliance | Stormwind, Ironforge, Darnassus, Exodar |
| Horde | Orgrimmar, Undercity, Thunder Bluff, Silvermoon |
| Neutral | Shattrath (level 58+), Dalaran (level 68+) |

Bots take a gryphon when a flight master is nearby; otherwise they ride. Playerbots may teleport a bot if it's stuck.

`MajorHub` (default 50) is the Stormwind / Orgrimmar crowd when the hourly schedule is at 100%. It is a fixed cap, not a percent of `MaxRandomBots`, so you can configure the crowd size to your hardware and tastes. `MajorHub` scales with the population table, for example using a value of 50, at 9am on a weekday (28%) is about 14 in Stormwind. Raise it to fill the cities, or lower it to leave more people out questing. Ironforge, Undercity, Shattrath, and Dalaran get half of `MajorHub`. Darnassus, Thunder Bluff, Exodar, and Silvermoon get 30%.

| Hub | 9am weekday | 7pm (100%) |
| --- | --- | --- |
| Stormwind, Orgrimmar | 14 | 50 |
| Ironforge, Undercity, Shattrath, Dalaran | 7 | 25 |
| Darnassus, Thunder Bluff, Exodar, Silvermoon | 4 | 15 |

`.circadian status` prints the city total and a per-hub breakdown (`Stormwind 48/50`). When surplus logouts are needed, extra city bots go first so the hubs empty as the realm quiets down.

## Commands

Available in-game (GM) or from the worldserver console:

```
.circadian status
.circadian on
.circadian off
.circadian target 400
.circadian auto
```

`target` holds a fixed cap until `auto` returns the module to the hourly schedule. `on`/`off` only toggle this module — they do not change `playerbots.conf`.

## Credits

- AzerothCore (Playerbots fork): [repository](https://github.com/mod-playerbots/azerothcore-wotlk)
- Playerbots: [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots)

## License

Released under the [GNU Affero General Public License v3.0](LICENSE).
