/*****************************************************************************
 * Project:Atmos visual foliage preferences. Licensed GPLv3 like OpenRCT2.
 *****************************************************************************/
#pragma once

#include "../Context.h"
#include "../Diagnostic.h"
#include "../PlatformEnvironment.h"
#include "../object/ObjectManager.h"
#include "../object/Object.h"
#include "../object/ObjectLimits.h"
#include "../object/SmallSceneryEntry.h"
#include "../object/LargeSceneryEntry.h"
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <string>

namespace OpenRCT2::Drawing::AtmosphereFoliage
{
    inline constexpr std::array kTypes{ObjectType::smallScenery, ObjectType::largeScenery, ObjectType::walls};
    inline constexpr size_t kSlots = kMaxSmallSceneryObjects;
    // Painting workers read only atomic flags. Object loading and UI changes publish on the main thread.
    inline std::array<std::array<std::atomic<bool>, kSlots>, kTypes.size()> enabled{};
    inline std::array<std::array<std::string, kSlots>, kTypes.size()> identities{};
    inline std::map<std::string, bool> overrides;
    inline std::filesystem::path profile;
    inline std::atomic<uint64_t> revision{};

    inline int TypeIndex(ObjectType type)
    {
        for (size_t i=0;i<kTypes.size();i++) if (kTypes[i]==type) return static_cast<int>(i);
        return -1;
    }

    inline bool IsEnabled(ObjectType type, size_t index)
    {
        const auto t=TypeIndex(type);
        return t>=0 && index<kSlots && enabled[t][index].load(std::memory_order_relaxed);
    }

    inline std::string Identity(const Object& object)
    {
        return std::to_string(static_cast<int>(object.GetObjectType()))+":"+object.GetDescriptor().ToString();
    }

    inline bool Default(Object& object)
    {
        const auto id=object.GetIdentifier();
        if (!(id.starts_with("rct2.") || id.starts_with("rct2tt.") || id.starts_with("rct2ww.") || id.starts_with("rct1.")))
            return false; // Custom flags, names and pixel colours never opt an object in.
        if (object.GetObjectType()==ObjectType::smallScenery)
        {
            const auto* entry=static_cast<const SmallSceneryEntry*>(object.GetLegacyData());
            if (entry && entry->flags.hasAny(SmallSceneryFlag::isTree, SmallSceneryFlag::canWither)) return true;
            // Original shrubs/topiary lack the tree/watering flags. These are explicit original IDs, not guesses.
            constexpr std::array<std::string_view, 29> plants{
                "fern1","ggrs1","tal","tbc","tcb","tdm","tht","tjb1","tjb2","tjb3","tjb4","tjf",
                "torn1","torn2","ts0","ts1","ts2","ts3","ts4","ts5","ts6","tsc","tsd",
                "tsh0","tsh1","tsh2","tsh3","tsh4","tsh5"};
            if (id=="rct2.scenery_small.tsq") return true;
            for (const auto plant:plants) if (id==std::string("rct2.scenery_small.")+std::string(plant)) return true;
        }
        else if (object.GetObjectType()==ObjectType::largeScenery)
        {
            const auto* entry=static_cast<const LargeSceneryEntry*>(object.GetLegacyData());
            return entry && entry->flags.has(LargeSceneryFlag::isTree);
        }
        return false;
    }

    inline void Refresh(IObjectManager& objects)
    {
        bool changed=false;
        for (size_t t=0;t<kTypes.size();t++) for (size_t i=0;i<kSlots;i++)
        {
            auto* object=objects.GetLoadedObject(kTypes[t],i);
            auto key=object ? Identity(*object) : std::string{};
            const auto found=overrides.find(key);
            const bool on=object && (found==overrides.end() ? Default(*object) : found->second);
            changed |= identities[t][i]!=key || enabled[t][i].load(std::memory_order_relaxed)!=on;
            identities[t][i]=std::move(key);
            enabled[t][i].store(on,std::memory_order_relaxed);
        }
        if (changed) revision.fetch_add(1,std::memory_order_relaxed);
    }

    // Called from the UI/frame owner, never from object constructors/destructors or painting workers.
    inline bool SyncProfile(IObjectManager& objects)
    {
        const auto path=std::filesystem::u8path(GetContext()->GetPlatformEnvironment().GetDirectoryPath(DirBase::user))
            / "atmosphere-foliage.txt";
        if (profile==path) return false;
        profile=path;overrides.clear();
        std::ifstream input(profile);
        std::string magic;
        if (std::getline(input,magic) && magic=="Project-Atmos foliage 1")
        {
            std::string key;int value;
            while(input>>std::quoted(key)>>value) if(value==0 || value==1) overrides[key]=value!=0;
        }
        Refresh(objects);
        return true;
    }

    inline bool SetEnabled(IObjectManager& objects, ObjectType type, size_t index, bool on)
    {
        SyncProfile(objects);
        const int t=TypeIndex(type);
        auto* object=t>=0 && index<kSlots ? objects.GetLoadedObject(type,index) : nullptr;
        if (!object) return false;
        const auto key=Identity(*object);
        auto updated=overrides;
        if (on==Default(*object)) updated.erase(key); else updated[key]=on;
        auto temp=profile;temp+=".tmp";
        std::ofstream output(temp,std::ios::trunc);
        output<<"Project-Atmos foliage 1\n";
        for (const auto& [id,value]:updated) output<<std::quoted(id)<<' '<<int(value)<<'\n';
        output.close();
        std::error_code error;
        if (output) std::filesystem::copy_file(temp,profile,std::filesystem::copy_options::overwrite_existing,error);
        if (!output || error) { LOG_ERROR("Unable to save Atmosphere foliage preferences");return false; }
        std::filesystem::remove(temp,error);
        overrides=std::move(updated);
        enabled[t][index].store(on,std::memory_order_relaxed);
        revision.fetch_add(1,std::memory_order_relaxed);
        return true;
    }
}
