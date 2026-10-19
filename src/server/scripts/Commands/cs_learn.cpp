/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "CommandScript.h"
#include "Language.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerCommand.h"
#include "RBAC.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Trainer.h"
#include <algorithm>
#include <array>
#include <cctype>

using namespace Acore::ChatCommands;

class learn_commandscript : public CommandScript
{
public:
    learn_commandscript() : CommandScript("learn_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable learnAllMyCommandTable =
        {
            { "class",      HandleLearnAllMyClassCommand,      rbac::RBAC_PERM_COMMAND_LEARN_ALL_MY_CLASS, Console::No },
            { "pettalents", HandleLearnAllMyPetTalentsCommand, rbac::RBAC_PERM_COMMAND_LEARN_MY_PETTALENTS, Console::No },
            { "trainer",    HandleLearnAllMyTrainerSpellsCommand, rbac::RBAC_PERM_COMMAND_LEARN_ALL_MY_SPELLS, Console::No },
            { "talents",    HandleLearnAllMyTalentsCommand,    rbac::RBAC_PERM_COMMAND_LEARN_ALL_TALENTS, Console::No },
            { "quest",      HandleLearnAllMyQuestSpells,       rbac::RBAC_PERM_COMMAND_LEARN_ALL_MY_SPELLS, Console::No }
        };

        static ChatCommandTable learnAllCommandTable =
        {
            { "my",        learnAllMyCommandTable },
            { "gm",        HandleLearnAllGMCommand,            rbac::RBAC_PERM_COMMAND_LEARN_ALL_GM, Console::No },
            { "crafts",    HandleLearnAllCraftsCommand,        rbac::RBAC_PERM_COMMAND_LEARN_ALL_CRAFTS, Console::No },
            { "default",   HandleLearnAllDefaultCommand,       rbac::RBAC_PERM_COMMAND_LEARN_ALL_DEFAULT, Console::No },
            { "lang",      HandleLearnAllLangCommand,          rbac::RBAC_PERM_COMMAND_LEARN_ALL_LANG, Console::No },
            { "recipes",   HandleLearnAllRecipesCommand,       rbac::RBAC_PERM_COMMAND_LEARN_ALL_RECIPES, Console::No },
        };

        static ChatCommandTable learnCommandTable =
        {
            { "all",        learnAllCommandTable },
            { "profession", HandleLearnProfessionCommand,      rbac::RBAC_PERM_COMMAND_LEARN_ALL_RECIPES, Console::No },
            { "",           HandleLearnCommand,                rbac::RBAC_PERM_COMMAND_LEARN, Console::No }
        };

        static ChatCommandTable unlearnCommandTable =
        {
            { "profession", HandleUnLearnProfessionCommand,    rbac::RBAC_PERM_COMMAND_UNLEARN, Console::No },
            { "",           HandleUnLearnCommand,              rbac::RBAC_PERM_COMMAND_UNLEARN, Console::No }
        };

        static ChatCommandTable commandTable =
        {
            { "learn",   learnCommandTable },
            { "unlearn", unlearnCommandTable }
        };
        return commandTable;
    }

    struct ProfessionRank
    {
        std::string_view Key;       // as typed in the command, spaces removed
        std::string_view Name;
    };

    // In step order; a rank's skill cap is 75 per step (Apprentice 75 ... Grand Master 450)
    static constexpr std::array<ProfessionRank, 6> ProfessionRanks =
    { {
        { "apprentice",  "Apprentice" },
        { "journeyman",  "Journeyman" },
        { "expert",      "Expert" },
        { "artisan",     "Artisan" },
        { "master",      "Master" },
        { "grandmaster", "Grand Master" }
    } };

    // Lower-case ASCII with spaces and underscores dropped, so "First Aid", "first_aid" and "firstaid" compare equal
    static std::string NormalizeProfessionArg(std::string_view text)
    {
        std::string result;
        result.reserve(text.size());
        for (char c : text)
            if (c != ' ' && c != '_')
                result.push_back(char(std::tolower(static_cast<unsigned char>(c))));

        return result;
    }

    static char const* GetSkillName(ChatHandler* handler, SkillLineEntry const* skillInfo)
    {
        char const* name = skillInfo->name[handler->GetSessionDbcLocale()];
        return name && *name ? name : skillInfo->name[DEFAULT_LOCALE];
    }

    static SkillLineEntry const* FindProfessionSkill(ChatHandler* handler, std::string_view name)
    {
        if (name.empty())
            return nullptr;

        for (uint32 i = 1; i < sSkillLineStore.GetNumRows(); ++i)
        {
            SkillLineEntry const* skillInfo = sSkillLineStore.LookupEntry(i);
            if (!skillInfo || (skillInfo->categoryId != SKILL_CATEGORY_PROFESSION && skillInfo->categoryId != SKILL_CATEGORY_SECONDARY))
                continue;

            for (LocaleConstant locale : { handler->GetSessionDbcLocale(), DEFAULT_LOCALE })
            {
                char const* skillName = skillInfo->name[locale];
                if (skillName && *skillName && NormalizeProfessionArg(skillName) == name)
                    return skillInfo;
            }
        }

        return nullptr;
    }

    // The skill's rank spells (Apprentice, Journeyman, ...) indexed by step - 1. Only the player rank chain is kept,
    // leaving out other spells that grant the same skill.
    static std::vector<uint32> GetProfessionRankSpells(uint32 skillId)
    {
        std::vector<uint32> const skillSpells = sSpellMgr->GetSkillRankSpells(skillId); // sorted by step

        uint32 firstRank = 0;
        for (uint32 spellId : skillSpells)
        {
            if (sSpellMgr->GetSpellLearnSkill(spellId)->step == 1 && sSpellMgr->GetNextSpellInChain(spellId))
            {
                firstRank = spellId;
                break;
            }
        }

        std::vector<uint32> ranks;
        for (uint32 spellId : skillSpells)
        {
            if (sSpellMgr->GetSpellLearnSkill(spellId)->step != ranks.size() + 1)
                continue;

            if (firstRank && sSpellMgr->GetFirstSpellInChain(spellId) != firstRank)
                continue;

            ranks.push_back(spellId);
        }

        return ranks;
    }

    // A trainer entry either is the spell itself or a teaching spell that learns it
    static std::vector<uint32> GetTrainerTaughtSpells(Trainer::Spell const& trainerSpell)
    {
        std::vector<uint32> taught;
        if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(trainerSpell.SpellId))
            for (SpellEffectInfo const& effect : spellInfo->GetEffects())
                if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && effect.TriggerSpell)
                    taught.push_back(effect.TriggerSpell);

        if (taught.empty())
            taught.push_back(trainerSpell.SpellId);

        return taught;
    }

    static bool IsSkillLineSpell(uint32 spellId, uint32 skillId)
    {
        SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        return std::any_of(bounds.first, bounds.second, [skillId](auto const& itr) { return itr.second->SkillLine == skillId; });
    }

    // Teaches every trainer recipe of the skill needing at most `cap` skill whose required abilities the player knows,
    // so specialisation recipes need the specialisation. Rank spells are left to the caller. Repeats until nothing new
    // is learned, so a recipe requiring another recipe is still taught. Returns the number of spells learned.
    static uint32 LearnTrainerRecipes(Player* player, uint32 skillId, uint16 cap)
    {
        uint32 learned = 0;
        bool progress = true;
        while (progress)
        {
            progress = false;
            for (auto const& [trainerId, trainer] : sObjectMgr->GetTrainers())
            {
                for (Trainer::Spell const& trainerSpell : trainer.GetSpells())
                {
                    if (trainerSpell.ReqSkillLine != skillId || trainerSpell.ReqSkillRank > cap)
                        continue;

                    if (std::any_of(trainerSpell.ReqAbility.begin(), trainerSpell.ReqAbility.end(),
                        [player](uint32 ability) { return ability && !player->HasSpell(ability); }))
                        continue;

                    for (uint32 spellId : GetTrainerTaughtSpells(trainerSpell))
                    {
                        if (player->HasSpell(spellId) || sSpellMgr->GetSpellLearnSkill(spellId) ||
                            !IsSkillLineSpell(spellId, skillId) || !player->IsSpellFitByClassAndRace(spellId))
                            continue;

                        player->learnSpell(spellId);
                        if (!player->HasSpell(spellId))
                            continue;

                        ++learned;
                        progress = true;
                    }
                }
            }
        }

        return learned;
    }

    static bool HandleLearnProfessionCommand(ChatHandler* handler, Tail args)
    {
        // Example: .learn profession enchanting artisan
        std::string name = NormalizeProfessionArg(args);

        // From the highest rank down, so "grandmaster" is matched before "master"
        uint8 step = 0;
        for (uint8 i = uint8(ProfessionRanks.size()); i > 0; --i)
        {
            std::string_view const key = ProfessionRanks[i - 1].Key;
            if (name.size() > key.size() && name.ends_with(key))
            {
                step = i;
                name.resize(name.size() - key.size());
                break;
            }
        }

        if (!step)
        {
            handler->SendErrorMessage("Syntax: .learn profession $profession $rank (apprentice, journeyman, expert, artisan, master, grand master)");
            return false;
        }

        SkillLineEntry const* skillInfo = FindProfessionSkill(handler, name);
        if (!skillInfo)
        {
            handler->SendErrorMessage("Unknown profession '{}'.", name);
            return false;
        }

        Player* target = handler->getSelectedPlayerOrSelf();
        if (!target)
        {
            handler->SendErrorMessage(LANG_NO_CHAR_SELECTED);
            return false;
        }

        uint32 const skillId = skillInfo->id;
        char const* skillName = GetSkillName(handler, skillInfo);
        std::string_view const rankName = ProfessionRanks[step - 1].Name;

        std::vector<uint32> const ranks = GetProfessionRankSpells(skillId);
        if (ranks.size() < step)
        {
            handler->SendErrorMessage("{} has no {} rank.", skillName, rankName);
            return false;
        }

        if (target->GetSkillStep(skillId) > step)
        {
            handler->SendErrorMessage("{} already has a higher {} rank. Use .unlearn profession first.", handler->GetNameLink(target), skillName);
            return false;
        }

        if (IsPrimaryProfessionSkill(skillId) && !target->HasSkill(skillId) && !target->GetFreePrimaryProfessionPoints())
        {
            handler->SendErrorMessage("{} has no free primary profession slot.", handler->GetNameLink(target));
            return false;
        }

        // Every rank up to the requested one, in order, as trainers teach them: Apprentice takes the profession slot
        // and each later rank raises the cap. Skipping ranks leaves trainers offering the missing ones, which reset the cap.
        for (uint8 i = 0; i < step; ++i)
            if (!target->HasSpell(ranks[i]))
                target->learnSpell(ranks[i]);

        SpellLearnSkillNode const* rankSkill = sSpellMgr->GetSpellLearnSkill(ranks[step - 1]);
        uint16 const cap = rankSkill->maxvalue ? rankSkill->maxvalue : target->GetMaxSkillValueForLevel();
        target->SetSkill(skillId, step, cap, cap);

        uint32 const recipes = LearnTrainerRecipes(target, skillId, cap);

        handler->PSendSysMessage("{} learned {} {} ({}/{}) and {} trainer recipes.", handler->GetNameLink(target), rankName, skillName, cap, cap, recipes);
        return true;
    }

    static bool HandleUnLearnProfessionCommand(ChatHandler* handler, Tail args)
    {
        // Example: .unlearn profession enchanting
        SkillLineEntry const* skillInfo = FindProfessionSkill(handler, NormalizeProfessionArg(args));
        if (!skillInfo)
        {
            handler->SendErrorMessage("Syntax: .unlearn profession $profession");
            return false;
        }

        Player* target = handler->getSelectedPlayerOrSelf();
        if (!target)
        {
            handler->SendErrorMessage(LANG_NO_CHAR_SELECTED);
            return false;
        }

        uint32 const skillId = skillInfo->id;
        char const* skillName = GetSkillName(handler, skillInfo);
        if (!target->HasSkill(skillId))
        {
            handler->SendErrorMessage("{} doesn't know {}.", handler->GetNameLink(target), skillName);
            return false;
        }

        // Removing the lowest known rank removes the higher ranks with it, then the skill and all its recipes, and gives
        // a primary profession slot back
        for (uint32 rankSpell : sSpellMgr->GetSkillRankSpells(skillId))
            if (target->HasSpell(rankSpell))
                target->removeSpell(rankSpell, SPEC_MASK_ALL, false);

        // A skill given without its rank spells (e.g. by .setskill) still has to go
        if (target->HasSkill(skillId))
            target->SetSkill(skillId, 0, 0, 0);

        handler->PSendSysMessage("{} unlearned {} and all its recipes.", handler->GetNameLink(target), skillName);
        return true;
    }

    static bool HandleLearnCommand(ChatHandler* handler, SpellInfo const* spell, Optional<EXACT_SEQUENCE("all")> allRanks)
    {
        Player* targetPlayer = handler->getSelectedPlayer();

        if (!targetPlayer)
        {
            handler->SendErrorMessage(LANG_PLAYER_NOT_FOUND);
            return false;
        }

        return Acore::PlayerCommand::HandleLearnSpellCommand(handler, targetPlayer, spell, allRanks);
    }

    static bool HandleLearnAllGMCommand(ChatHandler* handler)
    {
        for (uint32 i = 0; i < sSpellMgr->GetSpellInfoStoreSize(); ++i)
        {
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(i);
            if (!spellInfo || !SpellMgr::IsSpellValid(spellInfo))
                continue;

            if (!spellInfo->IsAbilityOfSkillType(SKILL_INTERNAL))
                continue;

            handler->GetSession()->GetPlayer()->learnSpell(i);
        }

        handler->SendSysMessage(LANG_LEARNING_GM_SKILLS);
        return true;
    }

    static bool HandleLearnAllMyClassCommand(ChatHandler* handler)
    {
        HandleLearnAllMyTrainerSpellsCommand(handler);
        HandleLearnAllMyTalentsCommand(handler);
        HandleLearnAllMyQuestSpells(handler);
        return true;
    }

    static bool HandleLearnAllMyQuestSpells(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        for (auto const& questPair : sObjectMgr->GetQuestTemplates())
        {
            Quest const* quest = questPair.second;
            if (quest->GetRequiredClasses() && player->SatisfyQuestClass(quest, false))
                player->learnQuestRewardedSpells(quest);
        }

        return true;
    }

    static bool HandleLearnAllMyTrainerSpellsCommand(ChatHandler* handler)
    {
        if (!sChrClassesStore.LookupEntry(handler->GetSession()->GetPlayer()->getClass()))
            return true;

        Player* player = handler->GetPlayer();
        std::vector<Trainer::Trainer const*> const& trainers = sObjectMgr->GetClassTrainers(player->getClass());

        bool hadNew;
        do
        {
            hadNew = false;
            for (Trainer::Trainer const* trainer : trainers)
            {
                if (!trainer->IsTrainerValidForPlayer(player))
                    continue;

                for (Trainer::Spell const& trainerSpell : trainer->GetSpells())
                {
                    if (!trainer->CanTeachSpell(player, &trainerSpell))
                        continue;

                    if (trainerSpell.IsCastable())
                        player->CastSpell(player, trainerSpell.SpellId, true);
                    else
                        player->learnSpell(trainerSpell.SpellId, false);

                    // a script can refuse the learn or force the spell's state, so only go around
                    // again when the spell actually stopped being teachable
                    if (!trainer->CanTeachSpell(player, &trainerSpell))
                        hadNew = true;
                }
            }
        } while (hadNew);

        handler->SendSysMessage(LANG_COMMAND_LEARN_CLASS_SPELLS);
        return true;
    }

    static bool HandleLearnAllMyTalentsCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        uint32 classMask = player->getClassMask();

        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* talentInfo = sTalentStore.LookupEntry(i);
            if (!talentInfo)
                continue;

            TalentTabEntry const* talentTabInfo = sTalentTabStore.LookupEntry(talentInfo->TalentTab);
            if (!talentTabInfo)
                continue;

            if ((classMask & talentTabInfo->ClassMask) == 0)
                continue;

            // xinef: search highest talent rank
            uint32 spellId = 0;
            uint8 rankId = MAX_TALENT_RANK;
            for (int8 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
            {
                if (talentInfo->RankID[rank] != 0)
                {
                    rankId = rank;
                    spellId = talentInfo->RankID[rank];
                    break;
                }
            }

            // xinef: some errors?
            if (!spellId || rankId == MAX_TALENT_RANK)
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo || !SpellMgr::IsSpellValid(spellInfo))
                continue;

            player->LearnTalent(talentInfo->TalentID, rankId, true);
        }

        player->SetFreeTalentPoints(0);
        player->SendTalentsInfoData(false);

        handler->SendSysMessage(LANG_COMMAND_LEARN_CLASS_TALENTS);
        return true;
    }

    static bool HandleLearnAllMyPetTalentsCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();

        Pet* pet = player->GetPet();
        if (!pet)
        {
            handler->SendErrorMessage(LANG_NO_PET_FOUND);
            return false;
        }

        CreatureTemplate const* creatureInfo = pet->GetCreatureTemplate();
        if (!creatureInfo)
        {
            handler->SendErrorMessage(LANG_WRONG_PET_TYPE);
            return false;
        }

        CreatureFamilyEntry const* petFamily = sCreatureFamilyStore.LookupEntry(creatureInfo->family);
        if (!petFamily)
        {
            handler->SendErrorMessage(LANG_WRONG_PET_TYPE);
            return false;
        }

        if (petFamily->petTalentType < 0)                       // not hunter pet
        {
            handler->SendErrorMessage(LANG_WRONG_PET_TYPE);
            return false;
        }

        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* talentInfo = sTalentStore.LookupEntry(i);
            if (!talentInfo)
                continue;

            TalentTabEntry const* talentTabInfo = sTalentTabStore.LookupEntry(talentInfo->TalentTab);
            if (!talentTabInfo)
                continue;

            // prevent learn talent for different family (cheating)
            if (((1 << petFamily->petTalentType) & talentTabInfo->petTalentMask) == 0)
                continue;

            // search highest talent rank
            uint32 spellId = 0;

            for (int8 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
            {
                if (talentInfo->RankID[rank] != 0)
                {
                    spellId = talentInfo->RankID[rank];
                    break;
                }
            }

            if (!spellId)                                        // ??? none spells in talent
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo || !SpellMgr::IsSpellValid(spellInfo))
                continue;

            // learn highest rank of talent and learn all non-talent spell ranks (recursive by tree)
            pet->learnSpellHighRank(spellId);
        }

        pet->SetFreeTalentPoints(0);

        handler->SendSysMessage(LANG_COMMAND_LEARN_PET_TALENTS);
        return true;
    }

    static bool HandleLearnAllLangCommand(ChatHandler* handler)
    {
        for (LanguageDesc const& langDesc : lang_description)
            if (uint32 langSpellId = langDesc.spell_id)
            {
                handler->GetPlayer()->learnSpell(langSpellId);
                handler->GetPlayer()->SetSkill(langDesc.skill_id, 0, 300, 300);
            }

        handler->SendSysMessage(LANG_COMMAND_LEARN_ALL_LANG);
        return true;
    }

    static bool HandleLearnAllDefaultCommand(ChatHandler* handler, Optional<PlayerIdentifier> player)
    {
        if (!player)
            player = PlayerIdentifier::FromTargetOrSelf(handler);
        if (!player || !player->IsConnected())
            return false;

        Player* target = player->GetConnectedPlayer();
        target->LearnDefaultSkills();
        target->LearnCustomSpells();
        target->learnQuestRewardedSpells();

        handler->PSendSysMessage(LANG_COMMAND_LEARN_ALL_DEFAULT_AND_QUEST, handler->GetNameLink(target));
        return true;
    }

    static bool HandleLearnAllCraftsCommand(ChatHandler* handler)
    {
        Player* target = handler->GetSession()->GetPlayer();

        for (uint32 i = 0; i < sSkillLineStore.GetNumRows(); ++i)
        {
            SkillLineEntry const* skillInfo = sSkillLineStore.LookupEntry(i);
            if (!skillInfo)
                continue;

            if ((skillInfo->categoryId == SKILL_CATEGORY_PROFESSION || skillInfo->categoryId == SKILL_CATEGORY_SECONDARY) &&
                    skillInfo->canLink)                             // only prof. with recipes have
            {
                HandleLearnSkillRecipesHelper(target, skillInfo->id);

                uint16 const maxLevel = target->GetPureMaxSkillValue(skillInfo->id);
                target->SetSkill(skillInfo->id, target->GetSkillStep(skillInfo->id), maxLevel, maxLevel);
            }
        }

        handler->SendSysMessage(LANG_COMMAND_LEARN_ALL_CRAFT);
        return true;
    }

    static bool HandleLearnAllRecipesCommand(ChatHandler* handler, WTail namePart)
    {
        //  Learns all recipes of specified profession and sets skill to max
        //  Example: .learn all_recipes enchanting

        Player* target = handler->getSelectedPlayer();
        if (!target)
        {
            handler->SendSysMessage(LANG_PLAYER_NOT_FOUND);
            return false;
        }

        if (namePart.empty())
            return false;

        // converting string that we try to find to lower case
        wstrToLower(namePart);

        SkillLineEntry const* targetSkillInfo = nullptr;
        char const* name = nullptr;
        for (uint32 i = 1; i < sSkillLineStore.GetNumRows(); ++i)
        {
            SkillLineEntry const* skillInfo = sSkillLineStore.LookupEntry(i);
            if (!skillInfo)
                continue;

            if ((skillInfo->categoryId != SKILL_CATEGORY_PROFESSION &&
                 skillInfo->categoryId != SKILL_CATEGORY_SECONDARY) ||
                !skillInfo->canLink)                            // only prof with recipes have set
                continue;

            uint8 locale = 0;
            for (; locale < TOTAL_LOCALES; ++locale)
            {
                name = skillInfo->name[locale];
                if (!name || !*name)
                    continue;

                if (Utf8FitTo(name, namePart))
                    break;
            }

            if (locale < TOTAL_LOCALES)
            {
                targetSkillInfo = skillInfo;
                break;
            }
        }

        if (!(name && targetSkillInfo))
            return false;

        HandleLearnSkillRecipesHelper(target, targetSkillInfo->id);

        uint16 maxLevel = target->GetPureMaxSkillValue(targetSkillInfo->id);
        target->SetSkill(targetSkillInfo->id, target->GetSkillStep(targetSkillInfo->id), maxLevel, maxLevel);
        handler->PSendSysMessage(LANG_COMMAND_LEARN_ALL_RECIPES, name);
        return true;
    }

    static void HandleLearnSkillRecipesHelper(Player* player, uint32 skillId)
    {
        // Rank spells (Apprentice -> Grand Master) must be learned so that the
        // skill-cleanup loop in Player::SetSkill (which calls removeSpell on the
        // first spell in each chain) can walk forward and strip every rank on
        // profession unlearn. Without the first rank in the spellbook that loop
        // bails out and the leftover rank spells re-grant the skill after relog
        // (issue #2330).
        for (uint32 rankSpell : sSpellMgr->GetSkillRankSpells(skillId))
            player->learnSpell(rankSpell);

        uint32 classmask = player->getClassMask();

        for (SkillLineAbilityEntry const* skillLine : GetSkillLineAbilitiesBySkillLine(skillId))
        {
            // not high rank
            if (skillLine->SupercededBySpell)
                continue;

            // skip racial skills
            if (skillLine->RaceMask != 0)
                continue;

            // skip wrong class skills
            if (skillLine->ClassMask && (skillLine->ClassMask & classmask) == 0)
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(skillLine->Spell);
            if (!spellInfo || !SpellMgr::IsSpellValid(spellInfo))
                continue;

            player->learnSpell(skillLine->Spell);
        }
    }

    static bool HandleUnLearnCommand(ChatHandler* handler, SpellInfo const* spell, Optional<EXACT_SEQUENCE("all")> allRanks)
    {
        Player* target = handler->getSelectedPlayer();
        if (!target)
        {
            handler->SendErrorMessage(LANG_NO_CHAR_SELECTED);
            return false;
        }

        return Acore::PlayerCommand::HandleUnlearnSpellCommand(handler, target, spell, allRanks);
    }
};

void AddSC_learn_commandscript()
{
    new learn_commandscript();
}
