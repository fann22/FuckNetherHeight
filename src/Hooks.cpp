#include "Hooks.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/utility/BinaryStream.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/network/LoopbackPacketSender.h"
#include "mc/network/LoopbackPacketSender.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/NetworkIdentifierWithSubId.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/network/packet/AddVolumeEntityPacket.h"
#include "mc/network/packet/ChangeDimensionPacket.h"
#include "mc/network/packet/DebugDrawerPacket.h"
#include "mc/network/packet/InteractPacket.h"
#include "mc/network/packet/InventoryTransactionPacket.h"
#include "mc/network/packet/LevelChunkPacket.h"
#include "mc/network/packet/PlayerActionPacket.h"
#include "mc/network/packet/PlayerActionType.h"
#include "mc/network/packet/PlayerAuthInputPacket.h"
#include "mc/network/packet/RemoveVolumeEntityPacket.h"
#include "mc/network/packet/ShapeDataPayload.h"
#include "mc/network/packet/SpawnParticleEffectPacket.h"
#include "mc/network/packet/StartGamePacket.h"
#include "mc/network/packet/SubChunkPacket.h"
#include "mc/network/packet/SubChunkRequestPacket.h"
#include "mc/network/packet/UpdateBlockPacket.h"
#include "mc/server/PropertiesSettings.h" // IWYU pragma: keep
#include "mc/server/ServerInstance.h"
#include "mc/server/ServerPlayer.h"
#include "mc/util/MolangVariable.h"
#include "mc/util/VarIntDataOutput.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/ChangeDimensionRequest.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/LoadingScreenIdManager.h"
#include "mc/world/level/SpawnSettings.h"
#include "mc/world/level/biome/glue/BiomeJsonDocumentGlue.h"
#include "mc/world/level/biome/glue/BiomeJsonDocumentGlueResolvedBiomeData.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/dimension/DimensionArguments.h"
#include "mc/world/level/dimension/VanillaDimensions.h"
#include "mc/server/config/server_configuration/ServerConfigurationJoinInfo.h"
#include "mc/events/event_data/ServerTelemetryData.h"
#include "mc/deps/core/resource/ContentIdentity.h"
#include "mc/world/actor/player/PlayerMovementSettings.h"
#include "mc/platform/UUID.h"
#include "mc/world/level/block/definition/BlockDefinitionGroup.h"
#include "mc/world/level/PortalForcer.h"
#include "mc/world/level/PortalShape.h"
#include "mc/world/level/PortalRecord.h"
#include "mc/world/level/block/PortalAxis.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/level/dimension/Dimension.h"

namespace {
using BiomeDataMap = std::unordered_map<std::string, std::unique_ptr<::BiomeJsonDocumentGlueResolvedBiomeData>>;

void patchPacket(MinecraftPacketIds id, Packet& packet) {
    switch (id) {
    case MinecraftPacketIds::RemoveVolumeEntityPacket: {
        auto& pk = (RemoveVolumeEntityPacket&)packet;
        if (pk.mDimensionType == VanillaDimensions::Nether()) {
            pk.mDimensionType = VanillaDimensions::TheEnd();
        }
    }
    case MinecraftPacketIds::AddVolumeEntityPacket: {
        auto& pk = (AddVolumeEntityPacket&)packet;
        if (pk.mDimensionType == VanillaDimensions::Nether()) {
            pk.mDimensionType = VanillaDimensions::TheEnd();
        }
    }
    default:
        return;
    }
}
LL_TYPE_INSTANCE_HOOK(
    PortalForcerCreatePortalHook,
    HookPriority::Normal,
    PortalForcer,
    &PortalForcer::createPortal,
    ::PortalRecord const&,
    ::Actor const& entity,
    int            radius
) {
    if (entity.getDimensionId() != VanillaDimensions::Nether()) {
        return origin(entity, radius);
    }

    BlockSource& region  = entity.getDimensionBlockSource();
    Vec3 const&  pos     = entity.getPosition();
    int          dirOffs = ll::memory::dAccess<Random>(&this->mRandom, 0).nextInt(4);

    BlockPos entityBlockPos{(int)pos.x, (int)pos.y, (int)pos.z};
    BlockPos targetPos = entityBlockPos;

    float closest  = -1.0f;
    int   dirTarget = 0;

    constexpr int PORTAL_MAX_Y = 118;
    constexpr int PORTAL_MIN_Y = 70;

    Vec3 distVec{};

    for (int x = entityBlockPos.x - radius; x <= entityBlockPos.x + radius; x++) {
        distVec.x = (float)x + 0.5f - pos.x;
        for (int z = entityBlockPos.z - radius; z <= entityBlockPos.z + radius; z++) {
            distVec.z = (float)z + 0.5f - pos.z;
            for (int y = PORTAL_MAX_Y; y >= 0; y--) {
                BlockPos checkPos{x, y, z};
                Block const& block = region.getBlock(checkPos);
                if (!block.isAir() && block.getTypeName() == "minecraft:portal") {
                    float dist = distVec.x * distVec.x + distVec.z * distVec.z;
                    if (closest < 0.0f || dist < closest) {
                        closest   = dist;
                        targetPos = checkPos;
                        dirTarget = dirOffs;
                    }
                    break;
                }
            }
        }
    }

    if (closest < 0.0f) {
        for (int x = entityBlockPos.x - radius; x <= entityBlockPos.x + radius; x++) {
            distVec.x = (float)x + 0.5f - pos.x;
            for (int z = entityBlockPos.z - radius; z <= entityBlockPos.z + radius; z++) {
                distVec.z = (float)z + 0.5f - pos.z;
                for (int y = PORTAL_MAX_Y; y >= 0; y--) {
                    BlockPos checkPos{x, y, z};

                    Block const& floor  = region.getBlock(BlockPos{x, y - 1, z});
                    Block const& space0 = region.getBlock(checkPos);
                    Block const& space1 = region.getBlock(BlockPos{x, y + 1, z});
                    Block const& space2 = region.getBlock(BlockPos{x, y + 2, z});
                    if (!floor.isAir() && space0.isAir() && space1.isAir() && space2.isAir()) {
                        float dist = distVec.x * distVec.x + distVec.z * distVec.z;
                        if (closest < 0.0f || dist < closest) {
                            closest   = dist;
                            targetPos = checkPos;
                            dirTarget = dirOffs;
                        }
                        break;
                    }
                }
            }
        }
    }

    int xInc = dirTarget % 2;
    int zInc = 1 - dirTarget % 2;
    if (dirTarget % 4 >= 2) {
        xInc = -xInc;
        zInc = dirTarget % 2 - 1;
    }

    PortalAxis axis = PortalAxis::Z;
    if (std::abs(xInc) == 1) axis = PortalAxis::X;

    if (closest < 0.0f) {
        targetPos.y = std::clamp(targetPos.y, PORTAL_MIN_Y, PORTAL_MAX_Y);

        auto obsidian  = Block::tryGetFromRegistry("minecraft:obsidian");
        auto air       = Block::tryGetFromRegistry("minecraft:air");
        auto netherrack = Block::tryGetFromRegistry("minecraft:netherrack");

        if (!obsidian || !air || !netherrack) return origin(entity, radius);

        for (int b = -1; b <= 1; b++) {
            for (int s = 1; s < 3; s++) {
                for (int h = -1; h < 3; h++) {
                    BlockPos cur{
                        zInc * b + xInc * (s - 1) + targetPos.x,
                        h + targetPos.y,
                        zInc * (s - 1) + targetPos.z - xInc * b
                    };
                    if (h >= 0)
                        region.setBlockAndRetainCompatibleBlockActor(cur, *air, 3);
                    else
                        region.setBlockAndRetainCompatibleBlockActor(cur, *obsidian, 3);
                }
            }
        }

        for (int l = 0; l < 5; l++) {
            for (int s = 0; s < 4; s++) {
                int tx = -2 * zInc + l * zInc + s * xInc + targetPos.x - xInc;
                int tz = -2 * xInc + s * zInc + l * xInc + targetPos.z - zInc;
                if ((l != 0 && l != 4) || (s != 0 && s != 3)) {
                    Block const& floorBlock = region.getBlock(BlockPos{tx, targetPos.y - 1, tz});
                    if (floorBlock.isAir()) {
                        region.setBlockAndRetainCompatibleBlockActor(
                            BlockPos{tx, targetPos.y - 1, tz}, *netherrack, 3
                        );
                    }
                }
            }
        }
    }

    auto obsidian = Block::tryGetFromRegistry("minecraft:obsidian");
    auto portal   = Block::tryGetFromRegistry("minecraft:portal");
    if (!obsidian || !portal) return origin(entity, radius);

    Block const& portalWithAxis = portal->getStateFromLegacyData(axis == PortalAxis::X ? 1 : 2);

    for (int s = 0; s < 4; s++) {
        for (int h = -1; h < 4; h++) {
            BlockPos cur{
                xInc * (s - 1) + targetPos.x,
                h + targetPos.y,
                zInc * (s - 1) + targetPos.z
            };
            bool isFrame = (s == 0 || s == 3 || h == -1 || h == 3);
            if (isFrame)
                region.setBlockAndRetainCompatibleBlockActor(cur, *obsidian, 2);
            else
                region.setBlockAndRetainCompatibleBlockActor(cur, portalWithAxis, 2);
        }
    }

    for (int s = 0; s < 4; s++) {
        for (int h = -1; h < 4; h++) {
            BlockPos cur{
                xInc * (s - 1) + targetPos.x,
                h + targetPos.y,
                zInc * (s - 1) + targetPos.z
            };
            region.updateNeighborsAt(cur);
        }
    }

    PortalShape newShape;
    newShape.evaluate(targetPos, region);

    PortalRecord record;
    record.mBaseBlockPos = newShape.mBottomLeft;
    record.mSpan         = (schar)newShape.mWidth;
    record.mXInc         = (schar)xInc;
    record.mZInc         = (schar)zInc;

    static PortalRecord sLastRecord;
    sLastRecord = record;
    return sLastRecord;
}
LL_TYPE_INSTANCE_HOOK /*NOLINT*/ (
    LoopbackPacketSenderHook0,
    HookPriority::Normal,
    LoopbackPacketSender,
    &LoopbackPacketSender::$sendToClient,
    void,
    ::NetworkIdentifier const& id,
    ::Packet const&            packet,
    ::SubClientId              recipientSubId
) {
    patchPacket(packet.getId(), const_cast<Packet&>(packet));
    auto player = ll::service::getServerNetworkHandler()->_getServerPlayer(id, recipientSubId);
    if (player == nullptr) return origin(id, packet, recipientSubId);
    if (player && player->getDimensionId() == VanillaDimensions::Nether()
        && packet.getId() == MinecraftPacketIds::FullChunkData) {
        auto& pk = (LevelChunkPacket&)packet;
        if (pk.mDimensionId == VanillaDimensions::Nether()) {
            pk.mDimensionId = VanillaDimensions::TheEnd();
        }
        if (pk.mClientRequestSubChunkLimit <= 8) {
            pk.mClientRequestSubChunkLimit = 11;
        }
    }
    origin(id, packet, recipientSubId);
}
LL_TYPE_INSTANCE_HOOK /*NOLINT*/ (
    LoopbackPacketSenderHook1,
    HookPriority::Normal,
    LoopbackPacketSender,
    &LoopbackPacketSender::$sendToClient,
    void,
    ::UserEntityIdentifierComponent const* userIdentifier,
    ::Packet const&                        packet
) {
    patchPacket(packet.getId(), const_cast<Packet&>(packet));
    auto player = ll::service::getServerNetworkHandler()->_getServerPlayer(
        userIdentifier->mNetworkId,
        userIdentifier->mClientSubId
    );
    if (player == nullptr) return origin(userIdentifier, packet);
    if (packet.getId() == MinecraftPacketIds::ChangeDimension) {
        auto& pk = (ChangeDimensionPacket&)packet;
        if (pk.mDimensionId == VanillaDimensions::Nether()) {
            pk.mDimensionId = VanillaDimensions::TheEnd();
        }
    }
    origin(userIdentifier, packet);
}
LL_TYPE_INSTANCE_HOOK /*NOLINT*/ (
    LoopbackPacketSenderHook2,
    HookPriority::Normal,
    LoopbackPacketSender,
    &LoopbackPacketSender::$sendToClients,
    void,
    ::std::vector<::NetworkIdentifierWithSubId> const& ids,
    ::Packet const&                                    packet
) {
    patchPacket(packet.getId(), const_cast<Packet&>(packet));
    origin(ids, packet);
}
LL_TYPE_INSTANCE_HOOK /*NOLINT*/ (
    DimensionCtorHook,
    HookPriority::Normal,
    Dimension,
    &Dimension::$ctor,
    void*,
    ::DimensionArguments&& args
) {
    if (args.mDimId == VanillaDimensions::Nether()) args.mHeightRange->mMax = 256;
    return origin(std::move(args));
}
void sendEmptyChunk(const NetworkIdentifier& netId, int chunkX, int chunkZ, bool forceUpdate) {
    std::array<uchar, 4096> biome{};
    LevelChunkPacket        levelChunkPacket;
    BinaryStream            binaryStream{levelChunkPacket.mSerializedChunk, false};
    VarIntDataOutput        varIntDataOutput(binaryStream);

    varIntDataOutput.writeBytes(&biome, 4096); // write void biome
    for (int i = 1; i <= 8; i++) {
        varIntDataOutput.writeByte(255ui8);
    }
    varIntDataOutput.mStream.writeByte(0, "Byte", 0); // write border blocks

    levelChunkPacket.mPos->x         = chunkX;
    levelChunkPacket.mPos->z         = chunkZ;
    levelChunkPacket.mDimensionId    = VanillaDimensions::Overworld();
    levelChunkPacket.mCacheEnabled   = false;
    levelChunkPacket.mSubChunksCount = 0;

    ll::service::getLevel()->getPacketSender()->sendToClient(netId, levelChunkPacket, SubClientId::PrimaryClient);

    if (forceUpdate) {
        UpdateBlockPacket blockPacket;
        blockPacket.mPos         = BlockPos{(float)(chunkX << 4), 80.f, (float)(chunkZ << 4)};
        blockPacket.mLayer       = 0;
        blockPacket.mUpdateFlags = 1;
        ll::service::getLevel()->getPacketSender()->sendToClient(netId, blockPacket, SubClientId::PrimaryClient);
    }
}
void sendEmptyChunks(const NetworkIdentifier& netId, const Vec3& position, int radius, bool forceUpdate) {
    int chunkX = (int)(position.x) >> 4;
    int chunkZ = (int)(position.z) >> 4;
    for (int x = -radius; x <= radius; x++) {
        for (int z = -radius; z <= radius; z++) {
            sendEmptyChunk(netId, chunkX + x, chunkZ + z, forceUpdate);
        }
    }
}
void fakeChangeDimension(
    const NetworkIdentifier& netId,
    ActorRuntimeID           runtimeId,
    DimensionType            fakeDimId,
    const Vec3&              pos,
    std::optional<uint>      screedId
) {
    ChangeDimensionPacket changeDimensionPacket;
    changeDimensionPacket.mDimensionId     = fakeDimId;
    changeDimensionPacket.mPos             = pos;
    changeDimensionPacket.mRespawn         = true;
    changeDimensionPacket.mLoadingScreenId = {screedId};
    ll::service::getLevel()->getPacketSender()->sendToClient(netId, changeDimensionPacket, SubClientId::PrimaryClient);
    PlayerActionPacket playerActionPacket;
    playerActionPacket.mAction    = PlayerActionType::ChangeDimensionAck;
    playerActionPacket.mRuntimeId = runtimeId;
    ll::service::getLevel()->getPacketSender()->sendToClient(netId, playerActionPacket, SubClientId::PrimaryClient);
    sendEmptyChunks(netId, pos, 3, true);
}
LL_TYPE_INSTANCE_HOOK /*NOLINT*/ (
    BuildSubChunkPacketDataHook,
    HookPriority::Normal,
    ServerNetworkHandler,
    &ServerNetworkHandler::_buildSubChunkPacketData,
    void,
    const ::NetworkIdentifier&     source,
    const ::ServerPlayer*          player,
    const ::SubChunkRequestPacket& packet,
    ::SubChunkPacket&              responsePacket,
    uint                           requestCount,
    bool
) {
    const_cast<SubChunkRequestPacket&>(packet).mDimensionType = player->getDimensionId();
    origin(source, player, packet, responsePacket, requestCount, false);
}
LL_TYPE_INSTANCE_HOOK /*NOLINT*/ (
    SpawnParticleEffectPacketCtorHook,
    HookPriority::Normal,
    SpawnParticleEffectPacket,
    &SpawnParticleEffectPacket::$ctor,
    void*,
    ::SpawnParticleEffectPacketPayload payload
) {
    if (payload.mVanillaDimensionId == VanillaDimensions::Nether()) {
        payload.mVanillaDimensionId = static_cast<uchar>(VanillaDimensions::TheEnd());
    }
    return origin(std::move(payload));
}
LL_TYPE_INSTANCE_HOOK /*NOLINT*/ (
    RequestPlayerChangeDimensionHook,
    HookPriority::Normal,
    Level,
    &Level::$requestPlayerChangeDimension,
    void,
    ::Player&                  player,
    ::ChangeDimensionRequest&& changeRequest
) {
    if (changeRequest.mToDimensionId == VanillaDimensions::Nether()
        || (changeRequest.mFromDimensionId == VanillaDimensions::Nether()
            && changeRequest.mToDimensionId == VanillaDimensions::TheEnd())) {
        auto loadingScreenIdManager = ll::memory::dAccess<LoadingScreenIdManager*>(&this->mLoadingScreenIdManager, 8);
        auto screenId               = loadingScreenIdManager->mLastLoadingScreenId + 1;
        ++loadingScreenIdManager->mLastLoadingScreenId;
        fakeChangeDimension(
            player.getNetworkIdentifier(),
            player.getRuntimeID(),
            VanillaDimensions::Overworld(),
            player.getPosition(),
            screenId
        );
    }
    return origin(player, std::move(changeRequest));
}
LL_TYPE_INSTANCE_HOOK /*NOLINT*/ (
    LevelInitializeHook,
    HookPriority::Normal,
    Level,
    &Level::$initialize,
    bool,
    std::string const&                           levelName,
    LevelSettings const&                         levelSettings,
    Experiments const&                           experiments,
    std::string const*                           levelId,
    std::optional<std::reference_wrapper<BiomeDataMap>> biomeIdToResolvedData
) {
    mClientSideChunkGenEnabled = false;
    return origin(levelName, levelSettings, experiments, levelId, biomeIdToResolvedData);
}
#ifdef LL_PLAT_S
LL_TYPE_INSTANCE_HOOK /*NOLINT*/ (
    PropertiesSettingsCtorHook,
    HookPriority::Normal,
    PropertiesSettings,
    &PropertiesSettings::$ctor,
    void*
) {
    auto res = origin();
    reinterpret_cast<PropertiesSettings*>(res)->mClientSideGenerationEnabled = false;
    return res;
}
#endif
LL_TYPE_INSTANCE_HOOK /*NOLINT*/ (
    StartGamePacketCtorHook,
    HookPriority::Normal,
    StartGamePacket,
    &StartGamePacket::$ctor,
    void*,
    ::LevelSettings const&                                                     settings,
    ::ActorUniqueID                                                            entityId,
    ::ActorRuntimeID                                                           runtimeId,
    ::GameType                                                                 entityGameType,
    bool                                                                       enableItemStackNetManager,
    ::Vec3 const&                                                              pos,
    ::Vec2 const&                                                              rot,
    ::std::string const&                                                       levelId,
    ::std::string const&                                                       levelName,
    ::ContentIdentity const&                                                   premiumTemplateContentIdentity,
    ::std::string const&                                                       multiplayerCorrelationId,
    ::BlockDefinitionGroup const&                                              blockDefinitionGroup,
    bool                                                                       isTrial,
    ::CompoundTag                                                              playerPropertyData,
    ::PlayerMovementSettings const&                                            movementSettings,
    ::std::string const&                                                       serverVersion,
    ::mce::UUID const&                                                         worldTemplateId,
    ::std::optional<::ServerConfiguration::ServerConfigurationJoinInfo> const& serverJoinInfo,
    ::Social::Events::ServerTelemetryData const&                               serverTelemetryData,
    uint64                                                                     levelCurrentTime,
    int                                                                        enchantmentSeed,
    uint64                                                                     blockTypeRegistryChecksum
) {
    if (settings.mSpawnSettings->dimension == VanillaDimensions::Nether()) {
        const_cast<ll::TypedStorage<sizeof(DimensionType), alignof(DimensionType), ::DimensionType>&>(
            settings.mSpawnSettings->dimension
        ) = VanillaDimensions::TheEnd();
    }
    return origin(
        settings,
        entityId,
        runtimeId,
        entityGameType,
        enableItemStackNetManager,
        pos,
        rot,
        levelId,
        levelName,
        premiumTemplateContentIdentity,
        multiplayerCorrelationId,
        blockDefinitionGroup,
        isTrial,
        playerPropertyData,
        movementSettings,
        serverVersion,
        worldTemplateId,
        serverJoinInfo,
        serverTelemetryData,
        levelCurrentTime,
        enchantmentSeed,
        blockTypeRegistryChecksum
    );
}
} // namespace

struct FuckNetherHeightHooks::Impl {
    ll::memory::HookRegistrar<
        LoopbackPacketSenderHook0,
        LoopbackPacketSenderHook1,
        LoopbackPacketSenderHook2,
        DimensionCtorHook,
        BuildSubChunkPacketDataHook,
        SpawnParticleEffectPacketCtorHook,
        RequestPlayerChangeDimensionHook,
        StartGamePacketCtorHook,
        LevelInitializeHook,
        PortalForcerCreatePortalHook
#ifdef LL_PLAT_S
        ,
        PropertiesSettingsCtorHook
#endif
        >
        hooks;
};
FuckNetherHeightHooks::FuckNetherHeightHooks() : pImpl(std::make_unique<Impl>()) {}
FuckNetherHeightHooks::~FuckNetherHeightHooks() = default;
FuckNetherHeightHooks& FuckNetherHeightHooks::getInstance() {
    static FuckNetherHeightHooks hooks;
    return hooks;
}

MolangScriptArg::MolangScriptArg() : mType(MolangScriptArgType::Float), mPOD() {}