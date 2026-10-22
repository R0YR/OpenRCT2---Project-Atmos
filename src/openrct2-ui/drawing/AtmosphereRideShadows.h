/*****************************************************************************
 * Atmosphere per-ride rendering preferences. Licensed GPLv3 like OpenRCT2.
 *****************************************************************************/
#pragma once

#include <openrct2/Context.h>
#include <openrct2/Diagnostic.h>
#include <openrct2/GameState.h>
#include <openrct2/PlatformEnvironment.h>
#include <openrct2/ride/Ride.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>

namespace OpenRCT2::Ui::Atmosphere::RideShadows
{
    // Temporarily parked while guest/staff reception is refined. Preserve saved per-ride preferences.
    inline constexpr bool kRenderingEnabled = false;

    namespace Detail
    {
        struct RideIdentity
        {
            uint32_t type{};
            int32_t buildDate{};
            std::string name;

            bool operator==(const RideIdentity&) const = default;
        };

        // UI changes and renderer collection both run on the main thread. No worker accesses this state.
        inline std::map<uint16_t, RideIdentity> disabled;
        inline std::string parkName;
        inline std::string scenario;
        inline TileCoordsXY mapSize{};
        inline std::string parkKey;
        inline std::filesystem::path filePath;
        inline bool loaded = false;
        inline uint64_t revision = 0;

        inline RideIdentity Identity(const Ride& ride)
        {
            return { static_cast<uint32_t>(ride.type), ride.buildDate, ride.getName() };
        }

        inline void SyncPark()
        {
            const auto& state = getGameState();
            if (loaded && parkName == state.park.name && scenario == state.scenarioFileName
                && mapSize == state.mapSize)
                return;
            loaded = true;
            parkName = state.park.name;
            scenario = state.scenarioFileName;
            mapSize = state.mapSize;
            std::ostringstream key;
            key << std::quoted(parkName) << ' ' << std::quoted(scenario) << ' ' << mapSize.x << ' ' << mapSize.y;
            parkKey = key.str();
            uint64_t hash = 14695981039346656037ULL;
            for (const unsigned char c : parkKey)
                hash = (hash ^ c) * 1099511628211ULL;
            std::ostringstream name;
            name << "atmosphere-ride-shadows-" << std::hex << hash << ".txt";
            filePath = std::filesystem::u8path(GetContext()->GetPlatformEnvironment().GetDirectoryPath(DirBase::user)) / name.str();
            disabled.clear();
            ++revision;
            std::ifstream input(filePath);
            std::string storedKey;
            if (!std::getline(input, storedKey) || storedKey != parkKey)
                return;
            uint32_t id;
            RideIdentity identity;
            while (input >> id >> identity.type >> identity.buildDate >> std::quoted(identity.name))
            {
                if (id < Limits::kMaxRidesInPark)
                    disabled.emplace(static_cast<uint16_t>(id), identity);
            }
        }

        inline bool Save()
        {
            auto temporary = filePath;
            temporary += ".tmp";
            std::ofstream output(temporary, std::ios::trunc);
            output << parkKey << '\n';
            for (const auto& [id, identity] : disabled)
                output << id << ' ' << identity.type << ' ' << identity.buildDate << ' ' << std::quoted(identity.name) << '\n';
            output.close();
            if (!output)
                return false;
            std::error_code error;
            std::filesystem::copy_file(temporary, filePath, std::filesystem::copy_options::overwrite_existing, error);
            if (error)
                return false;
            std::filesystem::remove(temporary, error);
            return true;
        }
    }

    inline bool IsEnabled(RideId id)
    {
        Detail::SyncPark();
        const auto found = Detail::disabled.find(id.ToUnderlying());
        if (found == Detail::disabled.end())
            return true;
        const auto* ride = GetRide(id);
        return ride == nullptr || !(found->second == Detail::Identity(*ride));
    }

    // A local visual preference: never mutates the park, simulation, or multiplayer state.
    inline bool SetEnabled(RideId id, bool enabled)
    {
        Detail::SyncPark();
        const auto* ride = GetRide(id);
        if (ride == nullptr)
            return false;
        if (enabled)
            Detail::disabled.erase(id.ToUnderlying());
        else
            Detail::disabled[id.ToUnderlying()] = Detail::Identity(*ride);
        ++Detail::revision;
        const bool saved = Detail::Save();
        if (!saved)
            LOG_WARNING("Unable to persist Atmosphere ride shadow preference.");
        return saved;
    }

    inline uint64_t Revision()
    {
        Detail::SyncPark();
        return Detail::revision;
    }
}
