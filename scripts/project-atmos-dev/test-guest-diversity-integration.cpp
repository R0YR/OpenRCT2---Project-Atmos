// Real-core integration harness. Arguments: data-dir isolated-user-dir rct2-dir test-park preview-dir
// Uses only an imported fixture and an in-memory .park round trip; never saves to the input park.
#include <openrct2/Context.h>
#include <openrct2/GameState.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/ParkImporter.h>
#include <openrct2/PlatformEnvironment.h>
#include <openrct2/command_line/sprite/SpriteCommands.h>
#include <openrct2/config/Config.h>
#include <openrct2/core/Imaging.h>
#include <openrct2/core/File.h>
#include <openrct2/core/MemoryStream.h>
#include <openrct2/core/OrcaStream.hpp>
#include <openrct2/drawing/Drawing.Sprite.h>
#include <openrct2/drawing/Colour.h>
#include <openrct2/drawing/RenderTarget.h>
#include <openrct2/entity/EntityRegistry.h>
#include <openrct2/entity/EntityList.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/object/ObjectList.h>
#include <openrct2/object/ObjectFactory.h>
#include <openrct2/object/ObjectManager.h>
#include <openrct2/object/ObjectRepository.h>
#include <openrct2/object/PeepAnimationsObject.h>
#include <openrct2/network/Network.h>
#include <openrct2/park/ParkFile.h>
#include <openrct2/park/Legacy.h>
#include <openrct2/rct12/RCT12.h>
#include <openrct2/peep/PeepAnimations.h>
#include <openrct2/peep/PeepSpriteIds.h>
#include <openrct2/scenario/Scenario.h>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

using namespace OpenRCT2;

static void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

static void exportPose(
    const PeepAnimationsObject& object, const std::filesystem::path& directory, const std::string& label,
    PeepAnimationGroup group, PeepAnimationType action, uint8_t direction, bool accessory = false, size_t animationFrame = 0)
{
    const auto& animation = object.GetPeepAnimation(group, action);
    const auto frame = animation.frameOffsets.at(animationFrame);
    const auto index = animation.baseImage + frame * 4 + direction;
    const auto* element = GfxGetG1Element(index);
    require(element != nullptr && element->offset != nullptr, "preview body G1 missing");
    const auto name = label + "-" + std::to_string(direction);
    require(CommandLine::Sprite::SpriteImageExport(*element, (directory / (name + "-raw.png")).string()), "raw export failed");

    Image image;
    image.Width = 64;
    image.Height = 64;
    image.Depth = 8;
    image.Stride = 64;
    image.Palette = StandardPalette;
    image.Pixels.resize(64 * 64);
    Drawing::RenderTarget rt;
    rt.bits = reinterpret_cast<Drawing::PaletteIndex*>(image.Pixels.data());
    rt.width = rt.cullingWidth = 64;
    rt.height = rt.cullingHeight = 64;
    const ScreenCoordsXY anchor{ 32, 48 };
    GfxDrawSpriteSoftware(rt, ImageId(index, Drawing::Colour::brightRed, Drawing::Colour::darkBlue), anchor);
    if (accessory)
    {
        const auto itemFrame = action == PeepAnimationType::watchRide ? 6 : action == PeepAnimationType::sittingIdle ? 7 : frame % 6;
        const auto item = object.GetAccessoryImage(group, itemFrame, direction);
        require(GfxGetG1Element(item) != nullptr, "preview accessory G1 missing");
        GfxDrawSpriteSoftware(rt, ImageId(item, Drawing::Colour::yellow), anchor);
    }
    Imaging::WriteToFile((directory / (name + ".png")).string(), image, ImageFormat::png);
}

int main(int argc, char** argv)
{
    try
    {
        require(argc == 6, "arguments: data-dir isolated-user-dir rct2-dir test-park preview-dir");
        gOpenRCT2Headless = true;
        gOpenRCT2NoGraphics = false;
        gCustomOpenRCT2DataPath = argv[1];
        gCustomUserDataPath = argv[2];
        gCustomRCT2DataPath = argv[3];
        Config::SetDefaults();
        Config::OpenFromPath((std::filesystem::path(argv[2]) / "config.ini").string());
        auto context = CreateContext();
        require(context->Initialise(), "context init failed");
        const auto networkVersion = Network::GetVersion();
        require(networkVersion == "Multiplayer disabled" || networkVersion.ends_with("-atmosphere-guests-2"),
            "multiplayer protocol lacks guest simulation compatibility stamp");
        std::cout << "PASS multiplayer compatibility: " << networkVersion << '\n';
        require(context->LoadParkFromFile(argv[4]), "fixture park load failed");
        auto& manager = context->GetObjectManager();
        auto& state = getGameState();
        for (const auto* guest : EntityList<Guest>())
            require(manager.GetLoadedObject<PeepAnimationsObject>(guest->animationObjectIndex)->GetIdentifier()
                == "rct2.peep_animations.guest", "legacy fixture guests were overridden by variants");
        state.entities.resetAllEntities();

        auto sparse = manager.GetLoadedObjects();
        sparse.GetList(ObjectType::peepAnimations).clear();
        sparse.SetObject(ObjectType::peepAnimations, 9, "rct2.peep_animations.guest");
        manager.LoadObjects(sparse);
        require(findGuestAnimationsBasisIndex() == 9, "sparse normal basis changed");
        std::array<ObjectEntryIndex, kAtmosphereGuestAnimationIdentifiers.size()> slots;
        std::set<ObjectEntryIndex> unique{ 9 };
        for (size_t i = 0; i < slots.size(); ++i)
        {
            slots[i] = manager.GetLoadedObjectEntryIndex(kAtmosphereGuestAnimationIdentifiers[i]);
            require(slots[i] != kObjectEntryIndexNull, "bundled variant not auto loaded");
            require(unique.insert(slots[i]).second, "duplicate appearance slot");
        }
        manager.LoadObjects(manager.GetLoadedObjects());
        require(findGuestAnimationsBasisIndex() == 9, "repeat load basis changed");
        for (size_t i = 0; i < slots.size(); ++i)
            require(slots[i] == manager.GetLoadedObjectEntryIndex(kAtmosphereGuestAnimationIdentifiers[i]), "repeat load changed slot");
        auto* legacyGuest = Guest::generate({ 64, 64, 16 });
        require(legacyGuest != nullptr, "cannot create legacy conversion fixture");
        legacyGuest->animationObjectIndex = kObjectEntryIndexNull;
        legacyGuest->animationGroup = PeepAnimationGroup::normal;
        ConvertPeepAnimationTypeToObjects(state);
        require(legacyGuest->animationObjectIndex == 9, "legacy conversion picked a variant instead of original");
        state.entities.resetAllEntities();
        std::cout << "PASS five active variants, legacy import unchanged\n";

        std::array<ObjectEntryIndex, 1200> sequence;
        std::map<ObjectEntryIndex, size_t> counts;
        ScenarioRandSeed(0x12345678, 0x87654321);
        for (auto& index : sequence)
        {
            index = findRandomGuestAnimationsIndex();
            ++counts[index];
        }
        ScenarioRandSeed(0x12345678, 0x87654321);
        for (const auto index : sequence)
            require(index == findRandomGuestAnimationsIndex(), "seeded appearance sequence differs");
        require(counts.size() == 7, "random selection did not cover all seven appearances");
        require(counts[9] > 380 && counts[9] < 600, "original appearance frequency outside expected range");
        for (const auto slot : slots)
            require(counts[slot] > 50 && counts[slot] < 190, "variant frequency outside expected range");
        std::cout << "PASS deterministic weighted selection 1200; original=" << counts[9] << '\n';

        std::vector<ObjectEntryDescriptor> variants;
        for (const auto id : kAtmosphereGuestAnimationIdentifiers)
            variants.emplace_back(id);
        manager.UnloadObjects(variants);
        const auto rngBefore = ScenarioRandState();
        require(findRandomGuestAnimationsIndex() == 9, "no-variant fallback changed appearance");
        require(rngBefore.s0 == ScenarioRandState().s0 && rngBefore.s1 == ScenarioRandState().s1,
            "no-variant fallback consumed RNG");
        auto* oldGuest = Guest::generate({ 64, 64, 16 });
        require(oldGuest != nullptr && oldGuest->animationObjectIndex == 9, "original guest creation failed");
        const auto oldId = oldGuest->id;
        manager.LoadObjects(sparse);
        require(oldGuest->animationObjectIndex == 9, "loading variants changed existing guest");
        std::cout << "PASS absent-variant RNG preservation and existing guest identity\n";

        const auto* normal = manager.GetLoadedObject<PeepAnimationsObject>(9);
        const auto previewRoot = std::filesystem::path(argv[5]);
        std::filesystem::create_directories(previewRoot);
        for (size_t appearance = 0; appearance <= slots.size(); ++appearance)
        {
            const auto slot = appearance == 0 ? ObjectEntryIndex{ 9 } : slots[appearance - 1];
            const auto* object = manager.GetLoadedObject<PeepAnimationsObject>(slot);
            const auto child = appearance == 5 || appearance == 6;
            require(object->GetNumAnimationGroups() == normal->GetNumAnimationGroups(), "animation groups not inherited");
            for (size_t group = 0; group < object->GetNumAnimationGroups(); ++group)
            {
                const auto pag = static_cast<PeepAnimationGroup>(group);
                for (const auto& [name, action] : getAnimationsByPeepType(AnimationPeepType::guest))
                {
                    const auto& animation = object->GetPeepAnimation(pag, action);
                    require(!animation.frameOffsets.empty(), "animation has no frame offsets");
                    for (const auto frame : animation.frameOffsets)
                    {
                        const auto directions = action == PeepAnimationType::hanging ? 1 : 4;
                        for (int d = 0; d < directions; ++d)
                        {
                            const auto index = animation.baseImage + (directions == 1 ? frame : frame * 4 + d);
                            const auto* g1 = GfxGetG1Element(index);
                            require(g1 != nullptr && g1->offset != nullptr, "inherited directional frame G1 missing");
                            require(g1->width > 0 && g1->height > 0, "inherited directional frame is empty");
                            if (child)
                            {
                                const auto& source = normal->GetPeepAnimation(pag, action);
                                const auto* original = GfxGetG1Element(source.baseImage + (directions == 1 ? frame : frame * 4 + d));
                                // Body transforms reserve four pixels for hair before applying 3/4 scale.
                                require(g1->width <= (original->width + 4) * 3 / 4 + 2
                                        && g1->height <= (original->height + 4) * 3 / 4 + 2,
                                    "child frame exceeds scaled source with hair padding");
                            }
                        }
                    }
                }
            }
            for (const auto pag : { PeepAnimationGroup::hat, PeepAnimationGroup::balloon, PeepAnimationGroup::umbrella })
                for (uint8_t frame = 0; frame < 8; ++frame)
                    for (uint8_t d = 0; d < 4; ++d)
                    {
                        const auto* g1 = GfxGetG1Element(object->GetAccessoryImage(pag, frame, d));
                        require(g1 != nullptr && g1->offset != nullptr, "accessory directional G1 missing");
                        if (child)
                        {
                            const auto* original = GfxGetG1Element(normal->GetAccessoryImage(pag, frame, d));
                            require(g1->width <= original->width && g1->height <= original->height, "child accessory not scaled");
                        }
                    }
            constexpr std::string_view prefix = "atmosphere.peep_animations.";
            const auto label = appearance == 0 ? "normal" : std::string(kAtmosphereGuestAnimationIdentifiers[appearance - 1].substr(prefix.size()));
            for (uint8_t d = 0; d < 4; ++d)
            {
                exportPose(*object, previewRoot, label + "-walk", PeepAnimationGroup::normal, PeepAnimationType::walking, d);
                exportPose(*object, previewRoot, label + "-stand", PeepAnimationGroup::normal, PeepAnimationType::watchRide, d);
                exportPose(*object, previewRoot, label + "-sit", PeepAnimationGroup::normal, PeepAnimationType::sittingIdle, d);
                exportPose(*object, previewRoot, label + "-map", PeepAnimationGroup::normal, PeepAnimationType::readMap, d);
                exportPose(*object, previewRoot, label + "-food", PeepAnimationGroup::burger, PeepAnimationType::eatFood, d);
                exportPose(*object, previewRoot, label + "-hat", PeepAnimationGroup::hat, PeepAnimationType::walking, d, true);
            }
            // One consistent view across all walking frames lets visual review catch hair flicker.
            for (size_t frame = 0; frame < 6; ++frame)
                exportPose(*object, previewRoot, label + "-walk-frame" + std::to_string(frame),
                    PeepAnimationGroup::normal, PeepAnimationType::walking, 0, false, frame);
        }
        std::cout << "PASS all groups/actions/directions, child scaling, accessory G1; actual pipeline previews exported\n";

        std::map<EntityId, ObjectEntryIndex> guestAppearances{ { oldId, 9 } };
        std::set<ObjectEntryIndex> spawned;
        ScenarioRandSeed(0x12345678, 0x87654321);
        for (int i = 0; i < 120; ++i)
        {
            auto* guest = Guest::generate({ 64, 64, 16 });
            require(guest != nullptr, "new guest creation failed");
            spawned.insert(guest->animationObjectIndex);
            guestAppearances.emplace(guest->id, guest->animationObjectIndex);
        }
        require(spawned.size() == 7, "Guest::generate did not cover all appearances");
        MemoryStream memory;
        ParkFileExporter exporter;
        exporter.ExportObjectsList = manager.GetPackableObjects();
        std::map<std::string, std::vector<uint8_t>> expectedArchives;
        for (const auto identifier : kAtmosphereGuestAnimationIdentifiers)
        {
            const auto found = std::find_if(exporter.ExportObjectsList.begin(), exporter.ExportObjectsList.end(),
                [identifier](const auto* item) { return item->Identifier == identifier; });
            require(found != exporter.ExportObjectsList.end(), "variant excluded from packable objects");
            require(std::filesystem::path((*found)->Path).extension() == ".parkobj", "variant packaging uses unsupported loose file");
            expectedArchives.emplace(std::string(identifier), File::ReadAllBytes((*found)->Path));
        }
        exporter.Export(state, memory, kParkFileSaveCompressionLevel);
        memory.SetPosition(0);
        OrcaStream archiveStream(memory, OrcaStream::Mode::reading);
        enum class Chunk : uint32_t { packedObjects = 0x80 };
        std::map<std::string, std::vector<uint8_t>> transferredArchives;
        require(archiveStream.readWriteChunk(Chunk::packedObjects, [&](OrcaStream::ChunkStream& chunk) {
            const auto count = chunk.read<uint32_t>();
            for (uint32_t i = 0; i < count; ++i)
            {
                const auto type = chunk.read<uint8_t>();
                std::string identifier;
                if (type == 0)
                {
                    RCTObjectEntry entry{};
                    chunk.read(&entry, sizeof(entry));
                }
                else
                {
                    require(type == 1, "unknown packed object descriptor");
                    identifier = chunk.read<std::string>();
                }
                const auto size = chunk.read<uint32_t>();
                std::vector<uint8_t> data(size);
                chunk.read(data.data(), data.size());
                if (expectedArchives.contains(identifier))
                {
                    require(data == expectedArchives.at(identifier), "packed archive bytes differ from original .parkobj");
                    require(transferredArchives.emplace(identifier, std::move(data)).second, "variant archive packed twice");
                }
            }
        }), "saved park lacks packedObjects chunk");
        require(transferredArchives.size() == 6, "saved park did not embed all six active archives");

        // Reconstruct missing objects from the actual saved payload in an independent empty repository.
        const auto transferRoot = std::filesystem::path(argv[2]).parent_path() / "transferred-userdata"
            / ("run-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
        auto transferEnvironment = CreatePlatformEnvironment();
        for (const auto base : { DirBase::user, DirBase::cache, DirBase::config, DirBase::rct1, DirBase::rct2, DirBase::openrct2 })
            transferEnvironment->SetBasePath(base, transferRoot.string());
        std::filesystem::create_directories(transferEnvironment->GetDirectoryPath(DirBase::user, DirId::objects));
        auto transferRepository = CreateObjectRepository(*transferEnvironment);
        transferRepository->Construct(0);
        for (const auto& [identifier, data] : transferredArchives)
        {
            require(transferRepository->FindObject(identifier) == nullptr, "transfer repository was not empty");
            transferRepository->AddObjectFromFile(ObjectGeneration::json, identifier, data.data(), data.size());
            const auto* item = transferRepository->FindObject(identifier);
            require(item != nullptr && std::filesystem::path(item->Path).extension() == ".parkobj",
                "independent repository could not extract packed variant");
            auto object = transferRepository->LoadObject(item);
            require(object != nullptr && object->GetIdentifier() == identifier, "independent extracted variant could not load");
        }
        std::cout << "PASS six real embedded .parkobj payloads extracted and loaded in independent repository\n";
        memory.SetPosition(0);
        auto importer = ParkImporter::CreateParkFile(context->GetObjectRepository());
        const auto result = importer->LoadFromStream(&memory, false);
        manager.LoadObjects(result.RequiredObjects);
        importer->Import(state);
        for (const auto& [id, appearance] : guestAppearances)
        {
            const auto* guest = state.entities.getEntity<Guest>(id);
            require(guest != nullptr && guest->animationObjectIndex == appearance, "memory .park roundtrip changed guest appearance");
        }
        require(findGuestAnimationsBasisIndex() == 9, "memory roundtrip changed sparse normal basis");
        for (size_t i = 0; i < slots.size(); ++i)
            require(slots[i] == manager.GetLoadedObjectEntryIndex(kAtmosphereGuestAnimationIdentifiers[i]), "memory roundtrip changed variant slot");
        std::cout << "PASS real Guest::generate and in-memory .park identity roundtrip (121 visitors)\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
