#include "ClientPlayer.h"

#include "PlayerMemoryAccess.h"

#include "../../../memory/ScatterReadBatch.h"
#include "../../SDK/EftOffsets.h"
#include "../MainGame.h"

#include <cmath>
#include <unordered_set>

namespace
{
    constexpr std::chrono::milliseconds corpseReadInterval{ 100 };
    constexpr std::chrono::milliseconds handsReadInterval{ 2000 };
    constexpr std::chrono::milliseconds failedReadRetryInterval{ 2500 };

    bool isBossRoleId(int roleId)
    {
        static const std::unordered_set<int> bossRoleIds
        {
            2, 3, 4, 5, 6, 7, 8, 11, 12, 13, 14, 15, 16, 17, 22, 23, 26, 27, 28, 29, 30,
            32, 33, 36, 41, 42, 43, 44, 45, 47, 65, 66, 67,
        };

        return bossRoleIds.contains(roleId);
    }

    bool isPmcRoleId(int roleId)
    {
        static const std::unordered_set<int> pmcRoleIds{ 9, 51, 52 };
        return pmcRoleIds.contains(roleId);
    }

    std::string readStringField(uint64_t address, int maximumLength = 128)
    {
        uint64_t stringPointer = 0;

        if (!PlayerMemoryAccess::tryReadPointer(address, stringPointer))
            return {};

        return PlayerMemoryAccess::readString(stringPointer, maximumLength);
    }

    std::string sideToString(EPlayerSide side)
    {
        switch (side)
        {
        case EPlayerSide::Usec: return "Usec";
        case EPlayerSide::Bear: return "Bear";
        case EPlayerSide::Savage: return "Savage";
        default: return "Unknown";
        }
    }

    void classifyPvePlayer(Player& player, const std::string& nickname)
    {
        const bool isSavage =
            (static_cast<uint32_t>(player.playerSide) &
                static_cast<uint32_t>(EPlayerSide::Savage)) != 0;

        player.isBlackDivision = false;
        player.isCultist = false;

        if (isBossRoleId(player.roleId))
        {
            player.name = nickname.empty() ? "Boss" : nickname;
            player.type = PlayerType::AIBoss;
            player.isBoss = true;
            player.isAi = true;
            player.isPlayer = false;
            player.isPlayerScav = false;
            return;
        }

        if (isPmcRoleId(player.roleId) || !isSavage)
        {
            player.name = nickname.empty()
                ? "PMC " + std::to_string(mainGame.pmcNumber++)
                : nickname;
            player.type = PlayerType::PMC;
            player.isBoss = false;
            player.isAi = false;
            player.isPlayer = true;
            player.isPlayerScav = false;
            return;
        }

        if (player.isAi)
        {
            player.name = nickname.empty() ? "Scav" : nickname;
            player.type = PlayerType::AIScav;
            player.isBoss = false;
            player.isPlayer = false;
            player.isPlayerScav = false;
            return;
        }

        player.name = nickname.empty()
            ? "PScav " + std::to_string(mainGame.pmcNumber++)
            : nickname;
        player.type = PlayerType::PScav;
        player.isBoss = false;
        player.isAi = false;
        player.isPlayer = true;
        player.isPlayerScav = true;
    }
}

PlayerKind ClientPlayer::getKind() const noexcept
{
    return PlayerKind::Client;
}

bool ClientPlayer::matches(std::string_view className, bool isLocal) const noexcept
{
    return !isLocal && (className == "ClientPlayer" || className == "LocalPlayer");
}

uint64_t ClientPlayer::getHeldItemOffset() const noexcept
{
    return sdk::ItemHandsController::Item;
}

void ClientPlayer::initialize(Player& player) const noexcept
{
    player.isLocal = false;
    player.setKind(getKind());
}

void ClientPlayer::configureControllerAddresses(Player& player) const noexcept
{
    player.P_InventoryControllerAddr = player.instance + sdk::Player::_inventoryController;
    player.P_HandsControllerAddr = player.instance + sdk::Player::_handsController;
}

bool ClientPlayer::tryResolveBoneMatrix(const Player& player, uint64_t& matrixPointer) const
{
    return PlayerMemoryAccess::tryReadChain(player.instance, { sdk::Player::_playerBody, 0x30, 0x30, 0x10 }, matrixPointer, DmaCacheMode::Uncached);
}

std::optional<Player> ClientPlayer::tryCreate(uint64_t instance, std::string_view className) const
{
    if (!Utils::valid_pointer(instance) || (className != "ClientPlayer" && className != "LocalPlayer"))
        return std::nullopt;

    Player player{};
    player.instance = instance;
    player.className = className;
    initialize(player);
    initializeSnapshot(player);

    if (player.isLocal)
        player.name = "LocalPlayer";
    else
    {
        player.name = "Ai";
        player.isAi = true;
		player.type = PlayerType::AIScav;
    }

    PlayerMemoryAccess::tryReadChain(instance, { sdk::Player::_playerBody, 0x30, 0x30, 0x10 }, player.playerBoneMatrixPtr, DmaCacheMode::Uncached);
    player.P_CorpseAddr = instance + sdk::Player::Corpse;

    if (!PlayerMemoryAccess::tryReadPointer(instance + sdk::Player::Profile, player.P_Profile) ||
        !PlayerMemoryAccess::tryReadPointer(player.P_Profile + sdk::Profile::Info, player.P_Info) ||
        !PlayerMemoryAccess::tryReadPointer(instance + sdk::Player::_playerBody, player.P_Body))
    {
        return std::nullopt;
    }

    PlayerMemoryAccess::tryReadPointer(instance + sdk::Player::ProceduralWeaponAnimation, player.P_PWA);
    configureControllerAddresses(player);

    if (!PlayerMemoryAccess::tryRead(player.P_Info + sdk::PlayerInfo::Side, player.playerSide) ||
        !PlayerMemoryAccess::tryReadPointer(instance + sdk::Player::MovementContext, player.P_MovementContext))
    {
        return std::nullopt;
    }

    player.side = sideToString(player.playerSide);

    if (!player.isLocal)
    {
        player.profileId = readStringField(player.P_Profile + sdk::Profile::Id, 64);
        player.accountId = readStringField(player.P_Profile + sdk::Profile::AccountId, 64);

        const std::string nickname =
            readStringField(player.P_Info + sdk::PlayerInfo::Nickname, 128);

        uint64_t settingsPointer = 0;

        if (PlayerMemoryAccess::tryReadPointer(
            player.P_Info + sdk::PlayerInfo::Settings,
            settingsPointer))
        {
            int roleId = player.roleId;

            if (PlayerMemoryAccess::tryRead(
                settingsPointer + sdk::ProfileSettings::Role,
                roleId))
            {
                player.roleId = roleId;
            }
        }

        classifyPvePlayer(player, nickname);
    }

    player.P_RotationAddress = player.P_MovementContext + sdk::MovementContext::_rotation;

    uint64_t characterController = 0;

    if (PlayerMemoryAccess::tryReadPointer(instance + sdk::Player::CharacterController, characterController))
        player.P_VelocityAddress = characterController + sdk::SimpleCharacterController::Velocity;

    if (!Utils::valid_pointer(player.P_CorpseAddr) ||
        !Utils::valid_pointer(player.P_InventoryControllerAddr) ||
        !Utils::valid_pointer(player.P_HandsControllerAddr) ||
        !Utils::valid_pointer(player.P_RotationAddress))
    {
        return std::nullopt;
    }

    return player;
}

void ClientPlayer::prepareRefresh(const Player& player, PlayerRuntimeRead& read, const PlayerRefreshContext& context) const
{
    read = {};
    read.instance = player.instance;
    read.kind = player.getKind();
    read.rotationAddress = player.P_RotationAddress;
    read.corpseAddress = player.P_CorpseAddr;
    read.handsControllerAddress = player.P_HandsControllerAddr;
    read.proceduralWeaponAnimation = player.P_PWA;
    read.velocityAddress = player.P_VelocityAddress;
    read.isLocal = player.isLocal;
    read.rotationRaw = player.rotationRAW;
    read.velocity = player.velocity;
    read.corpseClass = player.P_CorpseClass;
    read.handsController = player.P_HandsController;
    read.isAiming = player.isAiming;
    read.corpseDue = player.nextCorpseRead == std::chrono::steady_clock::time_point{} || context.now >= player.nextCorpseRead;
    read.handsDue = player.nextHandsControllerRead == std::chrono::steady_clock::time_point{} || context.now >= player.nextHandsControllerRead;
}

void ClientPlayer::queueRefresh(ScatterReadBatch& batch, PlayerRuntimeRead& read, const PlayerRefreshContext& context) const
{
    read.rotationQueued = batch.AddBytes(read.rotationAddress, &read.rotationRaw, sizeof(glm::vec2));

    if (context.predictionEnabled && !read.isLocal && Utils::valid_pointer(read.velocityAddress))
        read.velocityQueued = batch.Add(read.velocityAddress, read.velocity);

    if (read.corpseDue)
        read.corpseQueued = batch.Add(read.corpseAddress, read.corpseClass);

    if (read.handsDue)
        read.handsQueued = batch.Add(read.handsControllerAddress, read.handsController);

    read.aimingQueued = batch.Add(read.proceduralWeaponAnimation + sdk::ProceduralWeaponAnimation::_isAiming, read.isAiming);
}

void ClientPlayer::applyRefresh(Player& player, const PlayerRuntimeRead& read, bool executed, const PlayerRefreshContext& context) const
{
    if (executed)
    {
        if (read.rotationQueued)
            player.rotationRAW = read.rotationRaw;

        if (read.velocityQueued)
        {
            const float speed = glm::length(read.velocity);
            const bool velocityValid = std::isfinite(read.velocity.x) && std::isfinite(read.velocity.y) && std::isfinite(read.velocity.z) && std::isfinite(speed) && speed >= 0.1f && speed <= 15.0f;
            player.velocity = velocityValid ? read.velocity : glm::vec3{};
            player.velocityValid = velocityValid;
            player.lastVelocityUpdate = context.now;
        }

        if (read.corpseQueued)
            player.P_CorpseClass = read.corpseClass;

        if (read.handsQueued)
            player.P_HandsController = read.handsController;

        if (read.aimingQueued)
            player.isAiming = read.isAiming;
    }

    if (read.corpseDue)
        player.nextCorpseRead = executed && read.corpseQueued ? context.now + corpseReadInterval : context.now + failedReadRetryInterval;

    if (read.handsDue)
        player.nextHandsControllerRead = executed && read.handsQueued ? context.now + handsReadInterval : context.now + failedReadRetryInterval;
}
