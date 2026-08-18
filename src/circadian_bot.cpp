#include "circadian_bot.h"
#include "AreaDefines.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DBCEnums.h"
#include "DBCStores.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Optional.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "TravelMgr.h"
#include "TravelNode.h"

#include <fstream>
#include <unordered_map>
#include <vector>

namespace
{
struct CityHub
{
    uint32 zoneId = 0;
    TeamId team = TEAM_NEUTRAL;
    uint32 mapId = 0;
    uint32 minLevel = 10;
    char const* name = "";
    uint32 weight = 1;
    std::vector<uint32> hinterlands;
    std::vector<uint32> bankers;
    WorldPosition dest;
    bool resolved = false;
    uint32 peak = 0;
};

// Every major player hub. Stormwind/Orgrimmar are weighted 3x because they
// were the busiest squares, not because they are the only destinations.
std::vector<CityHub> g_cityHubs;

void InitCityHubs()
{
    g_cityHubs = {
        { AREA_STORMWIND_CITY,  TEAM_ALLIANCE, MAP_EASTERN_KINGDOMS, 10, "Stormwind",     3,
          { AREA_ELWYNN_FOREST, AREA_WESTFALL, AREA_REDRIDGE_MOUNTAINS, AREA_DUSKWOOD },
          { 2455, 2456, 2457 }, {}, false },
        { AREA_IRONFORGE,       TEAM_ALLIANCE, MAP_EASTERN_KINGDOMS, 10, "Ironforge",     1,
          { AREA_DUN_MOROGH, AREA_LOCH_MODAN, AREA_WETLANDS },
          { 2460, 2461, 5099 }, {}, false },
        { AREA_UNDERCITY,       TEAM_HORDE,    MAP_EASTERN_KINGDOMS, 10, "Undercity",     1,
          { AREA_TIRISFAL_GLADES, AREA_SILVERPINE_FOREST },
          { 4549, 2459, 2458, 4550 }, {}, false },
        { AREA_ORGRIMMAR,       TEAM_HORDE,    MAP_KALIMDOR,         10, "Orgrimmar",     3,
          { AREA_DUROTAR, AREA_THE_BARRENS },
          { 3320, 3309, 3318 }, {}, false },
        { AREA_THUNDER_BLUFF,   TEAM_HORDE,    MAP_KALIMDOR,         10, "Thunder Bluff", 1,
          { AREA_MULGORE },
          { 2996, 8356, 8357 }, {}, false },
        { AREA_DARNASSUS,       TEAM_ALLIANCE, MAP_KALIMDOR,         10, "Darnassus",     1,
          { AREA_TELDRASSIL },
          { 4155, 4208, 4209 }, {}, false },
        { AREA_THE_EXODAR,      TEAM_ALLIANCE, MAP_OUTLAND,          10, "Exodar",        1,
          { AREA_AZUREMYST_ISLE, AREA_BLOODMYST_ISLE },
          { 17773, 18350, 16710 }, {}, false },
        { AREA_SILVERMOON_CITY, TEAM_HORDE,    MAP_OUTLAND,          10, "Silvermoon",    1,
          { AREA_EVERSONG_WOODS, AREA_GHOSTLANDS },
          { 17631, 17632, 17633, 16615, 16616, 16617 }, {}, false },
        { AREA_SHATTRATH_CITY,  TEAM_NEUTRAL,  MAP_OUTLAND,          58, "Shattrath",     1,
          { AREA_TEROKKAR_FOREST },
          { 19246, 19338, 19034, 19318 }, {}, false },
        { AREA_DALARAN,         TEAM_NEUTRAL,  MAP_NORTHREND,        68, "Dalaran",       1,
          { AREA_CRYSTALSONG_FOREST },
          { 30604, 30605, 30607, 28675, 28676, 28677, 29530 }, {}, false },
    };
}

CityHub const* HubByZone(uint32 zoneId)
{
    for (CityHub const& hub : g_cityHubs)
        if (hub.zoneId == zoneId)
            return &hub;
    return nullptr;
}

CityHub const* HubByHinterland(uint32 zoneId)
{
    for (CityHub const& hub : g_cityHubs)
        for (uint32 hinterland : hub.hinterlands)
            if (hinterland == zoneId)
                return &hub;
    return nullptr;
}

bool ZoneIsCapital(uint32 zoneId)
{
    if (HubByZone(zoneId))
        return true;

    AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zoneId);
    return zone && (zone->flags & AREA_FLAG_CAPITAL);
}

bool BotFitsHub(Player const* bot, CityHub const& hub)
{
    if (bot->GetLevel() < hub.minLevel)
        return false;
    if (hub.team != TEAM_NEUTRAL && hub.team != bot->GetTeamId())
        return false;
    return true;
}

// Soft occupancy: a suggestion, not a quota. Some people leave even when
// the square is thin; some linger when it is already busy.
uint32 HangoutHoldChance(uint32 cityOnline, uint32 cityTarget, bool sessionActive)
{
    uint32 chance = 50;
    if (!cityTarget)
        chance = 15;
    else
    {
        float const ratio = static_cast<float>(cityOnline) / static_cast<float>(cityTarget);
        if (ratio < 0.70f)
            chance = 80;
        else if (ratio < 0.90f)
            chance = 65;
        else if (ratio < 1.15f)
            chance = 50;
        else if (ratio < 1.45f)
            chance = 25;
        else
            chance = 12;
    }

    if (sessionActive && chance < 75)
        chance += 20;
    if (chance > 90)
        chance = 90;
    return chance;
}
} // namespace

CircadianBot& CircadianBot::Instance()
{
    static CircadianBot instance;
    return instance;
}

void CircadianBot::LoadConfig()
{
    _enabled = sConfigMgr->GetOption<bool>("CircadianBot.Enable", true);
    _logging = sConfigMgr->GetOption<bool>("CircadianBot.Logging", true);
    // Read the playerbots conf key, not sPlayerbotAIConfig.maxRandomBots:
    // ApplyTarget overwrites that field, and this hook may run before playerbots loads.
    _peak = sConfigMgr->GetOption<uint32>("AiPlayerbot.MaxRandomBots", 500);
    _checkInterval = sConfigMgr->GetOption<uint32>("CircadianBot.CheckInterval", 30000);
    _logInterval = sConfigMgr->GetOption<uint32>("CircadianBot.LogInterval", 300000);
    _logoutsPerTick = sConfigMgr->GetOption<uint32>("CircadianBot.LogoutsPerTick", 40);
    _logFile = sConfigMgr->GetOption<std::string>("CircadianBot.LogFile", "CircadianBot.log");
    _cityHangout = sConfigMgr->GetOption<bool>("CircadianBot.CityHangout.Enable", true);
    _cityNudgesPerTick = sConfigMgr->GetOption<uint32>("CircadianBot.CityHangout.NudgesPerTick", 3);
    _cityMinLevel = sConfigMgr->GetOption<uint32>("CircadianBot.CityHangout.MinLevel", 10);
    _cityMinSession = sConfigMgr->GetOption<uint32>("CircadianBot.CityHangout.MinSession", 480);
    _cityMaxSession = sConfigMgr->GetOption<uint32>("CircadianBot.CityHangout.MaxSession", 1200);
    _cityMajorHub = sConfigMgr->GetOption<uint32>("CircadianBot.CityHangout.MajorHub", 50);

    // Weekday: work/school trough overnight, slow climb, evening peak 6pm-8pm.
    uint32 const weekdayDefaults[24] = {
        48, 22, 5, 3, 3, 5,
        6, 12, 20, 28, 34, 40,
        48, 52, 58, 66, 80, 92,
        100, 100, 100, 96, 84, 68
    };
    // Weekend (Sat/Sun): later nights, much busier mornings, plateau
    // through afternoon-evening-late. Matches typical MMO/Steam charts.
    uint32 const weekendDefaults[24] = {
        62, 38, 14, 8, 6, 8,
        16, 28, 42, 58, 72, 82,
        90, 94, 96, 98, 100, 100,
        100, 100, 100, 98, 90, 78
    };

    LoadHourTable("CircadianBot.Hour", _hourPct, weekdayDefaults);
    LoadHourTable("CircadianBot.Weekend.Hour", _weekendHourPct, weekendDefaults);

    if (_peak < 1)
        _peak = 1;
    if (_checkInterval < 1000)
        _checkInterval = 1000;
    if (_logoutsPerTick < 1)
        _logoutsPerTick = 1;
    if (_cityNudgesPerTick < 1)
        _cityNudgesPerTick = 1;
    if (_cityMinLevel < 1)
        _cityMinLevel = 1;
    if (_cityMinSession < 30)
        _cityMinSession = 30;
    if (_cityMaxSession < _cityMinSession)
        _cityMaxSession = _cityMinSession;
    if (_cityMajorHub < 1)
        _cityMajorHub = 1;

    _elapsed = _checkInterval;
    _logElapsed = _logInterval;
    _lastApplied = 0;
    _lastLoggedHour = 24;
    _targetReached = false;
}

void CircadianBot::Initialize()
{
    LOG_INFO("server.loading", "Initialize CircadianBot...");

    ResolveCityHubs();

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

void CircadianBot::LoadHourTable(char const* keyPrefix, std::array<uint32, 24>& dest, uint32 const* defaults)
{
    for (uint32 hour = 0; hour < 24; ++hour)
    {
        std::string const key = Acore::StringFormat("{}{:02}", keyPrefix, hour);
        uint32 pct = sConfigMgr->GetOption<uint32>(key, defaults[hour]);
        if (pct > 100)
            pct = 100;
        dest[hour] = pct;
    }
}

bool CircadianBot::IsWeekend(std::time_t now) const
{
    struct tm local {};
    localtime_r(&now, &local);
    return local.tm_wday == 0 || local.tm_wday == 6;
}

char const* CircadianBot::ScheduleName(std::time_t now) const
{
    return IsWeekend(now) ? "weekend" : "weekday";
}

char const* CircadianBot::CurrentScheduleName() const
{
    return ScheduleName(time(nullptr));
}

uint32 CircadianBot::HourPercent(std::time_t now) const
{
    struct tm local {};
    localtime_r(&now, &local);
    uint32 const hour = static_cast<uint32>(local.tm_hour);
    return IsWeekend(now) ? _weekendHourPct[hour] : _hourPct[hour];
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

uint32 CircadianBot::GetCityTarget() const
{
    if (!IsCityHangoutEnabled())
        return 0;

    uint32 target = 0;
    for (CityHub const& hub : g_cityHubs)
        target += DesiredHubCrowd(hub.zoneId);
    return target;
}

void CircadianBot::CountBotsByZone(std::unordered_map<uint32, uint32>& out) const
{
    out.clear();
    PlayerBotMap const bots = sRandomPlayerbotMgr.GetAllBots();
    for (auto const& [guid, player] : bots)
    {
        if (!player || !sRandomPlayerbotMgr.IsRandomBot(player) || !IsInCapital(player))
            continue;
        ++out[player->GetZoneId()];
    }
}

uint32 CircadianBot::DesiredHubCrowd(uint32 zoneId) const
{
    CityHub const* hub = HubByZone(zoneId);
    if (!hub)
        return 0;
    return hub->peak * HourPercent(time(nullptr)) / 100;
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

uint32 CircadianBot::GetOnlineCityBots() const
{
    uint32 count = 0;
    PlayerBotMap const bots = sRandomPlayerbotMgr.GetAllBots();
    for (auto const& [guid, player] : bots)
    {
        if (player && sRandomPlayerbotMgr.IsRandomBot(player) && IsInCapital(player))
            ++count;
    }
    return count;
}

std::string CircadianBot::CityHubSummary() const
{
    std::unordered_map<uint32, uint32> byZone;
    CountBotsByZone(byZone);

    std::string out;
    for (CityHub const& hub : g_cityHubs)
    {
        if (!out.empty())
            out += ", ";
        uint32 count = 0;
        auto it = byZone.find(hub.zoneId);
        if (it != byZone.end())
            count = it->second;
        out += Acore::StringFormat("{} {}/{}", hub.name, count, DesiredHubCrowd(hub.zoneId));
    }
    return out;
}

void CircadianBot::ResolveCityHubs()
{
    InitCityHubs();

    uint32 const major = _cityMajorHub;
    uint32 const mid = major > 1 ? major / 2 : 1;
    uint32 const small = major > 3 ? (major * 3) / 10 : 1;
    for (CityHub& hub : g_cityHubs)
    {
        switch (hub.zoneId)
        {
            case AREA_STORMWIND_CITY:
            case AREA_ORGRIMMAR:
                hub.peak = major;
                break;
            case AREA_IRONFORGE:
            case AREA_UNDERCITY:
            case AREA_SHATTRATH_CITY:
            case AREA_DALARAN:
                hub.peak = mid;
                break;
            default:
                hub.peak = small;
                break;
        }
    }

    std::unordered_map<uint32, WorldPosition> bankerPos;
    for (CityHub const& hub : g_cityHubs)
        for (uint32 entry : hub.bankers)
            bankerPos.emplace(entry, WorldPosition());

    for (auto const& itr : sObjectMgr->GetAllCreatureData())
    {
        auto wanted = bankerPos.find(itr.second.id);
        if (wanted == bankerPos.end() || wanted->second.GetMapId() || wanted->second.GetPositionX() != 0.0f)
            continue;

        wanted->second = WorldPosition(itr.second.mapid, itr.second.posX, itr.second.posY, itr.second.posZ,
                                       itr.second.orientation);
    }

    uint32 resolved = 0;
    for (CityHub& hub : g_cityHubs)
    {
        hub.resolved = false;
        for (uint32 entry : hub.bankers)
        {
            auto it = bankerPos.find(entry);
            if (it == bankerPos.end())
                continue;
            WorldPosition const& pos = it->second;
            if (pos.GetMapId() == 0 && pos.GetPositionX() == 0.0f && pos.GetPositionY() == 0.0f)
                continue;
            hub.dest = pos;
            hub.resolved = true;
            ++resolved;
            break;
        }
        if (!hub.resolved)
            LOG_WARN("module", "CircadianBot: no banker spawn found for {}", hub.name);
    }

    if (_cityHangout)
        LOG_INFO("module", "CircadianBot: city hangout ready ({} / {} hubs)", resolved, g_cityHubs.size());
}

bool CircadianBot::IsInCapital(Player const* bot) const
{
    return bot && ZoneIsCapital(bot->GetZoneId());
}

bool CircadianBot::CanNudge(Player* bot) const
{
    if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported() || !bot->IsAlive())
        return false;
    if (bot->GetLevel() < _cityMinLevel)
        return false;
    if (bot->GetGroup() || bot->InBattleground() || bot->InBattlegroundQueue() || bot->InArena())
        return false;
    if (bot->IsInCombat() || bot->HasUnitState(UNIT_STATE_IN_FLIGHT) || bot->IsInFlight())
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    switch (botAI->rpgInfo.GetStatus())
    {
        case RPG_IDLE:
        case RPG_REST:
        case RPG_WANDER_RANDOM:
        case RPG_WANDER_NPC:
        case RPG_GO_CAMP:
            return true;
        default:
            return false;
    }
}

bool CircadianBot::HangoutActive(uint32 botId) const
{
    auto it = _hangoutUntil.find(botId);
    if (it == _hangoutUntil.end())
        return false;
    return it->second > std::time(nullptr);
}

void CircadianBot::EnsureHangoutSession(Player* bot)
{
    uint32 const botId = bot->GetGUID().GetCounter();
    if (HangoutActive(botId))
        return;

    _hangoutUntil[botId] = std::time(nullptr) + static_cast<std::time_t>(urand(_cityMinSession, _cityMaxSession));
}

void CircadianBot::ApplyCityHangout(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    EnsureHangoutSession(bot);

    uint32 const roll = urand(1, 100);
    if (roll <= 55)
        botAI->rpgInfo.ChangeToWanderNpc();
    else if (roll <= 80)
    {
        botAI->rpgInfo.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
    }
    else
        botAI->rpgInfo.ChangeToWanderRandom();
}

void CircadianBot::PruneHangouts()
{
    if (_hangoutUntil.empty())
        return;

    std::time_t const now = std::time(nullptr);
    PlayerBotMap const bots = sRandomPlayerbotMgr.GetAllBots();
    std::unordered_map<uint32, Player*> online;
    for (auto const& [guid, player] : bots)
    {
        if (player && sRandomPlayerbotMgr.IsRandomBot(player))
            online[guid.GetCounter()] = player;
    }

    for (auto it = _hangoutUntil.begin(); it != _hangoutUntil.end();)
    {
        auto botIt = online.find(it->first);
        if (botIt == online.end() || !IsInCapital(botIt->second) || it->second <= now)
            it = _hangoutUntil.erase(it);
        else
            ++it;
    }
}

void CircadianBot::ExtendCityDwell()
{
    PlayerBotMap const bots = sRandomPlayerbotMgr.GetAllBots();
    std::unordered_map<uint32, uint32> byZone;
    CountBotsByZone(byZone);

    for (auto const& [guid, player] : bots)
    {
        if (!player || !sRandomPlayerbotMgr.IsRandomBot(player) || !IsInCapital(player))
            continue;
        if (player->GetGroup() || player->InBattleground() || player->InBattlegroundQueue() || player->InArena())
            continue;
        if (player->IsInCombat() || player->HasUnitState(UNIT_STATE_IN_FLIGHT) || player->IsInFlight())
            continue;
        if (!player->IsInWorld() || player->IsBeingTeleported() || !player->IsAlive())
            continue;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
        if (!botAI)
            continue;

        NewRpgStatus const status = botAI->rpgInfo.GetStatus();
        if (status == RPG_DO_QUEST || status == RPG_OUTDOOR_PVP)
            continue;

        uint32 const botId = player->GetGUID().GetCounter();
        bool const active = HangoutActive(botId);
        if (status != RPG_IDLE && status != RPG_GO_GRIND && status != RPG_TRAVEL_FLIGHT)
            continue;

        uint32 const desired = DesiredHubCrowd(player->GetZoneId());
        uint32 const have = byZone[player->GetZoneId()];
        if (urand(1, 100) > HangoutHoldChance(have, desired, active))
            continue;

        ApplyCityHangout(player);
    }
}

bool CircadianBot::TryWalkToCapital(Player* bot)
{
    CityHub const* hub = HubByHinterland(bot->GetZoneId());
    if (!hub || !hub->resolved)
        return false;
    if (bot->GetMapId() != hub->mapId)
        return false;
    if (!BotFitsHub(bot, *hub))
        return false;

    uint32 const desired = DesiredHubCrowd(hub->zoneId);
    std::unordered_map<uint32, uint32> byZone;
    CountBotsByZone(byZone);
    uint32 const have = byZone[hub->zoneId];
    if (desired && have >= desired + desired / 4)
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    botAI->rpgInfo.ChangeToGoCamp(hub->dest);
    return true;
}

bool CircadianBot::TryFlyToCapital(Player* bot)
{
    TravelMgr::FlightMasterInfo const* fm = sTravelMgr.GetNearestFlightMasterInfo(bot);
    if (!fm || bot->GetDistance(fm->pos) > 500.0f || !fm->taxiNodeId)
        return false;

    std::unordered_map<uint32, uint32> byZone;
    CountBotsByZone(byZone);

    std::vector<CityHub const*> weighted;
    for (CityHub const& hub : g_cityHubs)
    {
        if (!BotFitsHub(bot, hub))
            continue;
        if (hub.mapId != fm->pos.GetMapId())
            continue;
        if (sTravelMgr.GetFlightNodesInZone(hub.zoneId, bot->GetTeamId(), fm->taxiNodeId).empty())
            continue;

        uint32 const desired = DesiredHubCrowd(hub.zoneId);
        uint32 const have = byZone[hub.zoneId];
        if (desired && have >= desired + desired / 4)
            continue;

        uint32 weight = hub.weight;
        if (desired && have < desired)
            weight *= 3;
        for (uint32 i = 0; i < weight; ++i)
            weighted.push_back(&hub);
    }
    if (weighted.empty())
        return false;

    // Try a few weighted picks so a missing taxi path falls through to another hub.
    for (uint32 attempt = 0; attempt < weighted.size() && attempt < 6; ++attempt)
    {
        CityHub const* hub = weighted[urand(0, static_cast<uint32>(weighted.size()) - 1)];
        std::vector<uint32> nodes = sTravelMgr.GetFlightNodesInZone(hub->zoneId, bot->GetTeamId(), fm->taxiNodeId);
        if (nodes.empty())
            continue;

        uint32 const destNode = nodes[urand(0, static_cast<uint32>(nodes.size()) - 1)];
        std::vector<uint32> path = sTravelNodeMap.FindTaxiPath(fm->taxiNodeId, destNode);
        if (path.empty())
            continue;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return false;

        botAI->rpgInfo.ChangeToTravelFlight(fm->templateEntry, fm->pos, path);
        return true;
    }

    return false;
}

uint32 CircadianBot::NudgeTowardCities(uint32 maxNudges)
{
    PlayerBotMap const bots = sRandomPlayerbotMgr.GetAllBots();
    std::vector<Player*> candidates;
    candidates.reserve(bots.size());

    for (auto const& [guid, player] : bots)
    {
        if (!CanNudge(player) || IsInCapital(player))
            continue;
        candidates.push_back(player);
    }

    for (size_t i = candidates.size(); i > 1; --i)
        std::swap(candidates[i - 1], candidates[urand(0, static_cast<uint32>(i - 1))]);

    uint32 nudged = 0;
    for (Player* bot : candidates)
    {
        if (nudged >= maxNudges)
            break;
        // Gryphon first, then mounted overland from the hinterland.
        // Playerbots may teleport if a long path gets stuck; we do not
        // dump a wave of ports at hour change.
        if (TryFlyToCapital(bot) || TryWalkToCapital(bot))
        {
            EnsureHangoutSession(bot);
            ++nudged;
        }
    }
    return nudged;
}

void CircadianBot::UpdateCityHangout()
{
    PruneHangouts();
    ExtendCityDwell();

    uint32 const cityTarget = GetCityTarget();
    uint32 const cityOnline = GetOnlineCityBots();
    if (!cityTarget)
        return;

    float const ratio = static_cast<float>(cityOnline) / static_cast<float>(cityTarget);
    if (ratio >= 1.25f)
        return;

    uint32 nudges = _cityNudgesPerTick;
    if (ratio >= 1.0f)
        nudges = 1;
    else if (ratio >= 0.85f && nudges > 1)
        nudges = (nudges + 1) / 2;

    NudgeTowardCities(nudges);
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

    if (_cityHangout)
    {
        std::vector<Player*> inCity;
        std::vector<Player*> inField;
        inCity.reserve(surplus.size());
        inField.reserve(surplus.size());
        for (Player* bot : surplus)
        {
            if (IsInCapital(bot))
                inCity.push_back(bot);
            else
                inField.push_back(bot);
        }

        uint32 const cityTarget = GetCityTarget();
        uint32 extraCity = 0;
        if (inCity.size() > cityTarget)
            extraCity = static_cast<uint32>(inCity.size()) - cityTarget;

        surplus.clear();
        surplus.insert(surplus.end(), inCity.begin(), inCity.begin() + extraCity);
        surplus.insert(surplus.end(), inField.begin(), inField.end());
        surplus.insert(surplus.end(), inCity.begin() + extraCity, inCity.end());
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
    if (IsCityHangoutEnabled())
        LogLine(Acore::StringFormat("CircadianBot: {} / {} city {} / {} ({})",
            GetOnlineRandomBots(), GetTarget(), GetOnlineCityBots(), GetCityTarget(),
            CurrentScheduleName()));
    else
        LogLine(Acore::StringFormat("CircadianBot: {} / {} ({})",
            GetOnlineRandomBots(), GetTarget(), CurrentScheduleName()));
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

        if (IsCityHangoutEnabled())
            UpdateCityHangout();

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
        handler->PSendSysMessage("CircadianBot: {} / {} ({})",
            sCircadianBot.GetOnlineRandomBots(),
            sCircadianBot.GetTarget(),
            sCircadianBot.CurrentScheduleName());
        if (sCircadianBot.IsCityHangoutEnabled())
        {
            handler->PSendSysMessage("CircadianBot city: {} / {}",
                sCircadianBot.GetOnlineCityBots(),
                sCircadianBot.GetCityTarget());
            handler->PSendSysMessage("{}", sCircadianBot.CityHubSummary());
        }
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
