#include "circadian_bot.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "Log.h"
#include "Optional.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "StringFormat.h"

#include <fstream>
#include <vector>

CircadianBot& CircadianBot::Instance()
{
    static CircadianBot instance;
    return instance;
}

void CircadianBot::LoadConfig()
{
    _enabled = sConfigMgr->GetOption<bool>("CircadianBot.Enable", false);
    _logging = sConfigMgr->GetOption<bool>("CircadianBot.Logging", true);
    // Read the playerbots conf key, not sPlayerbotAIConfig.maxRandomBots:
    // ApplyTarget overwrites that field, and this hook may run before playerbots loads.
    _peak = sConfigMgr->GetOption<uint32>("AiPlayerbot.MaxRandomBots", 500);
    _checkInterval = sConfigMgr->GetOption<uint32>("CircadianBot.CheckInterval", 30000);
    _logInterval = sConfigMgr->GetOption<uint32>("CircadianBot.LogInterval", 300000);
    _logoutsPerTick = sConfigMgr->GetOption<uint32>("CircadianBot.LogoutsPerTick", 40);
    _logFile = sConfigMgr->GetOption<std::string>("CircadianBot.LogFile", "CircadianBot.log");

    uint32 const defaults[24] = {
        35, 30, 25, 25, 25, 25,
        35, 40, 45, 48, 50, 55,
        60, 65, 72, 80, 85, 93,
        96, 100, 100, 100, 70, 50
    };

    for (uint32 hour = 0; hour < 24; ++hour)
    {
        std::string const key = Acore::StringFormat("CircadianBot.Hour{:02}", hour);
        uint32 pct = sConfigMgr->GetOption<uint32>(key, defaults[hour]);
        if (pct > 100)
            pct = 100;
        _hourPct[hour] = pct;
    }

    if (_peak < 1)
        _peak = 1;
    if (_checkInterval < 1000)
        _checkInterval = 1000;
    if (_logoutsPerTick < 1)
        _logoutsPerTick = 1;

    _elapsed = _checkInterval;
    _logElapsed = _logInterval;
    _lastApplied = 0;
    _lastLoggedHour = 24;
    _targetReached = false;
}

void CircadianBot::Initialize()
{
    LOG_INFO("server.loading", "Initialize CircadianBot...");

    if (_enabled)
        ApplyTarget(GetTarget());

    LogStatus();

    _targetReached = (GetOnlineRandomBots() == GetTarget());

    std::time_t const now = time(nullptr);
    struct tm local {};
    localtime_r(&now, &local);
    _lastLoggedHour = static_cast<uint32>(local.tm_hour);
    _logElapsed = 0;
}

void CircadianBot::SetEnabled(bool enabled)
{
    _enabled = enabled;
    _elapsed = _checkInterval;
    _logElapsed = _logInterval;
    LOG_INFO("module", "CircadianBot: {}", enabled ? "enabled" : "disabled");
}

void CircadianBot::SetManualTarget(uint32 target)
{
    if (target < 1)
        target = 1;
    _manualTarget = target;
    ApplyTarget(target);
}

void CircadianBot::ClearManualTarget()
{
    _manualTarget = 0;
    _lastApplied = 0;
    _elapsed = _checkInterval;
}

uint32 CircadianBot::HourPercent(std::time_t now) const
{
    struct tm local {};
    localtime_r(&now, &local);
    return _hourPct[static_cast<uint32>(local.tm_hour)];
}

uint32 CircadianBot::GetTarget() const
{
    if (_manualTarget)
        return _manualTarget;

    uint32 target = (_peak * HourPercent(time(nullptr))) / 100;
    if (target < 1)
        target = 1;
    return target;
}

uint32 CircadianBot::GetOnlineRandomBots() const
{
    uint32 count = 0;
    PlayerBotMap const bots = sRandomPlayerbotMgr.GetAllBots();
    for (auto const& [guid, player] : bots)
    {
        if (player && sRandomPlayerbotMgr.IsRandomBot(player))
            ++count;
    }
    return count;
}

void CircadianBot::ApplyTarget(uint32 target)
{
    sPlayerbotAIConfig.minRandomBots = target;
    sPlayerbotAIConfig.maxRandomBots = target;
    sRandomPlayerbotMgr.SetValue(uint32(0), "bot_count", target);
    _lastApplied = target;
}

uint32 CircadianBot::LogoutSurplus(uint32 target, uint32 maxLogouts)
{
    PlayerBotMap bots = sRandomPlayerbotMgr.GetAllBots();
    std::vector<Player*> surplus;
    surplus.reserve(bots.size());

    for (auto const& [guid, player] : bots)
    {
        if (!player || !sRandomPlayerbotMgr.IsRandomBot(player))
            continue;
        if (player->GetGroup() || player->InBattleground() || player->IsInCombat())
            continue;
        if (player->HasUnitState(UNIT_STATE_IN_FLIGHT))
            continue;
        surplus.push_back(player);
    }

    uint32 const online = static_cast<uint32>(surplus.size());
    if (online <= target)
        return 0;

    uint32 toKick = online - target;
    if (toKick > maxLogouts)
        toKick = maxLogouts;

    uint32 kicked = 0;
    for (uint32 i = 0; i < toKick && i < surplus.size(); ++i)
    {
        Player* bot = surplus[i];
        if (!bot)
            continue;
        sRandomPlayerbotMgr.SetValue(bot->GetGUID().GetCounter(), "add", 0);
        sRandomPlayerbotMgr.LogoutPlayerBot(bot->GetGUID());
        ++kicked;
    }

    return kicked;
}

void CircadianBot::LogLine(std::string const& line) const
{
    if (!_logging)
        return;

    LOG_INFO("module", "{}", line);

    if (_logFile.empty())
        return;

    std::time_t const now = time(nullptr);
    struct tm local {};
    localtime_r(&now, &local);

    std::ofstream out(sLog->GetLogsDir() + _logFile, std::ios::app);
    if (!out)
    {
        LOG_ERROR("module", "CircadianBot: could not append to {}", _logFile);
        return;
    }

    char ts[32]{};
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &local);
    out << ts << ' ' << line << '\n';
}

void CircadianBot::CheckTargetReached(uint32 online, uint32 target)
{
    if (online != target)
    {
        _targetReached = false;
        return;
    }

    if (_targetReached)
        return;

    _targetReached = true;
    LogLine("CircadianBot: target reached");
}

void CircadianBot::LogStatus() const
{
    LogLine(Acore::StringFormat("CircadianBot: {} / {}", GetOnlineRandomBots(), GetTarget()));
}

void CircadianBot::Update(uint32 diff)
{
    if (!_enabled)
        return;

    _elapsed += diff;
    _logElapsed += diff;

    if (_elapsed >= _checkInterval)
    {
        _elapsed = 0;

        uint32 const target = GetTarget();
        if (target != _lastApplied)
            ApplyTarget(target);

        uint32 const online = GetOnlineRandomBots();
        if (online > target)
            LogoutSurplus(target, _logoutsPerTick);

        CheckTargetReached(GetOnlineRandomBots(), target);
    }

    std::time_t const now = time(nullptr);
    struct tm local {};
    localtime_r(&now, &local);
    uint32 const hour = static_cast<uint32>(local.tm_hour);
    bool const hourChanged = hour != _lastLoggedHour;
    bool const intervalElapsed = _logging && _logInterval && _logElapsed >= _logInterval;

    if (_logging && (hourChanged || intervalElapsed))
    {
        LogStatus();
        _lastLoggedHour = hour;
        _logElapsed = 0;
    }
}

class CircadianBotWorldScript : public WorldScript
{
public:
    CircadianBotWorldScript() : WorldScript("CircadianBotWorldScript", {
        WORLDHOOK_ON_STARTUP,
        WORLDHOOK_ON_UPDATE,
        WORLDHOOK_ON_AFTER_CONFIG_LOAD
    }) { }

    void OnStartup() override
    {
        sCircadianBot.Initialize();
    }

    void OnAfterConfigLoad(bool reload) override
    {
        sCircadianBot.LoadConfig();
        if (reload)
            sCircadianBot.Initialize();
    }

    void OnUpdate(uint32 diff) override
    {
        sCircadianBot.Update(diff);
    }
};

using namespace Acore::ChatCommands;

class circadian_commandscript : public CommandScript
{
public:
    circadian_commandscript() : CommandScript("circadian_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable circadianTable =
        {
            { "status", HandleStatus, SEC_GAMEMASTER, Console::Yes },
            { "on",     HandleOn,     SEC_GAMEMASTER, Console::Yes },
            { "off",    HandleOff,    SEC_GAMEMASTER, Console::Yes },
            { "target", HandleTarget, SEC_GAMEMASTER, Console::Yes },
            { "auto",   HandleAuto,   SEC_GAMEMASTER, Console::Yes },
        };

        static ChatCommandTable commandTable =
        {
            { "circadian", circadianTable },
        };

        return commandTable;
    }

    static bool HandleStatus(ChatHandler* handler)
    {
        handler->PSendSysMessage("CircadianBot: {} / {}",
            sCircadianBot.GetOnlineRandomBots(),
            sCircadianBot.GetTarget());
        return true;
    }

    static bool HandleOn(ChatHandler* handler)
    {
        sCircadianBot.SetEnabled(true);
        handler->PSendSysMessage("CircadianBot enabled.");
        return true;
    }

    static bool HandleOff(ChatHandler* handler)
    {
        sCircadianBot.SetEnabled(false);
        handler->PSendSysMessage("CircadianBot disabled.");
        return true;
    }

    static bool HandleTarget(ChatHandler* handler, Optional<uint32> count)
    {
        if (!count)
        {
            handler->PSendSysMessage("Usage: circadian target $count");
            return false;
        }

        sCircadianBot.SetManualTarget(*count);
        handler->PSendSysMessage("CircadianBot manual target {} (online {})",
            *count, sCircadianBot.GetOnlineRandomBots());
        return true;
    }

    static bool HandleAuto(ChatHandler* handler)
    {
        sCircadianBot.ClearManualTarget();
        handler->PSendSysMessage("CircadianBot using the hourly schedule. Target {}",
            sCircadianBot.GetTarget());
        return true;
    }
};

void AddSC_circadian_bot()
{
    new CircadianBotWorldScript();
    new circadian_commandscript();
}
