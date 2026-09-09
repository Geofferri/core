#pragma once

#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/Action.h"

namespace ai
{
    class AcceptResurrectAction : public Action {
    public:
        AcceptResurrectAction(PlayerbotAI* ai) : Action(ai, "accept resurrect") {}

        virtual bool Execute(Event& event) override
        {
            if (sServerFacade.IsAlive(bot) || !bot->IsRessurectRequested())
                return false;

            ObjectGuid guid = bot->GetResurrector();

            if (guid.IsEmpty())
                return false;

            WorldPacket packet(CMSG_RESURRECT_RESPONSE, 8 + 1);
            packet << guid;
            packet << uint8(1); // accept

            bot->GetSession()->HandleResurrectResponseOpcode(MakeTypedPacket<WorldPackets::Misc::ResurrectResponse>(packet));

            return true;
        }

        virtual bool isUseful() override { return !sServerFacade.IsAlive(bot) && bot->IsRessurectRequested(); }
        
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "accept resurrect"; } //Must equal iternal name
        virtual std::string GetHelpDescription()
        {
            return "This action clicks the accept button asked if it wants to be resurrected by a player.";
        }
        virtual std::vector<std::string> GetUsedActions() { return {}; }
        virtual std::vector<std::string> GetUsedValues() { return {}; }
#endif 
    };

}
