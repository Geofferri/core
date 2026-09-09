
#include "playerbot/playerbot.h"
#include "PartyMemberToDispel.h"

#include "playerbot/ServerFacade.h"

#include <vector>

using namespace ai;

class PartyMemberToDispelPredicate : public FindPlayerPredicate, public PlayerbotAIAware
{
public:
    PartyMemberToDispelPredicate(PlayerbotAI* ai, uint32 dispelType) :
        PlayerbotAIAware(ai), FindPlayerPredicate(), dispelType(dispelType) {}
    virtual ~PartyMemberToDispelPredicate() noexcept = default;

public:
    virtual bool Check(Unit* unit) override
    {
        Pet* pet = dynamic_cast<Pet*>(unit);
        if (pet && (0 /* getPetType not in vmangos */ == MINI_PET || 0 /* getPetType not in vmangos */ == SUMMON_PET))
            return false;

        return sServerFacade.IsAlive(unit) && sServerFacade.GetDistance2d(ai->GetBot(), unit) <= ai->GetRange("spell") && ai->HasAuraToDispel(unit, dispelType);
    }

private:
    uint32 dispelType;
};

Unit* PartyMemberToDispel::Calculate()
{
    uint32 dispelType = atoi(qualifier.c_str());

    PartyMemberToDispelPredicate predicate(ai, dispelType);

    Group* group = bot->GetGroup();
    if (!group)
        return FindPartyMember(predicate);

    std::vector<Unit*> candidates;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* player = ref->getSource();

        if (!player || player == bot)
            continue;

        if (Check(player) && predicate.Check(player))
            candidates.push_back(player);

        Pet* pet = player->GetPet();
        if (pet && Check(pet) && predicate.Check(pet))
            candidates.push_back(pet);
    }

    if (candidates.empty())
        return nullptr;

    size_t index = static_cast<size_t>(bot->GetGUIDLow() % candidates.size());

    return candidates[index];
}
