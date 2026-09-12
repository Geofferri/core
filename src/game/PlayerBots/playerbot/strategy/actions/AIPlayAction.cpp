#include "playerbot/playerbot.h"
#include "AIPlayAction.h"

#include "playerbot/PlayerbotAI.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/PlayerbotLLMInterface.h"
#include "playerbot/PlayerbotTextMgr.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/WorldPosition.h"
#include "playerbot/strategy/AiObjectContext.h"
#include "playerbot/strategy/NamedObjectContext.h"
#include "playerbot/strategy/actions/SayAction.h"
#include "playerbot/strategy/values/NearestGameObjects.h"
#include "World.h"
#include "ObjectAccessor.h"
#include "Group.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <random>
#include <thread>

using namespace ai;

bool AIPlayMoveToRequesterAction::Execute(Event& event)
{
    // Move once toward the player who asked; this does not install the persistent
    // follow movement generator or alter the bot's follow/stay strategies.
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester || requester == bot || !requester->IsInWorld() ||
        requester->GetMapId() != bot->GetMapId() || requester->GetInstanceId() != bot->GetInstanceId())
    {
        return false;
    }

    const float followDistance = ai->GetRange("follow");
    if (sServerFacade.GetDistance2d(bot, requester) <= followDistance)
    {
        return true;
    }

    return MoveNear(requester, followDistance);
}

bool AIPlayMoveRandomAction::Execute(Event&)
{
    const uint32 randnum = urand(1, 2000);
    const float pi = 3.14159265358979323846f;
    const float angle = pi * (float)randnum / 1000.0f;
    const float distance = (float)urand(20, 200);

    return MoveTo(bot->GetMapId(), bot->GetPositionX() + std::cos(angle) * distance,
        bot->GetPositionY() + std::sin(angle) * distance, bot->GetPositionZ());
}

bool AIPlayStopAttackAction::Execute(Event&)
{
    if (!bot)
        return false;

    bot->AttackStop();
    return true;
}

namespace
{
    struct KeywordIntent
    {
        const char* phrase;
        const char* action;
    };

    enum class CandidateKind
    {
        KEYWORD,
        ACTION
    };

    struct IntentCandidate
    {
        size_t position;
        size_t phraseLength;
        CandidateKind kind;
        const KeywordIntent* keyword;
        std::string name;
    };

    void AddKeywordAliases(std::vector<KeywordIntent>& intents, std::initializer_list<const char*> aliases,
        const char* action)
    {
        for (const char* alias : aliases)
            intents.push_back({ alias, action });
    }

    std::string NormalizeKeywords(const std::string& input)
    {
        std::string result;
        result.reserve(input.size());
        bool lastSpace = true;

        for (unsigned char ch : input)
        {
            if (std::isalnum(ch))
            {
                result.push_back((char)std::tolower(ch));
                lastSpace = false;
            }
            else if (ch == '\'')
            {
                // Keep contractions such as "don't" as one token.
            }
            else if (ch == '.' || ch == '!' || ch == '?' || ch == ';' || ch == '\n')
            {
                if (!lastSpace)
                    result.push_back(' ');
                result += "| ";
                lastSpace = true;
            }
            else if (!lastSpace)
            {
                result.push_back(' ');
                lastSpace = true;
            }
        }

        if (!result.empty() && result.back() == ' ')
            result.pop_back();
        return result;
    }

    bool FindKeyword(const std::string& normalizedText, const std::string& phrase, size_t& position)
    {
        std::string normalizedPhrase = NormalizeKeywords(phrase);
        if (normalizedPhrase.empty())
            return false;

        std::string paddedText = " " + normalizedText + " ";
        std::string paddedPhrase = " " + normalizedPhrase + " ";
        size_t found = paddedText.find(paddedPhrase);
        if (found == std::string::npos)
            return false;

        position = found - 1;
        return true;
    }

    bool IsNegatedBefore(const std::string& normalizedText, size_t position, bool includeStop)
    {
        std::string prefix = normalizedText.substr(0, position);
        size_t sentenceStart = prefix.rfind('|');
        if (sentenceStart != std::string::npos)
            prefix = prefix.substr(sentenceStart + 1);
        size_t start = prefix.size() > 48 ? prefix.size() - 48 : 0;
        prefix = prefix.substr(start);

        static const char* negations[] = { "not ", "never ", "dont ", "didnt ", "doesnt ", "isnt ", "wasnt ", "cannot ", "cant ", "wont ", "wouldnt ", "shouldnt ", "couldnt ", "no longer " };
        for (const char* negation : negations)
            if (prefix.find(negation) != std::string::npos)
                return true;

        return includeStop && prefix.find("stop ") != std::string::npos;
    }



    const std::vector<KeywordIntent>& GetKeywordIntents()
    {
        static const std::vector<KeywordIntent> intents = []()
        {
            std::vector<KeywordIntent> result = {
                { "stop following", "stop follow" },
                { "stopped following", "stop follow" },
                { "stop wandering", "stop follow" },
                { "stopped wandering", "stop follow" },
                { "stop moving", "stop follow" },
                { "stay here", "stop follow" },
                { "wait here", "stop follow" },
                { "hold position", "stop follow" },
                { "come here", "ai play move to requester" },
                { "come over here", "ai play move to requester" },
                { "get over here", "ai play move to requester" },
                { "get over to me", "ai play move to requester" },
                { "move to me", "ai play move to requester" },
                { "move toward me", "ai play move to requester" },
                { "move towards me", "ai play move to requester" },
                { "meet me", "ai play move to requester" },
                { "catch up", "ai play move to requester" },
                { "rejoin me", "ai play move to requester" },
                { "follow me", "ai play move to requester" },
                { "come along", "ai play move to requester" },
                { "come with", "ai play move to requester" },
                { "attack player", "attack enemy player" },
                { "attack enemy", "attack my target" },
                { "attack target", "attack my target" },
                { "kill target", "attack my target" },
                { "kill it", "attack my target" },
                { "take cover", "flee" },
                { "run away", "flee" },
                { "back off", "flee" },
                { "toward enemy", "attack my target" },
                { "approach enemy", "attack my target" },
                { "open fire", "attack my target" },
                { "meet up", "ai play move to requester" },
                { "head out", "ai play move random" },
                { "heal me", "rpg heal" },
                { "heal group", "rpg heal" },
                { "help me", "attack my target" },
                { "protect me", "tank assist" },
                { "stay close", "ai play move to requester" },
                { "free roam", "ai play move random" },
                { "stop combat", "ai play stop attack" },
                { "cease fire", "ai play stop attack" },
                { "attack", "attack my target" },
                { "attacks", "attack my target" },
                { "attacking", "attack my target" },
                { "kill", "attack my target" },
                { "kills", "attack my target" },
                { "killing", "attack my target" },
                { "fight", "attack my target" },
                { "fights", "attack my target" },
                { "fighting", "attack my target" },
                { "engage", "attack my target" },
                { "engages", "attack my target" },
                { "engaging", "attack my target" },
                { "slay", "attack my target" },
                { "charge", "attack my target" },
                { "enemy", "attack my target" },
                { "follow", "ai play move to requester" },
                { "follows", "ai play move to requester" },
                { "following", "ai play move to requester" },
                { "come", "ai play move to requester" },
                { "join", "ai play move to requester" },
                { "stay", "stop follow" },
                { "wait", "stop follow" },
                { "hold", "stop follow" },
                { "wander", "ai play move random" },
                { "wanders", "ai play move random" },
                { "wandering", "ai play move random" },
                { "roam", "ai play move random" },
                { "explore", "ai play move random" },
                { "travel", "ai play move random" },
                { "travels", "ai play move random" },
                { "traveling", "ai play move random" },
                { "return", "return" },
                { "attack", "attack my target" },
                { "kill", "attack my target" },
                { "fight", "attack my target" },
                { "engage", "attack my target" },
                { "defend", "attack my target" },
                { "flee", "flee" },
                { "retreat", "flee" },
                { "escape", "flee" },
                { "heal", "rpg heal" },
                { "heals", "rpg heal" },
                { "healing", "rpg heal" },
                { "buff", "buff" },
                { "boost", "buff" },
                { "cure", "cure poison on party" },
                { "cleanse", "cure poison on party" },
                { "dispel", "cure poison on party" },
                { "interrupt", "counterspell" },
                { "tank", "tank assist" },
                { "assist", "dps assist" },
                { "ranged", "switch to ranged" },
                { "melee", "switch to melee" },
                { "kite", "move out of enemy contact" },
                { "aoe", "dps aoe" },
                { "crowd control", "polymorph" },
                { "mount", "mount" },
                { "dismount", "dismount" },
                { "loot", "loot" },
                { "loots", "loot" },
                { "drink", "drink" },
                { "drinks", "drink" },
                { "thirsty", "drink" },
                { "eat", "food" },
                { "eats", "food" },
                { "hungry", "food" },
                { "healthstone", "healthstone" },
                { "potion", "healing potion" },
                { "quest", "rpg" },
                { "quests", "rpg" },
                { "gather", "add gathering loot" },
                { "mine", "add gathering loot" },
                { "herb", "add gathering loot" },
                { "fish", "fish" },
                { "craft", "rpg craft" },
                { "repair", "rpg repair" },
                { "vendor", "rpg" },
                { "buy", "rpg buy" },
                { "sell", "rpg sell" },
                { "auction", "rpg" },
                { "mail", "rpg get mail" },
                { "train", "rpg train" },
                { "duel", "rpg duel" },
                { "pvp", "attack enemy player" },
                { "battleground", "free bg join" },
                { "wave", "emote::wave" },
                { "dance", "emote::dance" },
                { "laugh", "emote::laugh" },
                { "cheer", "emote::cheer" },
                { "salute", "emote::salute" },
                { "bow", "emote::bow" },
                { "applaud", "emote::applaud" },
                { "beg", "emote::beg" },
                { "chicken", "emote::chicken" },
                { "cry", "emote::cry" },
                { "eat", "emote::eat" },
                { "flex", "emote::flex" },
                { "kick", "emote::kick" },
                { "kiss", "emote::kiss" },
                { "kneel", "emote::kneel" },
                { "point", "emote::point" },
                { "question", "emote::question" },
                { "rude", "emote::rude" },
                { "shout", "emote::shout" },
                { "shy", "emote::shy" },
                { "sleep", "emote::sleep" },
                { "hello", "emote::hello" },
                { "bye", "emote::bye" },
                { "thank", "emote::thank" },
                { "help", "emote::help" },
                { "hug", "emote::hug" },
                { "flirt", "emote::flirt" },
                { "silly", "emote::silly" },
                { "joke", "emote::joke" },
                { "welcome", "emote::welcome" },
                { "whistle", "emote::whistle" },
                { "yawn", "emote::yawn" },
                { "no", "emote::no" },
                { "yes", "emote::yes" },
                { "roar", "emote::roar" },
                { "land", "emote::land" },
                { "liftoff", "emote::liftoff" },
                { "wound", "emote::wound" },
                { "train", "emote::train" },
                { "bored", "emote::bored" },
                { "congrats", "emote::congratulate" },
                { "nod", "emote::nod" },
                { "sigh", "emote::sigh" },
                { "introduce", "emote::introduce" },
                { "wave", "emote::wave" },
                { "dance", "emote::dance" },
                { "laugh", "emote::laugh" },
                { "cheer", "emote::cheer" },
                { "salute", "emote::salute" },
                { "bow", "emote::bow" },
                { "applaud", "emote::applaud" },
                { "cry", "emote::cry" },
                { "roar", "emote::roar" },
                { "greet", "greet" },
                { "hello", "greet" },
                { "greetings", "greet" },
                { "talk", "talk" },
                { "sit", "sit" },
                { "jump", "jump" },
                { "mount", "mount" },
                { "pet", "initialize pet" },
                { "stealth", "stealth" },
                { "no war", "ai play stop attack" },
                { "silent", nullptr },
                { "food", "food" },
                { "rest", "food" },
                { "rpg", "rpg" },
                { "group", "ai play move to requester" },
                { "invite group", "invite nearby" },
                { "leave group", "leave" },
                { "release", "release" },
                { "unstuck", "unstuck" },
                { "", nullptr }
            };

            // Try class-appropriate abilities for broad support intents.
            AddKeywordAliases(result, { "cure", "cleanse", "dispel" }, "cure disease on party");
            AddKeywordAliases(result, { "cure", "cleanse", "dispel" }, "abolish poison on party");
            AddKeywordAliases(result, { "cure", "cleanse", "dispel" }, "abolish disease on party");
            AddKeywordAliases(result, { "cure", "cleanse", "dispel" }, "remove curse on party");
            AddKeywordAliases(result, { "cure", "cleanse", "dispel" }, "dispel magic on party");
            AddKeywordAliases(result, { "cure", "cleanse", "dispel" }, "cleanse poison on party");
            AddKeywordAliases(result, { "cure", "cleanse", "dispel" }, "cleanse disease on party");
            AddKeywordAliases(result, { "cure", "cleanse", "dispel" }, "cleanse magic on party");
            AddKeywordAliases(result, { "cure", "cleanse", "dispel" }, "purify poison on party");
            AddKeywordAliases(result, { "cure", "cleanse", "dispel" }, "purify disease on party");
            AddKeywordAliases(result, { "cure", "cleanse", "dispel" }, "cleanse spirit poison on party");
            AddKeywordAliases(result, { "cure", "cleanse", "dispel" }, "cleanse spirit disease on party");
            AddKeywordAliases(result, { "cure", "cleanse", "dispel" }, "cleanse spirit curse on party");

            AddKeywordAliases(result, { "interrupt", "interrupt spell" }, "kick");
            AddKeywordAliases(result, { "interrupt", "interrupt spell" }, "pummel");
            AddKeywordAliases(result, { "interrupt", "interrupt spell" }, "shield bash");
            AddKeywordAliases(result, { "interrupt", "interrupt spell" }, "wind shear");
            AddKeywordAliases(result, { "interrupt", "interrupt spell" }, "spell lock");
            AddKeywordAliases(result, { "interrupt", "interrupt spell" }, "earth shock");
            AddKeywordAliases(result, { "interrupt", "interrupt spell" }, "hammer of justice");
            AddKeywordAliases(result, { "stop casting", "cancel my cast" }, "interrupt current spell");

            AddKeywordAliases(result, { "crowd control", "cc", "control the target", "disable the enemy" }, "freezing trap");
            AddKeywordAliases(result, { "crowd control", "cc", "control the target", "disable the enemy" }, "hibernate");
            AddKeywordAliases(result, { "crowd control", "cc", "control the target", "disable the enemy" }, "shackle undead");
            AddKeywordAliases(result, { "crowd control", "cc", "control the target", "disable the enemy" }, "fear");
            AddKeywordAliases(result, { "crowd control", "cc", "control the target", "disable the enemy" }, "banish");
            AddKeywordAliases(result, { "crowd control", "cc", "control the target", "disable the enemy" }, "hammer of justice");
            AddKeywordAliases(result, { "crowd control", "cc", "control the target", "disable the enemy" }, "repentance");
            AddKeywordAliases(result, { "crowd control", "cc", "control the target", "disable the enemy" }, "blind");
            AddKeywordAliases(result, { "crowd control", "cc", "control the target", "disable the enemy" }, "gouge");
            AddKeywordAliases(result, { "crowd control", "cc", "control the target", "disable the enemy" }, "kidney shot");
            AddKeywordAliases(result, { "crowd control", "cc", "control the target", "disable the enemy" }, "intimidating shout");
            AddKeywordAliases(result, { "crowd control", "cc", "control the target", "disable the enemy" }, "scatter shot");
            AddKeywordAliases(result, { "crowd control", "cc", "control the target", "disable the enemy" }, "wyvern sting");

            // Every AI-play keyword dispatches one finite action; no strategy is changed.
            AddKeywordAliases(result, {
                    "always follow", "always follow me", "keep following", "keep following me",
                    "continue following", "follow me from now on", "stay with me", "stay by my side",
                    "stick with me", "keep close to me", "stay close to me", "remain by my side",
                    "at your side", "stay together", "stick together", "keep together"
                }, "ai play move to requester");

            AddKeywordAliases(result, {
                    "always attack", "keep attacking", "continue attacking", "keep fighting",
                    "continue fighting", "always fight", "stay aggressive", "keep focus on my target"
                }, "dps assist");

            // Natural language has many surface forms for the same intent. These
            // aliases deliberately include verbs, inflections, role language, and
            // casual phrasing, while execution remains gated by native bot actions.
            AddKeywordAliases(result, {
                    "follow", "follows", "following", "followed", "come", "join", "joining",
                    "accompany", "accompanies", "accompanying", "escort", "escorting", "trail", "trailing",
                    "tail", "tailing", "behind", "behind you", "beside", "beside you", "alongside",
                    "alongside you", "your side", "keep up", "keeping up", "stick", "sticking",
                    "stick close", "sticking close", "walking", "walk with", "marching", "come along",
                    "coming along", "come with", "go with", "going with", "travel with", "meeting",
                    "with you", "right behind", "your heels"
                }, "ai play move to requester");

            AddKeywordAliases(result, {
                    "coming", "moving", "move", "moving closer", "move closer", "approaching me",
                    "walking over", "walk over", "run over", "get over here", "come over here",
                    "come here", "come to me", "get to me", "head to me", "head over here",
                    "moving toward you", "moving towards you", "move toward me", "move towards me",
                    "coming over", "on my way", "i am coming", "i'm coming", "be right there",
                    "i will be right there", "i'll be right there", "meet me", "meet up", "catch up",
                    "rejoin me", "join me", "join us", "come closer", "come right over"
                }, "ai play move to requester");

            AddKeywordAliases(result, {
                    "stay", "staying", "wait", "waiting", "hold", "holding", "halt", "pause", "freeze",
                    "stationary", "remain", "remaining", "idle", "guard", "guarding", "watch", "watching",
                    "stand", "standing", "park", "camp", "anchor", "hold position", "stay put", "wait here",
                    "stand still", "hold still", "remain here", "keep watch", "stand guard", "stay back",
                    "stay behind", "stay nearby", "stop", "stopping",
                    "cover", "covering", "overwatch", "hold ground", "hold fast"
                }, "stop follow");

            AddKeywordAliases(result, {
                    "wander", "wandering", "roam", "roaming", "explore", "exploring", "scout", "scouting",
                    "patrol", "patrolling", "adventure", "adventuring", "venture", "venturing",
                    "trek", "trekking", "hike", "hiking", "traverse", "traversing", "sightsee",
                    "sightseeing", "look around", "seek",
                    "seeking", "discover", "discovering", "exploration", "recon", "reconnoiter",
                    "go exploring", "scout ahead", "search around"
                }, "ai play move random");

            AddKeywordAliases(result, {
                    "travel", "travels", "traveling", "travelling", "journey", "journeying", "depart",
                    "departing", "leave", "leaving", "advance", "advancing", "proceed", "proceeding",
                    "continue", "continuing", "embark", "embarking", "head out", "set out", "move on",
                    "moving on", "press on", "ride out", "travel onward", "go onward", "head north",
                    "head south", "head east", "head west", "venture forth", "make tracks", "get going"
                }, "ai play move random");

            AddKeywordAliases(result, {
                    "attack", "attacks", "attacking", "fight", "fights", "fighting", "battle", "battling",
                    "combat", "engage", "engages", "engaging", "kill", "kills", "killing", "slay", "slays",
                    "slaying", "destroy", "destroying", "defeat", "defeating", "strike", "striking", "hit",
                    "hitting", "shoot", "shooting", "fire", "firing", "charge", "charging", "rush", "rushing",
                    "assault", "assaulting", "ambush", "ambushing", "hunt", "hunting", "pull", "pulling",
                    "provoke", "provoking", "execute", "executing", "finish", "finish off",
                    "move toward", "move towards", "moving toward", "moving towards", "approaching enemy", "approach target",
                    "take down", "bring down", "dispatch", "eliminate", "smash", "crush", "annihilate", "destroy",
                    "go in", "get them", "focus fire", "open fire", "strike back", "defend us", "target enemy",
                    "enemy", "enemies", "hostile", "hostiles", "foe", "foes", "opponent", "opponents"
                }, "attack my target");

            AddKeywordAliases(result, {
                    "flee", "fleeing", "retreat", "retreating", "escape", "escaping", "withdraw", "withdrawing",
                    "evade", "evading", "dodge", "dodging", "avoid", "avoiding", "disengage", "disengaging",
                    "run away", "back away", "back off", "fall back", "pull back", "get away", "break away",
                    "move away", "moving away", "move back", "moving back", "scatter", "scattering", "survive", "surviving",
                    "take cover", "retire", "withdraw", "keep distance", "stay clear", "give ground", "retreat now"
                }, "flee");

            AddKeywordAliases(result, {
                    "heal", "heals", "healing", "healer", "mend", "mending", "restore", "restoring", "recover",
                    "recovering", "patch", "patching", "tend", "tending", "stabilize",
                    "stabilizing", "aid", "aiding", "help", "helping", "support", "supporting", "rescue", "rescuing",
                    "revive", "reviving", "resurrect", "resurrecting", "raise", "raising", "rez", "recovery",
                    "heal up", "top off", "restore health", "treat", "treating", "mend wounds",
                    "patch up", "save", "saving", "keep alive", "bring back", "revitalize", "renew"
                }, "rpg heal");

            AddKeywordAliases(result, {
                    "buff", "buffs", "buffing", "bless", "blessing", "blessings", "strengthen", "strengthening",
                    "fortify", "fortifying", "empower", "empowering", "boosting", "enhance", "enhancing",
                    "protect", "protection", "shield", "shielding", "ward", "warding", "prepare", "preparing",
                    "prepare us", "bolster", "bolstering", "aura", "auras", "power up",
                    "gear up", "make stronger"
                }, "buff");

            AddKeywordAliases(result, {
                    "tank", "tanking", "tank it", "hold aggro", "take aggro", "draw aggro", "taunt", "taunting",
                    "protect tank", "protect group", "defend group", "guard group", "take point", "frontline",
                    "front line", "stand between", "keep safe", "bodyguard", "bodyguarding"
                }, "tank assist");

            AddKeywordAliases(result, {
                    "damage", "damaging", "dps", "damage dealer", "deal damage", "hit hard", "go offensive",
                    "offensive", "offense", "focus target", "focus", "assist", "assisting", "help attack",
                    "target focus", "burn target", "burst", "bursting"
                }, "dps assist");

            AddKeywordAliases(result, {
                    "ranged", "ranging", "range", "distance", "distant", "afar", "from afar", "at range",
                    "keep range", "stay ranged", "snipe", "sniping", "archer", "archery", "longbow", "crossbow",
                    "gun", "guns"
                }, "switch to ranged");

            AddKeywordAliases(result, {
                    "melee", "meleeing", "close", "closer", "up close", "close in", "close range", "get close",
                    "get closer", "engage close", "close combat", "move in", "moving in", "charge in", "rush in"
                }, "switch to melee");

            AddKeywordAliases(result, {
                    "kite", "kiting", "kite them", "keep moving", "run circles", "circle them", "stay mobile",
                    "hit run", "moving attack", "backpedal", "backpedaling", "maintain distance"
                }, "move out of enemy contact");

            AddKeywordAliases(result, {
                    "cast", "casting", "spell", "spells", "spellcasting", "ability", "abilities", "skill",
                    "skills", "power", "powers", "magic", "magical", "cast something", "use spell",
                    "use ability"
                }, "cast random spell");

            AddKeywordAliases(result, {
                    "mount", "mounting", "mounted", "ride", "riding", "saddle", "saddling", "steed", "horse",
                    "mount up", "get mounted", "ride faster", "travel mounted"
                }, "mount");

            AddKeywordAliases(result, {
                    "dismount", "dismounting", "unmount", "unmounting", "on foot", "get down", "leave mount"
                }, "dismount");

            AddKeywordAliases(result, {
                    "loot", "looting", "plunder", "plundering", "scavenge", "scavenging", "collect", "collecting",
                    "claim", "claiming", "pick up", "pickup", "salvage", "salvaging", "loot corpse", "collect loot",
                    "take loot", "claim spoils", "gather spoils", "take treasure", "collect treasure"
                }, "loot");

            AddKeywordAliases(result, {
                    "gather", "gathering", "harvest", "harvesting", "mine", "mining", "herb", "herbalism", "skin",
                    "skinning", "forage", "foraging", "salvage materials", "collect herbs",
                    "gather herbs", "gather ore", "gather materials", "harvest plants", "pick herbs", "mine ore"
                }, "add gathering loot");

            AddKeywordAliases(result, {
                    "fish", "fishing", "angler", "angling", "cast line", "reel in", "catch fish", "go fishing"
                }, "fish");

            AddKeywordAliases(result, {
                    "talk", "talking", "speak", "speaking", "chat", "chatting", "converse", "conversing", "gossip",
                    "ask", "asking", "inquire", "inquiring", "question", "questioning", "hailing",
                    "approach npc", "speak to", "talk to", "start conversation", "speak with",
                    "address", "addressing", "consult", "consulting", "interview", "interviewing"
                }, "talk");

            AddKeywordAliases(result, {
                    "interact", "interacting", "click", "clicking", "activate", "activating", "open", "opening",
                    "inspect", "inspecting", "examine", "examining", "investigate", "investigating", "use object",
                    "open dialog", "open dialogue", "browse options", "choose option"
                }, "gossip hello");

            AddKeywordAliases(result, {
                    "use", "using", "activate item", "use item"
                }, "use consumable");

            AddKeywordAliases(result, {
                    "quest", "quests", "questing", "objective", "objectives", "task", "tasks", "mission", "missions",
                    "accept", "accepting", "complete", "completing", "abandon",
                    "abandoning", "quest share",
                    "deliver", "delivering", "do quest", "questwork"
                }, "rpg");

            AddKeywordAliases(result, {
                    "accept quest", "accepting quest", "take quest", "start quest", "begin quest"
                }, "accept quest");

            AddKeywordAliases(result, {
                    "complete quest", "finish quest", "deliver quest",
                    "report quest", "claim reward", "choose reward", "quest reward", "select reward",
                    "show rewards", "list rewards", "reward options"
                }, "talk to quest giver");

            AddKeywordAliases(result, { "turn in", "hand in" }, "talk to quest giver");
            AddKeywordAliases(result, { "share quest", "share it" }, "share");
            AddKeywordAliases(result, { "abandon quest", "drop quest" }, "drop");

            AddKeywordAliases(result, {
                    "craft", "crafting", "create", "creating", "forge", "forging", "smith",
                    "smithing", "cook", "cooking", "brew", "brewing", "alchemy", "alchemist", "enchant", "enchanting",
                    "smelt", "smelting", "prospect", "prospecting", "disenchant", "disenchanting", "tailor", "tailoring",
                    "leatherwork", "leatherworking", "engineering", "craft item", "make item", "create item", "use recipe",
                    "learn recipe", "profession", "professions"
                }, "rpg craft");

            AddKeywordAliases(result, {
                    "buying", "purchase", "purchasing", "shop", "shopping", "selling", "trading",
                    "barter", "auction", "auctioning", "bid", "bidding", "vendor", "merchant", "repairing",
                    "fix", "fixing", "training", "trainer", "learn", "learning", "mailing", "post",
                    "posting", "send mail", "check mail", "collect mail", "buy gear", "sell items", "repair gear",
                    "visit vendor", "visit trainer", "browse wares", "purchase supplies", "restock"
                }, "rpg");

            AddKeywordAliases(result, {
                    "group", "grouping", "party", "partying", "raid", "raiding", "guild", "guilding", "form group",
                    "form party", "form raid", "join group", "join party", "join raid", "regroup", "rally", "assemble",
                    "assembly", "gather group", "follow leader", "stick together", "stay together", "move together",
                    "split up", "separate", "formation", "team up", "squad up", "make party", "make group"
                }, "ai play move to requester");

            AddKeywordAliases(result, {
                    "invite", "inviting", "recruit", "recruiting", "invite party", "invite group", "invite nearby",
                    "add player", "bring in", "group invite", "party invite"
                }, "invite nearby");

            AddKeywordAliases(result, {
                    "leave", "leaving", "depart group", "drop group", "disband", "leave group", "leave party",
                    "leave raid", "quit group", "exit group", "dismiss group"
                }, "leave");

            AddKeywordAliases(result, {
                    "ready", "ready check", "check readiness", "stand ready", "prepare group"
                }, "ready check");

            AddKeywordAliases(result, {
                    "hi", "hey", "greetings", "salutations", "hail", "welcoming", "say hello", "say hi", "greet warmly"
                }, "greet");
            AddKeywordAliases(result, { "smile", "smiling", "grin", "grinning" }, "emote::smile");
            AddKeywordAliases(result, { "chuckle", "chuckling", "giggle", "giggling", "snicker", "snickering" }, "emote::laugh");
            AddKeywordAliases(result, { "cheering", "celebrate", "celebrating", "hooray", "rejoice" }, "emote::cheer");
            AddKeywordAliases(result, { "clap", "clapping", "applauding", "applause" }, "emote::applaud");
            AddKeywordAliases(result, { "saluting", "render salute" }, "emote::salute");
            AddKeywordAliases(result, { "bowing", "curtsy" }, "emote::bow");
            AddKeywordAliases(result, { "crying", "sob", "sobbing", "weep", "weeping" }, "emote::cry");
            AddKeywordAliases(result, { "sighing", "yawn", "yawning" }, "emote::sigh");
            AddKeywordAliases(result, { "flexing", "show muscles", "show off" }, "emote::flex");
            AddKeywordAliases(result, { "pointing", "indicate", "gesture" }, "emote::point");
            AddKeywordAliases(result, { "nodding", "agree", "agreement" }, "emote::nod");
            AddKeywordAliases(result, { "shrug", "shrugging", "uncertain", "confused" }, "emote::question");
            AddKeywordAliases(result, { "blushing", "hugging", "embrace" }, "emote::hug");
            AddKeywordAliases(result, { "kissing", "smooch" }, "emote::kiss");
            AddKeywordAliases(result, { "flirting", "charm", "flirtatious" }, "emote::flirt");
            AddKeywordAliases(result, { "whistling", "catcall" }, "emote::whistle");
            AddKeywordAliases(result, { "roaring", "bellow", "bellowing" }, "emote::roar");
            AddKeywordAliases(result, { "dancing", "boogie", "groove" }, "emote::dance");
            AddKeywordAliases(result, { "thank", "thanks", "say thanks", "thankful" }, "emote::thank");
            AddKeywordAliases(result, { "goodbye", "farewell", "bye", "see you" }, "emote::bye");
            AddKeywordAliases(result, { "congratulate", "congratulations", "congrats", "well done" }, "emote::congratulate");

            AddKeywordAliases(result, {
                    "eat", "eating", "hungry", "starving", "snack", "snacking", "feast", "feasting", "dine", "dining",
                    "food", "meal", "consume food", "eat up", "munch", "munching"
                }, "food");

            AddKeywordAliases(result, {
                    "drink", "drinking", "thirsty", "parched", "sip", "sipping", "hydrate", "hydrating", "quench",
                    "water", "drink up", "restore mana", "refresh", "gulp", "gulping"
                }, "drink");

            AddKeywordAliases(result, {
                    "rest", "resting", "replenish", "take break", "recover mana", "recover health", "sit down"
                }, "food");

            AddKeywordAliases(result, { "bandage", "bandaging", "apply bandage", "use bandage" }, "use bandage");
            AddKeywordAliases(result, { "healthstone", "stone", "use stone", "use healthstone" }, "healthstone");
            AddKeywordAliases(result, { "potion", "potions", "use potion", "take potion", "drink potion", "heal potion" }, "healing potion");

            AddKeywordAliases(result, { "pet", "petting", "call pet", "summon pet" }, "initialize pet");
                AddKeywordAliases(result, { "call pet", "summon pet" }, "call pet");
                AddKeywordAliases(result, { "dismiss pet" }, "dismiss pet");
                AddKeywordAliases(result, { "send pet", "pet attack" }, "attack my target");
                AddKeywordAliases(result, { "pet follow" }, "ai play move to requester");
                AddKeywordAliases(result, { "pet stay" }, "stop follow");
                AddKeywordAliases(result, { "feed pet" }, "feed pet");
                AddKeywordAliases(result, { "revive pet" }, "revive pet");
                AddKeywordAliases(result, { "heal pet", "mend pet" }, "mend pet");

            AddKeywordAliases(result, {
                    "stealth", "sneak", "sneaking", "hide", "hiding", "invisible", "vanish", "vanishing", "stalk",
                    "stalking", "sneak past", "move unseen", "stay hidden"
                }, "stealth");

            AddKeywordAliases(result, { "dueling", "challenge", "challenging" }, "rpg duel");
                AddKeywordAliases(result, { "pvp", "fight player", "enemy player", "war", "battlefield" }, "attack enemy player");
                AddKeywordAliases(result, { "battlegrounds", "bg", "arena", "arenas", "join battleground", "enter battleground" }, "free bg join");

            result.erase(std::remove_if(result.begin(), result.end(), [](const KeywordIntent& intent)
            {
                return !intent.phrase || !*intent.phrase;
            }), result.end());

            return result;
        }();

        return intents;
    }

    bool ApplyIntent(PlayerbotAI* ai, const KeywordIntent& intent, const std::string& source, Player* owner)
    {
        if (!intent.action || !ai->CanDoSpecificAction(intent.action, true, true))
            return false;

        Event event("ai play", source, owner);
        return ai->DoSpecificAction(intent.action, event, true);
    }

    bool IsRawMovementActionBlocked(const std::string& action)
    {
        return action == "follow" || action == "stay" || action == "wander" ||
            action == "move random" || action == "reset" || action == "reset ai" ||
            action == "reset strats" || action == "change strategy";
    }

    bool ApplyActionName(PlayerbotAI* ai, const std::string& action, const std::string& normalized,
        const std::string& source, Player* owner)
    {
        if (action == "ai play" || IsRawMovementActionBlocked(action))
            return false;

        size_t position = 0;
        if (!FindKeyword(normalized, action, position) || IsNegatedBefore(normalized, position, true))
            return false;

        if (!ai->GetAiObjectContext()->GetAction(action))
            return false;

        if (!ai->CanDoSpecificAction(action, true, true))
            return false;

        Event event("ai play", source, owner);
        return ai->DoSpecificAction(action, event, true);
    }



    std::string JoinStrings(const std::vector<std::string>& strings)
    {
        std::string result;
        for (const std::string& value : strings)
        {
            if (value.empty())
                continue;
            if (!result.empty())
                result += " ";
            result += value;
        }
        return result;
    }

    std::string GetNearbySight(PlayerbotAI* ai)
    {
        AiObjectContext* context = ai->GetAiObjectContext();
        Player* bot = ai->GetBot();
        std::vector<std::pair<ObjectGuid, bool>> visible;
        std::set<ObjectGuid> added;

        const char* playerLists[] = { "nearest non bot players", "nearest friendly players" };
        for (const char* valueName : playerLists)
        {
            Value<std::list<ObjectGuid>>* value = context->GetValue<std::list<ObjectGuid>>(valueName);
            if (!value)
                continue;

            for (ObjectGuid guid : value->Get())
            {
                if (added.insert(guid).second)
                    visible.emplace_back(guid, false);
            }
        }

        const char* creatureLists[] = { "nearest npcs", "possible targets" };
        for (const char* valueName : creatureLists)
        {
            Value<std::list<ObjectGuid>>* value = context->GetValue<std::list<ObjectGuid>>(valueName);
            if (!value)
                continue;

            for (ObjectGuid guid : value->Get())
            {
                if (added.insert(guid).second)
                    visible.emplace_back(guid, false);
            }
        }

        Value<std::list<ObjectGuid>>* gameObjects = context->GetValue<std::list<ObjectGuid>>("nearest game objects");
        if (gameObjects)
        {
            for (ObjectGuid guid : gameObjects->Get())
            {
                if (added.insert(guid).second)
                    visible.emplace_back(guid, true);
            }
        }

        if (visible.empty())
            return "No nearby players, creatures, or objects are visible.";

        static thread_local std::mt19937 generator(std::random_device{}());
        std::shuffle(visible.begin(), visible.end(), generator);
        size_t maxVisible = std::min<size_t>(3, visible.size());
        size_t selectedCount = urand(1, (uint32)maxVisible);

        std::vector<std::string> descriptions;
        for (size_t i = 0; i < selectedCount; ++i)
        {
            const ObjectGuid& guid = visible[i].first;
            if (visible[i].second)
            {
                GameObject* object = ai->GetGameObject(guid);
                if (object)
                    descriptions.push_back(std::string("object ") + object->GetName());
                continue;
            }

            Unit* unit = ai->GetUnit(guid);
            if (!unit)
                continue;

            std::string kind = unit->IsPlayer() ? "player" : "creature";
            if (unit->IsPlayer() && ai->IsRealPlayer(unit))
                kind = "real player";
            else if (sServerFacade.IsHostileTo(unit, bot))
                kind = "hostile creature";

            descriptions.push_back(kind + " " + unit->GetName() + " level " + std::to_string(unit->GetLevel()));
        }

        return descriptions.empty() ? "No nearby players, creatures, or objects are visible." : JoinStrings(descriptions);
    }

    uint32 NextControlInterval()
    {
        uint32 minimum = sPlayerbotAIConfig.llmControlMinInterval;
        uint32 maximum = sPlayerbotAIConfig.llmControlMaxInterval;
        if (maximum < minimum)
            maximum = minimum;
        return urand(minimum, maximum);
    }

    bool BuildAutonomousRequest(PlayerbotAI* ai, std::string& json, std::string& startPattern,
        std::string& endPattern, std::string& deletePattern, std::string& splitPattern)
    {
        Player* bot = ai->GetBot();
        AiObjectContext* context = ai->GetAiObjectContext();
        if (!bot || !context)
            return false;

        std::map<std::string, std::string> placeholders;
        ChatReplyAction::GetAIChatPlaceholders(placeholders, bot, bot);
        ChatReplyAction::GetAIChatPlaceholders(placeholders, bot, "bot", bot);
        placeholders["<other name>"] = "the world around you";
        placeholders["<other gender>"] = "unknown";
        placeholders["<other level>"] = "unknown";
        placeholders["<other class>"] = "unknown";
        placeholders["<other race>"] = "unknown";
        placeholders["<other type>"] = "your surroundings";
        placeholders["<channel name>"] = "while adventuring";
        placeholders["<initial message>"] = "What do you do next? Nearby: " + GetNearbySight(ai);

        std::string card;
        if (Value<std::string>* value = context->GetValue<std::string>("manual saved string::llmdefaultprompt"))
            card = value->Get();

        std::string previousContext = ai->GetAIPlayContext();

        std::map<std::string, std::string> jsonFill;
        jsonFill["<pre prompt>"] = PlayerbotTextMgr::GetReplacePlaceholders(sPlayerbotAIConfig.llmPrePrompt + " " + card, placeholders);
        jsonFill["<context>"] = previousContext;
        jsonFill["<prompt>"] = PlayerbotTextMgr::GetReplacePlaceholders(sPlayerbotAIConfig.llmPrompt, placeholders);
        jsonFill["<post prompt>"] = PlayerbotTextMgr::GetReplacePlaceholders(sPlayerbotAIConfig.llmPostPrompt, placeholders);

        uint32 fixedLength = jsonFill["<pre prompt>"].size() + jsonFill["<prompt>"].size() + jsonFill["<post prompt>"].size();
        PlayerbotLLMInterface::LimitContext(jsonFill["<context>"], fixedLength + jsonFill["<context>"].size());

        for (auto& field : jsonFill)
            field.second = PlayerbotLLMInterface::SanitizeForJson(field.second);

        for (auto& placeholder : placeholders)
            placeholder.second = PlayerbotLLMInterface::SanitizeForJson(placeholder.second);

        startPattern = PlayerbotTextMgr::GetReplacePlaceholders(sPlayerbotAIConfig.llmResponseStartPattern, placeholders);
        endPattern = PlayerbotTextMgr::GetReplacePlaceholders(sPlayerbotAIConfig.llmResponseEndPattern, placeholders);
        deletePattern = PlayerbotTextMgr::GetReplacePlaceholders(sPlayerbotAIConfig.llmResponseDeletePattern, placeholders);
        splitPattern = PlayerbotTextMgr::GetReplacePlaceholders(sPlayerbotAIConfig.llmResponseSplitPattern, placeholders);
        json = PlayerbotTextMgr::GetReplacePlaceholders(sPlayerbotAIConfig.llmApiJson, jsonFill);
        json = PlayerbotTextMgr::GetReplacePlaceholders(json, placeholders);
        return !json.empty();
    }
}

bool AIPlayAction::isUseful()
{
    if (!sPlayerbotAIConfig.llmEnabled || !ai->HasStrategy("ai play", BotState::BOT_STATE_NON_COMBAT))
        return false;

    if (sPlayerbotAIConfig.llmRequirePlayerPresence && !ai->HasRealPlayerNearbyOrInGroup())
        return false;

    return !ai->aiPlayGenerationPending && (!ai->nextAIPlayGenerationTime || time(nullptr) >= ai->nextAIPlayGenerationTime);
}

bool AIPlayAction::Execute(Event& event)
{
    (void)event;
    TryStartAutonomous(ai);
    return true;
}

void AIPlayAction::TryStartAutonomous(PlayerbotAI* ai)
{
    if (!ai)
        return;

    AIPlayAction* action = dynamic_cast<AIPlayAction*>(ai->GetAiObjectContext()->GetAction("ai play"));
    if (!action || !action->isUseful())
        return;

    time_t now = time(nullptr);
    if (!ai->nextAIPlayGenerationTime)
    {
        ai->nextAIPlayGenerationTime = now + NextControlInterval();
        return;
    }

    ai->aiPlayGenerationPending = true;
    ai->nextAIPlayGenerationTime = now + NextControlInterval();

    std::string json, startPattern, endPattern, deletePattern, splitPattern;
    if (!BuildAutonomousRequest(ai, json, startPattern, endPattern, deletePattern, splitPattern))
    {
        ai->aiPlayGenerationPending = false;
        return;
    }

    ObjectGuid botGuid = ai->GetBot()->GetObjectGuid();
    ObjectGuid ownerGuid = ai->GetMaster() ? ai->GetMaster()->GetObjectGuid() : ObjectGuid();
    std::thread([botGuid, ownerGuid, json, startPattern, endPattern, deletePattern, splitPattern]()
    {
        std::vector<std::string> debugLines;
        std::string response = PlayerbotLLMInterface::Generate(json, sPlayerbotAIConfig.llmGenerationTimeout,
            sPlayerbotAIConfig.llmMaxSimultaniousGenerations, debugLines);
        std::vector<std::string> lines = PlayerbotLLMInterface::ParseResponse(response, startPattern,
            endPattern, deletePattern, splitPattern, debugLines);
        std::string generatedText = JoinStrings(lines);
        if (generatedText.empty())
            generatedText = response;

        sWorld.GetMessager().AddMessage([botGuid, ownerGuid, generatedText](World*)
        {
            Player* bot = sObjectAccessor.FindPlayer(botGuid);
            if (bot && bot->IsInWorld() && bot->GetPlayerbotAI())
                bot->GetPlayerbotAI()->QueueAIPlayText(generatedText, true, ownerGuid);
        });
    }).detach();
}

bool AIPlayAction::ProcessPlayerMessage(PlayerbotAI* ai, uint32 type, ObjectGuid sender, ObjectGuid receiver, const std::string& text)
{
    (void)receiver;
    if (!ai || !ai->HasStrategy("ai play", BotState::BOT_STATE_NON_COMBAT) || !sPlayerbotAIConfig.llmEnabled)
        return false;

    if (sPlayerbotAIConfig.llmRequirePlayerPresence && !ai->HasRealPlayerNearbyOrInGroup())
        return false;

    Player* player = sObjectAccessor.FindPlayer(sender);
    if (!player || !player->IsInWorld() || !ai->IsRealPlayer(player))
        return false;

    Player* bot = ai->GetBot();
    bool addressed = type == CHAT_MSG_WHISPER;
    Group* group = bot->GetGroup();
    bool grouped = group && player->GetGroup() == group;
    std::string normalizedText = NormalizeKeywords(text);
    size_t mentionPosition = 0;
    bool mentioned = FindKeyword(normalizedText, bot->GetName(), mentionPosition);
    if (!addressed && !grouped && !mentioned)
        return false;

    if (!ai->aiPlayContext.empty())
        ai->aiPlayContext += "\n";
    ai->aiPlayContext += std::string(player->GetName()) + ": " + text;
    if (ai->aiPlayContext.size() > 32768)
        ai->aiPlayContext.erase(0, ai->aiPlayContext.size() - 32768);

    return ProcessGeneratedText(ai, text, false, player);
}

bool AIPlayAction::ProcessGeneratedText(PlayerbotAI* ai, const std::string& text, bool appendContext, Player* owner)
{
    if (!ai || text.empty() || !ai->HasStrategy("ai play", BotState::BOT_STATE_NON_COMBAT))
        return false;

    std::string normalized = NormalizeKeywords(text);
    if (normalized.empty())
        return false;

    if (appendContext && ai->GetBot())
    {
        if (!ai->aiPlayContext.empty())
            ai->aiPlayContext += "\n";
        ai->aiPlayContext += std::string(ai->GetBot()->GetName()) + ": " + text;
        if (ai->aiPlayContext.size() > 32768)
            ai->aiPlayContext.erase(0, ai->aiPlayContext.size() - 32768);
    }

    std::set<std::string> actions;
    ai->GetAiObjectContext()->GetSupportedActions(actions);
    std::set<std::string> keywordActions;
    for (const KeywordIntent& intent : GetKeywordIntents())
        if (intent.action)
            keywordActions.insert(intent.action);

    if (!owner)
        owner = ai->GetMaster();
    std::vector<IntentCandidate> candidates;
    for (const KeywordIntent& intent : GetKeywordIntents())
    {
        size_t position = 0;
        if (FindKeyword(normalized, intent.phrase, position) && !IsNegatedBefore(normalized, position, true))
            candidates.push_back({ position, std::string(intent.phrase).size(), CandidateKind::KEYWORD, &intent, "" });
    }

    for (const std::string& action : actions)
    {
        if (action == "ai play" || IsRawMovementActionBlocked(action) || !keywordActions.count(action))
            continue;

        size_t position = 0;
        if (FindKeyword(normalized, action, position) && !IsNegatedBefore(normalized, position, true))
            candidates.push_back({ position, action.size(), CandidateKind::ACTION, nullptr, action });
    }

    // Act on the first meaningful intent in the text, preferring a longer phrase
    // when multiple aliases begin at the same word. Exactly one finite action is
    // executed for each player message or LLM generation.
    std::stable_sort(candidates.begin(), candidates.end(), [](const IntentCandidate& left, const IntentCandidate& right)
    {
        if (left.position != right.position)
            return left.position < right.position;
        if (left.phraseLength != right.phraseLength)
            return left.phraseLength > right.phraseLength;
        return static_cast<int>(left.kind) < static_cast<int>(right.kind);
    });

    for (const IntentCandidate& candidate : candidates)
    {
        bool applied = false;
        if (candidate.kind == CandidateKind::KEYWORD && candidate.keyword)
            applied = ApplyIntent(ai, *candidate.keyword, text, owner);
        else if (candidate.kind == CandidateKind::ACTION)
            applied = ApplyActionName(ai, candidate.name, normalized, text, owner);

        if (applied)
            return true;
    }

    return false;
}

void AIPlayAction::QueueGeneratedResponse(ObjectGuid botGuid, ObjectGuid ownerGuid, const std::string& text)
{
    if (text.empty())
        return;

    sWorld.GetMessager().AddMessage([botGuid, ownerGuid, text](World*)
    {
        Player* bot = sObjectAccessor.FindPlayer(botGuid);
        if (bot && bot->IsInWorld() && bot->GetPlayerbotAI())
            bot->GetPlayerbotAI()->QueueAIPlayText(text, false, ownerGuid);
    });
}
