#include "circadian_bot.h"
#include "AreaDefines.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DBCEnums.h"
#include "DBCStores.h"
#include "Log.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Optional.h"
#include "Player.h"
#include "UnitDefines.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "TravelMgr.h"
#include "TravelNode.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <unordered_map>
#include <variant>
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
    std::vector<WorldPosition> hangouts;
    WorldPosition dest;
    bool resolved = false;
    uint32 peak = 0;
};

// Most dests are on the auction-house floor (the room, a few yards off
// the auctioneers). The plaza / mailboxes stay in the pool so the
// fountain is not empty. WanderNpc walks 150 yards to any trainer.
float const HANGOUT_RADIUS = 55.0f;
float const HANGOUT_CLUSTER = 150.0f;
float const HANGOUT_MAIL_RADIUS = 80.0f;
float const HANGOUT_DOOR_OFFSET = 18.0f;

// Every major player hub. Stormwind/Orgrimmar are weighted 3x because they
// were the busiest squares, not because they are the only destinations.
std::vector<CityHub> g_cityHubs;

void InitCityHubs()
{
    g_cityHubs = {
        { AREA_STORMWIND_CITY,  TEAM_ALLIANCE, MAP_EASTERN_KINGDOMS, 10, "Stormwind",     3,
          { AREA_ELWYNN_FOREST, AREA_WESTFALL, AREA_REDRIDGE_MOUNTAINS, AREA_DUSKWOOD },
          { 2455, 2456, 2457 }, {}, {}, false },
        { AREA_IRONFORGE,       TEAM_ALLIANCE, MAP_EASTERN_KINGDOMS, 10, "Ironforge",     1,
          { AREA_DUN_MOROGH, AREA_LOCH_MODAN, AREA_WETLANDS },
          { 2460, 2461, 5099 }, {}, {}, false },
        { AREA_UNDERCITY,       TEAM_HORDE,    MAP_EASTERN_KINGDOMS, 10, "Undercity",     1,
          { AREA_TIRISFAL_GLADES, AREA_SILVERPINE_FOREST },
          { 4549, 2459, 2458, 4550 }, {}, {}, false },
        { AREA_ORGRIMMAR,       TEAM_HORDE,    MAP_KALIMDOR,         10, "Orgrimmar",     3,
          { AREA_DUROTAR, AREA_THE_BARRENS },
          { 3320, 3309, 3318 }, {}, {}, false },
        { AREA_THUNDER_BLUFF,   TEAM_HORDE,    MAP_KALIMDOR,         10, "Thunder Bluff", 1,
          { AREA_MULGORE },
          { 2996, 8356, 8357 }, {}, {}, false },
        { AREA_DARNASSUS,       TEAM_ALLIANCE, MAP_KALIMDOR,         10, "Darnassus",     1,
          { AREA_TELDRASSIL },
          { 4155, 4208, 4209 }, {}, {}, false },
        { AREA_THE_EXODAR,      TEAM_ALLIANCE, MAP_OUTLAND,          10, "Exodar",        1,
          { AREA_AZUREMYST_ISLE, AREA_BLOODMYST_ISLE },
          { 17773, 18350, 16710 }, {}, {}, false },
        { AREA_SILVERMOON_CITY, TEAM_HORDE,    MAP_OUTLAND,          10, "Silvermoon",    1,
          { AREA_EVERSONG_WOODS, AREA_GHOSTLANDS },
          { 17631, 17632, 17633, 16615, 16616, 16617 }, {}, {}, false },
        { AREA_SHATTRATH_CITY,  TEAM_NEUTRAL,  MAP_OUTLAND,          58, "Shattrath",     1,
          { AREA_TEROKKAR_FOREST },
          { 19246, 19338, 19034, 19318 }, {}, {}, false },
        { AREA_DALARAN,         TEAM_NEUTRAL,  MAP_NORTHREND,        68, "Dalaran",       1,
          { AREA_CRYSTALSONG_FOREST },
          { 30604, 30605, 30607, 28675, 28676, 28677, 29530 }, {}, {}, false },
    };
}

CityHub* FindHub(uint32 zoneId)
{
    for (CityHub& hub : g_cityHubs)
        if (hub.zoneId == zoneId)
            return &hub;
    return nullptr;
}

CityHub const* HubByZone(uint32 zoneId)
{
    return FindHub(zoneId);
}

WorldPosition const* PickHangout(CityHub const& hub)
{
    if (!hub.hangouts.empty())
        return &hub.hangouts[urand(0, static_cast<uint32>(hub.hangouts.size()) - 1)];
    if (hub.resolved)
        return &hub.dest;
    return nullptr;
}

float DistToHangout(Player const* bot, CityHub const& hub)
{
    float best = 99999.0f;
    auto consider = [&](WorldPosition const& pos)
    {
        if (pos.GetMapId() != bot->GetMapId())
            return;
        float const d = bot->GetExactDist(pos);
        if (d < best)
            best = d;
    };

    if (!hub.hangouts.empty())
    {
        for (WorldPosition const& pos : hub.hangouts)
            consider(pos);
    }
    else if (hub.resolved)
        consider(hub.dest);

    return best;
}

bool OnHangoutSquare(Player const* bot, CityHub const& hub)
{
    return DistToHangout(bot, hub) <= HANGOUT_RADIUS;
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

WorldPosition PickWorldGrind(Player* bot)
{
    std::vector<WorldLocation> const& locs = sTravelMgr.GetLocsPerLevelCache(bot->GetLevel());
    std::vector<WorldPosition> near;
    std::vector<WorldPosition> any;
    near.reserve(32);
    any.reserve(32);

    for (WorldLocation const& loc : locs)
    {
        if (loc.GetMapId() != bot->GetMapId())
            continue;

        uint32 const zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, loc.GetMapId(),
            loc.GetPositionX(), loc.GetPositionY(), loc.GetPositionZ());
        if (ZoneIsCapital(zoneId))
            continue;

        WorldPosition const pos(loc);
        any.push_back(pos);
        float const dist = bot->GetExactDist(loc);
        if (dist >= 200.0f && dist <= 2500.0f)
            near.push_back(pos);
    }

    std::vector<WorldPosition> const& pool = !near.empty() ? near : any;
    if (pool.empty())
        return {};
    return pool[urand(0, static_cast<uint32>(pool.size()) - 1)];
}

void ShufflePlayers(std::vector<Player*>& list)
{
    for (size_t i = list.size(); i > 1; --i)
        std::swap(list[i - 1], list[urand(0, static_cast<uint32>(i - 1))]);
}

bool EligibleForSeed(Player* bot)
{
    if (!bot || !sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;
    if (!bot->IsInWorld() || bot->IsBeingTeleported() || !bot->IsAlive())
        return false;
    if (bot->GetGroup() || bot->InBattleground() || bot->InBattlegroundQueue() || bot->InArena())
        return false;
    if (bot->IsInCombat() || bot->HasUnitState(UNIT_STATE_IN_FLIGHT) || bot->IsInFlight())
        return false;
    if (bot->GetMap() && (bot->GetMap()->IsDungeon() || bot->GetMap()->IsRaid() || bot->GetMap()->IsBattlegroundOrArena()))
        return false;
    return GET_PLAYERBOT_AI(bot) != nullptr;
}

WorldPosition HangoutCentroid(std::vector<WorldPosition> const& pts)
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
    for (WorldPosition const& p : pts)
    {
        x += p.GetPositionX();
        y += p.GetPositionY();
        z += p.GetPositionZ();
    }
    float const n = static_cast<float>(pts.size());
    return WorldPosition(pts.front().GetMapId(), x / n, y / n, z / n, 0.0f);
}

WorldPosition TowardPlaza(WorldPosition const& from, WorldPosition const& plaza, float step)
{
    float const dx = plaza.GetPositionX() - from.GetPositionX();
    float const dy = plaza.GetPositionY() - from.GetPositionY();
    float const len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0f)
        return from;
    float used = step;
    if (used > len * 0.6f)
        used = len * 0.6f;
    float const s = used / len;
    return WorldPosition(from.GetMapId(),
        from.GetPositionX() + dx * s,
        from.GetPositionY() + dy * s,
        from.GetPositionZ(),
        from.GetOrientation());
}

WorldPosition SnapToGround(WorldPosition pos)
{
    Map const* map = sMapMgr->CreateBaseMap(pos.GetMapId());
    if (!map)
        return pos;
    float const z = map->GetHeight(PHASEMASK_NORMAL, pos.GetPositionX(), pos.GetPositionY(),
        pos.GetPositionZ() + 5.0f, true);
    if (z > INVALID_HEIGHT)
        pos.setZ(z);
    return pos;
}

void AddHangout(std::vector<WorldPosition>& dests, WorldPosition const& pos, uint32 copies)
{
    WorldPosition const snapped = SnapToGround(pos);
    for (uint32 i = 0; i < copies; ++i)
        dests.push_back(snapped);
}

void BuildPlazaHangouts(CityHub& hub, std::vector<WorldPosition> const& cluster,
    std::vector<WorldPosition> const& mailboxes, std::vector<WorldPosition> const& auctioneers)
{
    hub.hangouts.clear();
    if (cluster.empty())
        return;

    WorldPosition const plaza = SnapToGround(HangoutCentroid(cluster));

    // AH hall: step off the auctioneer toward the door so they fill the
    // room. Repeated so PickHangout lands here more often than the fountain.
    if (!auctioneers.empty())
    {
        WorldPosition ahCenter = HangoutCentroid(auctioneers);
        ahCenter = SnapToGround(TowardPlaza(ahCenter, plaza, 10.0f));
        AddHangout(hub.hangouts, ahCenter, 4);
        float const ahRing[4][2] = { { 4.0f, 0.0f }, { -4.0f, 0.0f }, { 0.0f, 4.0f }, { 0.0f, -4.0f } };
        for (auto const& d : ahRing)
        {
            AddHangout(hub.hangouts, WorldPosition(ahCenter.GetMapId(),
                ahCenter.GetPositionX() + d[0], ahCenter.GetPositionY() + d[1],
                ahCenter.GetPositionZ(), 0.0f), 3);
        }
        for (WorldPosition const& auctioneer : auctioneers)
            AddHangout(hub.hangouts, TowardPlaza(auctioneer, plaza, 10.0f), 3);
    }

    AddHangout(hub.hangouts, plaza, 2);
    float const ring[6][2] = {
        { 8.0f, 0.0f }, { -8.0f, 0.0f }, { 0.0f, 8.0f }, { 0.0f, -8.0f },
        { 6.0f, 6.0f }, { -6.0f, 6.0f }
    };
    for (auto const& d : ring)
    {
        hub.hangouts.push_back(SnapToGround(WorldPosition(plaza.GetMapId(),
            plaza.GetPositionX() + d[0], plaza.GetPositionY() + d[1], plaza.GetPositionZ(), 0.0f)));
    }

    for (WorldPosition const& box : mailboxes)
    {
        if (box.GetMapId() == plaza.GetMapId() && box.GetExactDist(plaza) <= HANGOUT_MAIL_RADIUS)
            hub.hangouts.push_back(box);
    }
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
    _cityMajorHub = sConfigMgr->GetOption<uint32>("CircadianBot.CityHangout.MajorHub", 100);
    _seedOnStart = sConfigMgr->GetOption<bool>("CircadianBot.CityHangout.SeedOnStart", true);

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
    uint32 const nextHour = (hour + 1) % 24;
    uint32 const minute = static_cast<uint32>(local.tm_min);

    // Blend toward the next hour so walkers leave during the ramp,
    // not at the hour change. Sunday 23:xx → Monday uses the weekday table.
    std::time_t nextTime = now - static_cast<std::time_t>(local.tm_min) * 60
        - static_cast<std::time_t>(local.tm_sec) + 3600;
    uint32 const cur = IsWeekend(now) ? _weekendHourPct[hour] : _hourPct[hour];
    uint32 const nxt = IsWeekend(nextTime) ? _weekendHourPct[nextHour] : _hourPct[nextHour];
    return (cur * (60 - minute) + nxt * minute) / 60;
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
    std::unordered_map<uint32, uint32> onSquare;
    CountBotsByZone(byZone);

    PlayerBotMap const bots = sRandomPlayerbotMgr.GetAllBots();
    for (auto const& [guid, player] : bots)
    {
        if (!player || !sRandomPlayerbotMgr.IsRandomBot(player))
            continue;
        CityHub const* hub = HubByZone(player->GetZoneId());
        if (hub && OnHangoutSquare(player, *hub))
            ++onSquare[hub->zoneId];
    }

    std::string out;
    for (CityHub const& hub : g_cityHubs)
    {
        if (!out.empty())
            out += ", ";
        uint32 count = 0;
        auto it = byZone.find(hub.zoneId);
        if (it != byZone.end())
            count = it->second;
        uint32 square = 0;
        auto sit = onSquare.find(hub.zoneId);
        if (sit != onSquare.end())
            square = sit->second;
        out += Acore::StringFormat("{} {}/{} ({} square)", hub.name, count, DesiredHubCrowd(hub.zoneId), square);
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

    std::unordered_map<uint32, std::vector<WorldPosition>> banks;
    std::unordered_map<uint32, std::vector<WorldPosition>> auctioneers;
    std::unordered_map<uint32, std::vector<WorldPosition>> mailboxes;
    std::unordered_map<uint32, WorldPosition> fallbackBanker;

    for (CityHub const& hub : g_cityHubs)
        for (uint32 entry : hub.bankers)
            fallbackBanker.emplace(entry, WorldPosition());

    for (auto const& itr : sObjectMgr->GetAllCreatureData())
    {
        CreatureData const& data = itr.second;
        WorldPosition const pos(data.mapid, data.posX, data.posY, data.posZ, data.orientation);

        auto fallback = fallbackBanker.find(data.id);
        if (fallback != fallbackBanker.end() && fallback->second.GetMapId() == 0 && fallback->second.GetPositionX() == 0.0f)
            fallback->second = pos;

        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(data.id);
        if (!tmpl)
            continue;

        bool const isBank = (tmpl->npcflag & UNIT_NPC_FLAG_BANKER) != 0;
        bool const isAH = (tmpl->npcflag & UNIT_NPC_FLAG_AUCTIONEER) != 0;
        if (!isBank && !isAH)
            continue;

        uint32 const zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, data.mapid, data.posX, data.posY, data.posZ);
        CityHub* hub = FindHub(zoneId);
        if (!hub || hub->mapId != data.mapid)
            continue;

        if (isAH)
            auctioneers[hub->zoneId].push_back(pos);
        if (isBank)
            banks[hub->zoneId].push_back(pos);
    }

    for (auto const& itr : sObjectMgr->GetAllGOData())
    {
        GameObjectData const& data = itr.second;
        GameObjectTemplate const* tmpl = sObjectMgr->GetGameObjectTemplate(data.id);
        if (!tmpl || tmpl->type != GAMEOBJECT_TYPE_MAILBOX)
            continue;

        uint32 const zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, data.mapid, data.posX, data.posY, data.posZ);
        CityHub* hub = FindHub(zoneId);
        if (!hub || hub->mapId != data.mapid)
            continue;

        mailboxes[hub->zoneId].push_back(WorldPosition(data.mapid, data.posX, data.posY, data.posZ, data.orientation));
    }

    uint32 resolved = 0;
    uint32 spots = 0;
    for (CityHub& hub : g_cityHubs)
    {
        hub.hangouts.clear();
        hub.resolved = false;

        std::vector<WorldPosition> cluster;
        std::vector<WorldPosition> const& ah = auctioneers[hub.zoneId];
        std::vector<WorldPosition> const& bank = banks[hub.zoneId];
        if (!ah.empty())
        {
            cluster = ah;
            for (WorldPosition const& b : bank)
            {
                for (WorldPosition const& a : ah)
                {
                    if (b.GetExactDist(a) <= HANGOUT_CLUSTER)
                    {
                        cluster.push_back(b);
                        break;
                    }
                }
            }
        }
        else if (!bank.empty())
            cluster = bank;

        if (cluster.empty())
        {
            for (uint32 entry : hub.bankers)
            {
                auto it = fallbackBanker.find(entry);
                if (it == fallbackBanker.end())
                    continue;
                WorldPosition const& pos = it->second;
                if (pos.GetMapId() == 0 && pos.GetPositionX() == 0.0f && pos.GetPositionY() == 0.0f)
                    continue;
                cluster.push_back(pos);
                break;
            }
        }

        BuildPlazaHangouts(hub, cluster, mailboxes[hub.zoneId], auctioneers[hub.zoneId]);

        if (!hub.hangouts.empty())
        {
            hub.dest = hub.hangouts[0];
            hub.resolved = true;
            ++resolved;
            spots += static_cast<uint32>(hub.hangouts.size());
        }
        else
            LOG_WARN("module", "CircadianBot: no bank/AH spawn found for {}", hub.name);
    }

    if (_cityHangout)
        LOG_INFO("module", "CircadianBot: city hangout ready ({} / {} hubs, {} plaza spots)",
                 resolved, g_cityHubs.size(), spots);
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

    CityHub const* hub = HubByZone(bot->GetZoneId());
    if (hub && hub->resolved && !OnHangoutSquare(bot, *hub))
    {
        if (WorldPosition const* dest = PickHangout(*hub))
        {
            botAI->rpgInfo.ChangeToGoCamp(*dest);
            return;
        }
    }

    // Stay on the square. WanderNpc walks to trainers across the city.
    uint32 const roll = urand(1, 100);
    if (roll <= 40)
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
        if (status == RPG_TRAVEL_FLIGHT || status == RPG_GO_CAMP)
            continue;

        CityHub const* hub = HubByZone(player->GetZoneId());
        bool const onSquare = hub && OnHangoutSquare(player, *hub);
        bool const localHangout = (status == RPG_REST || status == RPG_WANDER_RANDOM);
        if (localHangout && onSquare)
            continue;

        uint32 const botId = player->GetGUID().GetCounter();
        bool const active = HangoutActive(botId);
        uint32 const desired = DesiredHubCrowd(player->GetZoneId());
        uint32 const have = byZone[player->GetZoneId()];
        if (desired && have > desired)
            continue;

        uint32 chance = HangoutHoldChance(have, desired, active);
        if (!onSquare && have < desired && chance < 70)
            chance = 70;
        if (urand(1, 100) > chance)
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

    WorldPosition const* dest = PickHangout(*hub);
    if (!dest)
        return false;

    botAI->rpgInfo.ChangeToGoCamp(*dest);
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

bool CircadianBot::SendOutOfCity(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    WorldPosition const pos = PickWorldGrind(bot);
    if (pos == WorldPosition())
        return false;

    botAI->rpgInfo.ChangeToGoGrind(pos);
    _hangoutUntil.erase(bot->GetGUID().GetCounter());
    return true;
}

uint32 CircadianBot::EvictSurplusCityBots(uint32 maxEvict)
{
    if (!maxEvict)
        return 0;

    std::unordered_map<uint32, uint32> byZone;
    CountBotsByZone(byZone);

    PlayerBotMap const bots = sRandomPlayerbotMgr.GetAllBots();
    std::vector<Player*> evict;
    evict.reserve(maxEvict);

    struct HubExtra
    {
        CityHub const* hub = nullptr;
        uint32 extra = 0;
    };
    std::vector<HubExtra> over;
    uint32 extraTotal = 0;
    for (CityHub const& hub : g_cityHubs)
    {
        uint32 const desired = DesiredHubCrowd(hub.zoneId);
        uint32 const have = byZone[hub.zoneId];
        if (!desired || have <= desired)
            continue;
        uint32 const extra = have - desired;
        over.push_back({ &hub, extra });
        extraTotal += extra;
    }
    std::sort(over.begin(), over.end(), [](HubExtra const& a, HubExtra const& b)
    {
        return a.extra > b.extra;
    });

    for (HubExtra const& item : over)
    {
        if (evict.size() >= maxEvict)
            break;

        CityHub const& hub = *item.hub;
        // Share the tick across overfull hubs so Dalaran 12x does not
        // wait behind Ironforge's leftover 13.
        uint32 share = item.extra;
        if (extraTotal > maxEvict && extraTotal)
            share = std::max<uint32>(4, (item.extra * maxEvict) / extraTotal);
        if (share > maxEvict - evict.size())
            share = maxEvict - evict.size();

        std::vector<Player*> offSquare;
        std::vector<Player*> onSquare;
        offSquare.reserve(item.extra);
        onSquare.reserve(item.extra);

        for (auto const& [guid, player] : bots)
        {
            if (!player || !sRandomPlayerbotMgr.IsRandomBot(player))
                continue;
            if (player->GetZoneId() != hub.zoneId)
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
            if (status == RPG_DO_QUEST || status == RPG_OUTDOOR_PVP || status == RPG_TRAVEL_FLIGHT)
                continue;
            if (status == RPG_GO_GRIND)
                continue;

            if (OnHangoutSquare(player, hub) && HangoutActive(player->GetGUID().GetCounter()))
                onSquare.push_back(player);
            else
                offSquare.push_back(player);
        }

        ShufflePlayers(offSquare);
        ShufflePlayers(onSquare);

        uint32 taken = 0;
        for (Player* bot : offSquare)
        {
            if (taken >= share)
                break;
            evict.push_back(bot);
            ++taken;
        }
        for (Player* bot : onSquare)
        {
            if (taken >= share)
                break;
            evict.push_back(bot);
            ++taken;
        }
    }

    // Playerbots GoCamp walks extras in on its own (ProbTeleToBankers / inn hubs).
    // Turn around anyone still on the road to an already-full capital.
    for (auto const& [guid, player] : bots)
    {
        if (evict.size() >= maxEvict)
            break;
        if (!player || !sRandomPlayerbotMgr.IsRandomBot(player) || IsInCapital(player))
            continue;
        if (player->GetGroup() || player->InBattleground() || player->IsInCombat())
            continue;
        if (!player->IsInWorld() || player->IsBeingTeleported() || !player->IsAlive())
            continue;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
        if (!botAI || botAI->rpgInfo.GetStatus() != RPG_GO_CAMP)
            continue;

        auto const* data = std::get_if<NewRpgInfo::GoCamp>(&botAI->rpgInfo.data);
        if (!data)
            continue;

        uint32 const destZone = sMapMgr->GetZoneId(PHASEMASK_NORMAL, data->pos.GetMapId(),
            data->pos.GetPositionX(), data->pos.GetPositionY(), data->pos.GetPositionZ());
        uint32 const desired = DesiredHubCrowd(destZone);
        uint32 const have = byZone[destZone];
        if (!desired || have < desired)
            continue;

        evict.push_back(player);
    }

    uint32 sent = 0;
    for (Player* bot : evict)
    {
        if (sent >= maxEvict)
            break;
        if (SendOutOfCity(bot))
            ++sent;
    }
    return sent;
}

bool CircadianBot::TeleportBotTo(Player* bot, WorldPosition const& dest)
{
    if (!bot || dest == WorldPosition())
        return false;

    float const x = dest.GetPositionX() + frand(-6.0f, 6.0f);
    float const y = dest.GetPositionY() + frand(-6.0f, 6.0f);
    float z = dest.GetPositionZ();
    if (Map const* map = sMapMgr->CreateBaseMap(dest.GetMapId()))
    {
        float const ground = map->GetHeight(PHASEMASK_NORMAL, x, y, z + 5.0f, true);
        if (ground > INVALID_HEIGHT)
            z = ground + 0.05f;
    }

    bot->GetMotionMaster()->Clear();
    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
        botAI->Reset(true);
    bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
    if (!bot->TeleportTo(dest.GetMapId(), x, y, z, dest.GetOrientation()))
        return false;
    bot->SendMovementFlagUpdate();
    return true;
}

void CircadianBot::SeedCityHangouts()
{
    _citiesSeeded = true;
    if (!IsCityHangoutEnabled())
        return;

    PlayerBotMap const bots = sRandomPlayerbotMgr.GetAllBots();
    std::unordered_map<uint32, std::vector<Player*>> inHub;
    std::vector<Player*> field;
    field.reserve(bots.size());

    for (auto const& [guid, player] : bots)
    {
        if (!EligibleForSeed(player))
            continue;
        if (CityHub const* hub = HubByZone(player->GetZoneId()))
            inHub[hub->zoneId].push_back(player);
        else
            field.push_back(player);
    }

    struct HubPlan
    {
        CityHub const* hub = nullptr;
        uint32 desired = 0;
        std::vector<Player*> keep;
        std::vector<Player*> extra;
    };
    std::vector<HubPlan> plans;
    plans.reserve(g_cityHubs.size());

    for (CityHub const& hub : g_cityHubs)
    {
        if (!hub.resolved)
            continue;

        HubPlan plan;
        plan.hub = &hub;
        plan.desired = DesiredHubCrowd(hub.zoneId);
        std::vector<Player*>& here = inHub[hub.zoneId];
        std::vector<Player*> onSquare;
        std::vector<Player*> offSquare;
        for (Player* bot : here)
        {
            if (OnHangoutSquare(bot, hub))
                onSquare.push_back(bot);
            else
                offSquare.push_back(bot);
        }
        ShufflePlayers(onSquare);
        ShufflePlayers(offSquare);

        uint32 keep = plan.desired;
        for (Player* bot : onSquare)
        {
            if (plan.keep.size() >= keep)
                plan.extra.push_back(bot);
            else
                plan.keep.push_back(bot);
        }
        for (Player* bot : offSquare)
        {
            if (plan.keep.size() >= keep)
                plan.extra.push_back(bot);
            else
                plan.keep.push_back(bot);
        }
        plans.push_back(std::move(plan));
    }

    std::sort(plans.begin(), plans.end(), [](HubPlan const& a, HubPlan const& b)
    {
        if (a.hub->weight != b.hub->weight)
            return a.hub->weight > b.hub->weight;
        uint32 const aNeed = a.desired > a.keep.size() ? a.desired - static_cast<uint32>(a.keep.size()) : 0;
        uint32 const bNeed = b.desired > b.keep.size() ? b.desired - static_cast<uint32>(b.keep.size()) : 0;
        return aNeed > bNeed;
    });

    ShufflePlayers(field);

    uint32 teleIn = 0;
    uint32 teleOut = 0;

    auto takeFit = [&](std::vector<Player*>& pool, CityHub const& hub) -> Player*
    {
        for (auto it = pool.begin(); it != pool.end(); ++it)
        {
            if (!EligibleForSeed(*it) || !BotFitsHub(*it, hub))
                continue;
            Player* bot = *it;
            pool.erase(it);
            return bot;
        }
        return nullptr;
    };

    for (HubPlan& plan : plans)
    {
        while (plan.keep.size() < plan.desired)
        {
            Player* bot = nullptr;
            for (HubPlan& donor : plans)
            {
                if (donor.hub == plan.hub || donor.extra.empty())
                    continue;
                bot = takeFit(donor.extra, *plan.hub);
                if (bot)
                    break;
            }
            if (!bot)
                bot = takeFit(field, *plan.hub);
            if (!bot)
                break;

            WorldPosition const* dest = PickHangout(*plan.hub);
            if (!dest || !TeleportBotTo(bot, *dest))
                continue;

            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            {
                botAI->rpgInfo.ChangeToRest();
                bot->SetStandState(UNIT_STAND_STATE_SIT);
            }
            EnsureHangoutSession(bot);
            plan.keep.push_back(bot);
            ++teleIn;
        }
    }

    for (HubPlan& plan : plans)
    {
        for (Player* bot : plan.extra)
        {
            if (!EligibleForSeed(bot))
                continue;
            WorldPosition const grind = PickWorldGrind(bot);
            if (grind == WorldPosition() || !TeleportBotTo(bot, grind))
                continue;
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                botAI->rpgInfo.ChangeToGoGrind(grind);
            _hangoutUntil.erase(bot->GetGUID().GetCounter());
            ++teleOut;
        }
    }

    std::string planned;
    for (HubPlan const& plan : plans)
    {
        if (!planned.empty())
            planned += ", ";
        planned += Acore::StringFormat("{} {}/{}", plan.hub->name, plan.keep.size(), plan.desired);
    }
    LogLine(Acore::StringFormat("CircadianBot: seeded cities (in {} out {}) {}",
        teleIn, teleOut, planned));
}

void CircadianBot::UpdateCityHangout()
{
    PruneHangouts();

    uint32 const cityTarget = GetCityTarget();
    uint32 const cityOnline = GetOnlineCityBots();
    uint32 evicts = _logoutsPerTick;
    if (cityTarget && cityOnline > cityTarget * 2)
        evicts = _logoutsPerTick * 3;
    EvictSurplusCityBots(evicts);
    ExtendCityDwell();

    if (!cityTarget)
        return;

    // Fill Stormwind even when Dalaran is over the global city cap.
    // TryFly / TryWalk already skip hubs that are full.
    std::unordered_map<uint32, uint32> byZone;
    CountBotsByZone(byZone);
    uint32 deficit = 0;
    for (CityHub const& hub : g_cityHubs)
    {
        uint32 const desired = DesiredHubCrowd(hub.zoneId);
        uint32 const have = byZone[hub.zoneId];
        if (desired && have < desired)
            deficit += desired - have;
    }
    if (!deficit)
        return;

    // Floor is NudgesPerTick; scale up when a hub is far behind the curve
    // so 4pm→7pm actually fills instead of arriving after peak.
    uint32 cap = _cityNudgesPerTick * 8;
    if (cap < 8)
        cap = 8;
    if (cap > 40)
        cap = 40;
    uint32 nudges = deficit;
    if (nudges < _cityNudgesPerTick)
        nudges = _cityNudgesPerTick;
    if (nudges > cap)
        nudges = cap;

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
        LogLine(Acore::StringFormat("CircadianBot: {} / {} city {} / {} ({}) {}",
            GetOnlineRandomBots(), GetTarget(), GetOnlineCityBots(), GetCityTarget(),
            CurrentScheduleName(), CityHubSummary()));
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
        {
            if (!_citiesSeeded && _seedOnStart)
            {
                uint32 const ready = GetOnlineRandomBots();
                if (target && ready * 10 >= target * 8)
                    SeedCityHangouts();
            }
            UpdateCityHangout();
        }

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
