# ![logo](https://raw.githubusercontent.com/azerothcore/azerothcore.github.io/master/images/logo-github.png) AzerothCore

# mod-circadian-bot

A module for the [Playerbots AzerothCore fork](https://github.com/mod-playerbots/azerothcore-wotlk/tree/Playerbot) and [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots).

Back in the glory days a realm's population was generally quiet before dawn, filled through the afternoon, peaked in the evening and went quiet again after midnight. This module makes the bot count follow that pattern.

The mod tracks the server's local hour and logs in or out bots to maintain the percentage table in the config file. The world is busier around peak times (afternoon and evening) and quieter during late/early hours.

Hourly percents are configurable. Defaults assume an evening-peak realm.

### How to install

1. Place the module under the `modules` folder of your Playerbots AzerothCore source.
2. Recompile `./acore.sh compiler all`.
3. enable the addon in the config file.

### Playerbots settings

Set `AiPlayerbot.MaxRandomBots` in `playerbots.conf` to the evening peak you want. This module uses that value as 100% for the hourly schedule.

Playerbots keeps bots logged in for up to a year so they will not logout when the cap lowers unless you change this in `playerbots.conf`:

```
AiPlayerbot.EnablePeriodicOnlineOffline = 1
AiPlayerbot.MinRandomBotInWorldTime = 7200
AiPlayerbot.MaxRandomBotInWorldTime = 14400
```

| Option | Function |
| --- | --- |
| `EnablePeriodicOnlineOffline` | If `0`, playerbots tells bots to stay for a year. This module still logs surplus out when the cap drops. |
| `MinRandomBotInWorldTime` | Soonest a bot may log out on its own (seconds). `3600` = 1 hour. |
| `MaxRandomBotInWorldTime` | Latest a bot may log out on its own (seconds). `14400` = 4 hours. |

Also leave `AiPlayerbot.PeriodicOnlineOfflineRatio` at least `2.0` (the default) so there are enough offline bots to rotate in.

### Module configuration

Customise to suit your hardware:

playerbots.conf:
```
AiPlayerbot.MaxRandomBots = 4000
```

mod_circadian_bot.conf:
```
CircadianBot.Enable = 1
CircadianBot.CheckInterval = 30000
CircadianBot.LogoutsPerTick = 40
CircadianBot.Logging = 1
CircadianBot.LogInterval = 300000
CircadianBot.LogFile = "CircadianBot.log"
```

`Logging = 1` writes a `target` vs `online` snapshot every `LogInterval` and at each local hour change, plus `world` (all online characters). Lines go to Server.log and, if `LogFile` is set, an append-only file in `LogsDir` (`CircadianBot.log` next to the worldserver binary by default). `.circadian status` still prints in the console when logging is off.

`Hour00`–`Hour23` are a percent of `AiPlayerbot.MaxRandomBots` (0–100). Defaults peak in the evening (19:00–21:00 at 100%). A target of `0` is not used; the floor is 1 bot.

Bots in a group, combat, flight, or a battleground are not logged out. Surplus logouts are capped at `LogoutsPerTick` every `CheckInterval` (default 40 every 30 seconds).

### Commands

In-game (GM) or the worldserver console:

```
.circadian status
.circadian on
.circadian off
.circadian target 2000
.circadian auto
```

`target` holds a fixed cap until `auto` returns to the hourly schedule. `on` / `off` only toggle this module; they do not change `playerbots.conf`.

### License

GNU Affero General Public License v3.0. See [LICENSE](LICENSE).

### Credits

- AzerothCore (Playerbots fork): [repository](https://github.com/mod-playerbots/azerothcore-wotlk)
- Playerbots: [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) - [discord](https://discord.gg/NQm5QShwf9)
