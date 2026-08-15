#ifndef CIRCADIAN_BOT_H
#define CIRCADIAN_BOT_H

#include "Define.h"
#include <array>
#include <ctime>
#include <string>

class CircadianBot
{
public:
    static CircadianBot& Instance();

    void LoadConfig();
    void Initialize();
    void Update(uint32 diff);
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return _enabled; }
    uint32 GetPeak() const { return _peak; }
    uint32 GetTarget() const;
    uint32 GetOnlineRandomBots() const;
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

    bool _enabled = false;
    bool _logging = true;
    uint32 _peak = 500;
    uint32 _checkInterval = 30000;
    uint32 _logInterval = 300000;
    uint32 _logoutsPerTick = 40;
    uint32 _manualTarget = 0;
    uint32 _elapsed = 0;
    uint32 _logElapsed = 0;
    uint32 _lastApplied = 0;
    uint32 _lastLoggedHour = 24;
    bool _targetReached = false;
    std::string _logFile;
    std::array<uint32, 24> _hourPct{};
};

#define sCircadianBot CircadianBot::Instance()

#endif
