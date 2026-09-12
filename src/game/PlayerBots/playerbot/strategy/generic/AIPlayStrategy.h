#pragma once

#include "playerbot/strategy/Strategy.h"

namespace ai
{
    class AIPlayStrategy : public Strategy
    {
    public:
        AIPlayStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        int GetType() override { return STRATEGY_TYPE_NONCOMBAT; }
        std::string getName() override { return "ai play"; }

#ifdef GenerateBotHelp
        std::string GetHelpName() override { return "ai play"; }
        std::string GetHelpDescription() override
        {
            return "Lets the LLM influence the bot through recognized playerbot actions and strategies.";
        }
        std::vector<std::string> GetRelatedStrategies() override { return { "ai chat" }; }
#endif
    };
}
