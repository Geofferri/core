
#include "playerbot/playerbot.h"
#include "EnemyPlayerValue.h"
#include "TargetValue.h"

using namespace ai;

namespace
{
    uint32 GetPvpEngageChance(Player* bot, Player* target)
    {
        if (!bot || !target)
            return 0;

        // Positive = target is higher level than bot.
        // Negative = bot is higher level than target.
        int32 levelDiff = int32(target->GetLevel()) - int32(bot->GetLevel());

        // Same level: 95% chance to voluntarily engage.
        if (levelDiff == 0)
            return 95;

        // Bot is LOWER level than target.
        //
        // 1 below  = 85%
        // 2 below  = 75%
        // ...
        // 8 below  = 15%
        // 9 below  = 5%
        // 10+ below = 1%
        if (levelDiff > 0)
        {
            if (levelDiff >= 10)
                return 1;

            return 95 - uint32(levelDiff) * 10;
        }

        // Bot is HIGHER level than target.
        //
        // 1 above  = 90%
        // 2 above  = 85%
        // ...
        // 9 above  = 50%
        // 10+ above = 45%
        uint32 levelsAbove = uint32(-levelDiff);

        if (levelsAbove >= 10)
            return 45;

        return 95 - levelsAbove * 5;
    }

    bool IsDefendingAgainstPlayer(Player* bot, Player* target)
    {
        if (!bot || !target)
            return false;

        // Target is already attacking the bot.
        if (target->GetThreatManager().getThreat(bot) > 0.0f || target->GetVictim() == bot)
        {
            return true;
        }

        // Target is attacking the bot's pet.
        Pet* pet = bot->GetPet();
        if (pet)
        {
            if (target->GetThreatManager().getThreat(pet) > 0.0f || target->GetVictim() == pet)
            {
                return true;
            }
        }

        return false;
    }

    bool ShouldEngagePlayer(Player* bot, Player* target)
    {
        if (!bot || !target)
            return false;

        if (IsDefendingAgainstPlayer(bot, target))
            return true;

        uint32 engageChance = GetPvpEngageChance(bot, target);

        if (engageChance >= 100)
            return true;

        uint64 seed = bot->GetObjectGuid().GetRawValue();

        seed ^= target->GetObjectGuid().GetRawValue() + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);

        seed ^= seed >> 30;
        seed *= 0xbf58476d1ce4e5b9ULL;
        seed ^= seed >> 27;
        seed *= 0x94d049bb133111ebULL;
        seed ^= seed >> 31;

        uint32 roll = uint32(seed % 100ULL) + 1;

        return roll <= engageChance;
    }
}

std::list<ObjectGuid> EnemyPlayersValue::Calculate()
{
    std::list<ObjectGuid> result;
    if (ai->AllowActivity(ALL_ACTIVITY))
    {
        if (bot->IsInWorld() && !bot->IsBeingTeleported())
        {
            // Check if we only need one attacker
            bool getOne = false;
            if (!qualifier.empty())
            {
                getOne = std::stoi(qualifier);
            }

            if (getOne)
            {
                // Try to get one enemy target
                result = AI_VALUE2(std::list<ObjectGuid>, "possible attack targets", 1);
                ApplyFilter(result, getOne);
            }

            // If the one enemy player failed, retry with multiple possible attack targets
            if (result.empty())
            {
                result = AI_VALUE(std::list<ObjectGuid>, "possible attack targets");
                ApplyFilter(result, getOne);
            }
        }
    }

    return result;
}

bool EnemyPlayersValue::IsValid(Unit* target, Player* player)
{
    if (target)
    {
        // If the target is a player
        Player* enemyPlayer = dynamic_cast<Player*>(target);
        if (enemyPlayer)
        {
            // If the target is friendly to the player
            if (sServerFacade.IsFriendlyTo(target, player))
            {
                return false;
            }

            // Check that the target is not a mind controlled ally
            if (target->HasAuraType(SPELL_AURA_MOD_CHARM) || target->HasAuraType(SPELL_AURA_MOD_POSSESS))
            {
                if (player && (player->GetGroup() && player->GetGroup()->IsMember(target->GetObjectGuid())))
                {
                    return false;
                }
            }

            /*
            // Check if too far away (Do we need this?)
            const float maxPvPDistance = GetMaxAttackDistance(player);
            const bool inCannon = player->GetPlayerbotAI() && player->GetPlayerbotAI()->IsInVehicle(false, true);
            uint32 const pvpDistance = (inCannon || player->GetHealth() > enemyPlayer->GetHealth()) ? maxPvPDistance : 20.0f;
            if (!player->IsWithinDist(enemyPlayer, pvpDistance, false))
            {
                return false;
            }
            */

            return true;
        }
    }

    return false;
}

void EnemyPlayersValue::ApplyFilter(std::list<ObjectGuid>& targets, bool getOne)
{
    std::list<ObjectGuid> filteredTargets;

    for (const ObjectGuid& targetGuid : targets)
    {
        Unit* target = ai->GetUnit(targetGuid);

        if (!IsValid(target, bot))
            continue;

        Player* enemyPlayer = dynamic_cast<Player*>(target);
        if (!enemyPlayer)
            continue;

        if (!ShouldEngagePlayer(bot, enemyPlayer))
            continue;

        filteredTargets.push_back(target->GetObjectGuid());

        if (getOne)
            break;
    }

    targets = filteredTargets;
}

bool HasEnemyPlayersValue::Calculate()
{
    return !context->GetValue<std::list<ObjectGuid>>("enemy player targets", 1)->Get().empty();
}

Unit* EnemyPlayerValue::Calculate()
{
    // Prioritize the duel opponent
    if(bot->m_duel && bot->m_duel->opponent && !sServerFacade.IsFriendlyTo(bot->m_duel->opponent, bot))
    {
        return bot->m_duel->opponent;
    }

    Unit* bestEnemyPlayer = nullptr;
    std::list<ObjectGuid> enemyPlayers = AI_VALUE(std::list<ObjectGuid>, "enemy player targets");
    if (!enemyPlayers.empty())
    {
        const bool isMelee = !ai->IsRanged(bot);
        uint32 bestEnemyPlayerHealth = std::numeric_limits<uint32>::max();
        float bestEnemyPlayerDistance = std::numeric_limits<float>::max();
      
        // Use the first enemy player as a base
        Unit* firstTarget = ai->GetUnit(enemyPlayers.front());
        if (firstTarget)
        {
            bestEnemyPlayerDistance = firstTarget->GetDistance(bot);
            bestEnemyPlayerHealth = firstTarget->GetHealth();
            bestEnemyPlayer = firstTarget;
        }

        for (const ObjectGuid& targetGuid : enemyPlayers)
        {
            Unit* target = ai->GetUnit(targetGuid);
            if (target)
            {
                // Prioritize an enemy player if it has a battleground flag
                if ((bot->GetTeam() == HORDE && target->HasAura(23333)) ||
                    (bot->GetTeam() == ALLIANCE && target->HasAura(23335)))
                {
                    bestEnemyPlayer = target;
                    break;
                }

                if (isMelee)
                {
                    // Score best enemy player based on lowest distance
                    const float distanceToEnemyPlayer = target->GetDistance(bot);
                    if (distanceToEnemyPlayer < bestEnemyPlayerDistance)
                    {
                        bestEnemyPlayerDistance = distanceToEnemyPlayer;
                        bestEnemyPlayer = target;
                    }
                }
                else
                {
                    // Score best enemy player based on lowest health
                    const uint32 enemyPlayerHealth = target->GetHealth();
                    if (enemyPlayerHealth < bestEnemyPlayerHealth)
                    {
                        bestEnemyPlayerHealth = enemyPlayerHealth;
                        bestEnemyPlayer = target;
                    }
                }
            }
        }
    }

    return bestEnemyPlayer;
}


float EnemyPlayerValue::GetMaxAttackDistance(Player* bot)
{
    if (!bot->GetBattleGround())
        return 60.0f;

    if (bot->InBattleGround())
    {
        BattleGround* bg = bot->GetBattleGround();
        if (!bg)
            return 40.0f;

        BattleGroundTypeId bgType = bg->GetTypeID();

#ifdef MANGOSBOT_TWO
        if (bgType == BATTLEGROUND_RB)
            bgType = bg->GetTypeId(true);

        if (bgType == BATTLEGROUND_IC)
        {
            if (bot->GetPlayerbotAI()->IsInVehicle(false, true))
                return 120.0f;
        }
#endif
        if (bgType == BATTLEGROUND_AV)
        {
            bool strifeTime = bg->GetStartTime() < (uint32)(20 * MINUTE * IN_MILLISECONDS);
            return strifeTime ? 40.0f : 10.0f;
        }
    }

    return 40.0f;
}
