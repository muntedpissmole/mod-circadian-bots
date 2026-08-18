#ifndef CIRCADIAN_BOT_H
#define CIRCADIAN_BOT_H

#include "Define.h"
#include <array>
#include <ctime>
#include <string>
#include <unordered_map>

class Player;
class WorldPosition;

class CircadianBot
{
public:
    static CircadianBot& Instance();

    void LoadConfig();
    void Initialize();
    void Update(uint32 diff);
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return _enabled; }
    bool IsCityHangoutEnabled() const { return _enabled && _cityHangout; }
    uint32 GetPeak() const { return _peak; }
    uint32 GetTarget() const;
    uint32 GetCityTarget() const;
    uint32 GetOnlineRandomBots() const;
    uint32 GetOnlineCityBots() const;
    std::string CityHubSummary() const;
    char const* CurrentScheduleName() const;
    void SetManualTarget(uint32 target);
    void ClearManualTarget();
    bool HasManualTarget() const { return _manualTarget != 0; }
    uint32 LogoutSurplus(uint32 target, uint32 maxLogouts);
    void LogStatus() const;

private:
    CircadianBot() = default;

    void ApplyTarget(uint32 target);
    void LogLine(std::string const& line) const;
    void CheckTargetReached(uint32 online, uint32 target);
    uint32 HourPercent(std::time_t now) const;
    bool IsWeekend(std::time_t now) const;
    char const* ScheduleName(std::time_t now) const;
    void LoadHourTable(char const* keyPrefix, std::array<uint32, 24>& dest, uint32 const* defaults);
    void ResolveCityHubs();
    void UpdateCityHangout();
    void PruneHangouts();
    void ExtendCityDwell();
    bool SendOutOfCity(Player* bot);
    uint32 EvictSurplusCityBots(uint32 maxEvict);
    uint32 NudgeTowardCities(uint32 maxNudges);
    bool CanNudge(Player* bot) const;
    bool IsInCapital(Player const* bot) const;
    bool HangoutActive(uint32 botId) const;
    void EnsureHangoutSession(Player* bot);
    void ApplyCityHangout(Player* bot);
    bool TryWalkToCapital(Player* bot);
    bool TryFlyToCapital(Player* bot);
    bool TeleportBotTo(Player* bot, WorldPosition const& dest);
    void SeedCityHangouts();
    void CountBotsByZone(std::unordered_map<uint32, uint32>& out) const;
    uint32 DesiredHubCrowd(uint32 zoneId) const;

    bool _enabled = true;
    bool _logging = true;
    bool _cityHangout = true;
    uint32 _peak = 500;
    uint32 _checkInterval = 30000;
    uint32 _logInterval = 300000;
    uint32 _logoutsPerTick = 40;
    uint32 _cityNudgesPerTick = 3;
    uint32 _cityMinLevel = 10;
    uint32 _cityMinSession = 480;
    uint32 _cityMaxSession = 1200;
    uint32 _cityMajorHub = 100;
    bool _seedOnStart = true;
    bool _citiesSeeded = false;
    uint32 _manualTarget = 0;
    uint32 _elapsed = 0;
    uint32 _logElapsed = 0;
    uint32 _lastApplied = 0;
    uint32 _lastLoggedHour = 24;
    bool _targetReached = false;
    std::string _logFile;
    std::array<uint32, 24> _hourPct{};
    std::array<uint32, 24> _weekendHourPct{};
    std::unordered_map<uint32, std::time_t> _hangoutUntil;
};

#define sCircadianBot CircadianBot::Instance()

#endif
