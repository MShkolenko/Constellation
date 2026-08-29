/*
 * Constellation — server-side companion module for AlgalonCore.
 *
 * Phase 1-2: a fixed roster of companions (one per playable race, gender-matched
 * names), created server-side on first need and logged in through socketless
 * sessions. Sessions are NEVER handed to the session manager: a null-socket
 * session would be reaped by World::UpdateSessions (Update returns false), so
 * the module owns them and ticks them from the world-update hook itself.
 * Outbound packets are dropped by WorldSession::SendPacket's null-socket guard.
 */

#include "Registration.h"
#include "Roster.h"

#include "AccountMgr.h"
#include "CharacterCache.h"
#include "CharacterPackets.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DB2Stores.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldSession.h"

#include <memory>
#include <vector>

using namespace Trinity::ChatCommands;

namespace Constellation
{
namespace
{
bool IsEnabled()
{
    return sConfigMgr->GetBoolDefault("Constellation.Enable", false);
}

bool AutoSummon()
{
    return sConfigMgr->GetBoolDefault("Constellation.AutoSummon", true);
}

std::string AccountName()
{
    return sConfigMgr->GetStringDefault("Constellation.Account", "CONSTELLATION");
}

// ---------------------------------------------------------------- companions

enum class Stage : uint8
{
    Offline,        // known, no session
    EnumQueued,     // session made, char-enum sent (fills _legitCharacters)
    LoginSent,      // player-login sent, holder in flight
    InWorld,        // session->GetPlayer() is live
    Failed          // gave up after retries; .constellation summon retries
};

struct Companion
{
    RosterEntry const* Entry = nullptr;
    ObjectGuid Guid;                    // filled once the character exists
    WorldSession* Session = nullptr;    // owned by the module, not the manager
    Stage State = Stage::Offline;
    uint32 TicksInState = 0;
    uint8 Retries = 0;
};

class Manager
{
public:
    static Manager* Instance()
    {
        static Manager instance;
        return &instance;
    }

    void OnWorldUpdate(uint32 diff)
    {
        if (!IsEnabled())
            return;

        // gentle start: let the world settle before the first login
        if (_warmupMs < 10000)
        {
            _warmupMs += diff;
            return;
        }

        if (!_bootstrapped)
            Bootstrap();

        // stagger the pipeline: at most one state advance per 2 s tick window
        _throttleMs += diff;
        bool mayAdvance = _throttleMs >= 2000;

        for (Companion& c : _companions)
        {
            TickSession(c, diff);
            if (mayAdvance && AdvanceOne(c))
            {
                _throttleMs = 0;
                mayAdvance = false;
            }
        }
    }

    void Shutdown()
    {
        for (Companion& c : _companions)
            Dismiss(c, /*final=*/true);
    }

    bool SummonAll(ChatHandler* handler)
    {
        uint32 woken = 0;
        for (Companion& c : _companions)
            if (c.State == Stage::Failed || c.State == Stage::Offline)
            {
                c.State = Stage::Offline;
                c.Retries = 0;
                ++woken;
            }
        handler->PSendSysMessage("Constellation: {} companions queued (auto pipeline).", woken);
        return true;
    }

    bool DismissAll(ChatHandler* handler)
    {
        uint32 dropped = 0;
        for (Companion& c : _companions)
            if (c.Session)
            {
                Dismiss(c, /*final=*/false);
                ++dropped;
            }
        handler->PSendSysMessage("Constellation: {} companions dismissed.", dropped);
        return true;
    }

    void Status(ChatHandler* handler)
    {
        uint32 inWorld = 0, failed = 0;
        for (Companion const& c : _companions)
        {
            if (c.State == Stage::InWorld) ++inWorld;
            if (c.State == Stage::Failed)  ++failed;
        }
        handler->PSendSysMessage("Constellation {}: {} — roster {}, in world {}, failed {}",
            CONSTELLATION_VERSION, IsEnabled() ? "enabled" : "disabled",
            uint32(_companions.size()), inWorld, failed);
        for (Companion const& c : _companions)
            if (c.State == Stage::InWorld && c.Session && c.Session->GetPlayer())
                handler->PSendSysMessage("  {} — {} in world", c.Entry->Name,
                    c.Session->GetPlayer()->GetName());
    }

private:
    // one-time: account + roster rows resolved (created if missing)
    void Bootstrap()
    {
        _bootstrapped = true;

        _accountId = AccountMgr::GetId(AccountName());
        if (!_accountId)
        {
            std::string password = Trinity::StringFormat("cst-{}", GameTime::GetGameTime());
            if (sAccountMgr->CreateAccount(AccountName(), password) != AccountOpResult::AOR_OK)
            {
                TC_LOG_ERROR("server.loading", "Constellation: cannot create account '{}'", AccountName());
                return;
            }
            _accountId = AccountMgr::GetId(AccountName());
            TC_LOG_INFO("server.loading", "Constellation: account '{}' created (id {})", AccountName(), _accountId);
        }

        for (RosterEntry const& entry : Roster)
        {
            Companion c;
            c.Entry = &entry;
            if (CharacterCacheEntry const* cached = sCharacterCache->GetCharacterCacheByName(entry.Name))
                c.Guid = cached->Guid;
            _companions.push_back(c);
        }
        TC_LOG_INFO("server.loading", "Constellation: bootstrap — account {}, roster {}", _accountId, uint32(_companions.size()));
    }

    // our sessions live outside the manager: tick them or their query
    // callbacks (char-enum, login holder) never fire
    void TickSession(Companion& c, uint32 diff)
    {
        if (!c.Session)
            return;
        // Update()'s FIRST statement closes the realm socket when the connection
        // is idle — unguarded, and a socketless session is idle forever. Keep the
        // idle clock reset every tick so that branch never runs (found by gdb:
        // null deref at m_Socket[REALM]->CloseSocket()).
        c.Session->ResetTimeOutTime(false);
        WorldSessionFilter updater(c.Session);
        c.Session->Update(diff, updater);   // returns false for null socket — ignored by design
    }

    bool AdvanceOne(Companion& c)
    {
        ++c.TicksInState;
        switch (c.State)
        {
            case Stage::Offline:
            {
                if (!AutoSummon() && c.Retries == 0 && c.TicksInState < 2)
                    return false;   // waits for .constellation summon
                if (!_accountId)
                    return false;
                // session FIRST: Player's constructor (used by CreateCharacter)
                // dereferences it — creating before the session was the phase-1 segfault
                if (!c.Session)
                    MakeSession(c);
                if (!c.Guid && !CreateCharacter(c))
                    { Fail(c, "creation"); return true; }
                WorldPacket raw(CMSG_ENUM_CHARACTERS);
                WorldPackets::Character::EnumCharacters enumChars(std::move(raw));
                c.Session->HandleCharEnumOpcode(enumChars);
                c.State = Stage::EnumQueued;
                c.TicksInState = 0;
                return true;
            }
            case Stage::EnumQueued:
            {
                // enum callback needs a few ticks; then declare the login
                if (c.TicksInState < 3)
                    return false;
                WorldPacket raw(CMSG_PLAYER_LOGIN);
                WorldPackets::Character::PlayerLogin login(std::move(raw));
                login.Guid = c.Guid;
                c.Session->HandlePlayerLoginOpcode(login);
                c.State = Stage::LoginSent;
                c.TicksInState = 0;
                return true;
            }
            case Stage::LoginSent:
            {
                if (c.Session->GetPlayer())
                {
                    c.State = Stage::InWorld;
                    c.TicksInState = 0;
                    TC_LOG_INFO("server.worldserver", "Constellation: {} is in the world", c.Entry->Name);
                    return true;
                }
                if (c.TicksInState > 15)    // ~30 s — enum not ready or rejected; retry
                {
                    if (++c.Retries > 3)
                        { Fail(c, "login"); return true; }
                    DropSession(c);
                    c.State = Stage::Offline;
                    c.TicksInState = 0;
                }
                return false;
            }
            case Stage::InWorld:
            {
                if (!c.Session->GetPlayer())    // died out of world? re-run pipeline
                {
                    DropSession(c);
                    c.State = Stage::Offline;
                    c.TicksInState = 0;
                }
                return false;
            }
            case Stage::Failed:
            default:
                return false;
        }
    }

    void Fail(Companion& c, char const* what)
    {
        TC_LOG_ERROR("server.worldserver", "Constellation: {} failed at {} — parked", c.Entry->Name, what);
        DropSession(c);
        c.State = Stage::Failed;
        c.TicksInState = 0;
    }

    void MakeSession(Companion& c)
    {
        std::string name = AccountName();
        c.Session = new WorldSession(_accountId, std::move(name), 0, "",
            nullptr, SEC_PLAYER, uint8(sWorld->getIntConfig(CONFIG_EXPANSION)),
            0, "Win", Minutes(0), 65299 /* frozen client build */, {}, LOCALE_ruRU, 0, false);
    }

    void DropSession(Companion& c)
    {
        if (!c.Session)
            return;
        if (c.Session->GetPlayer())
            c.Session->LogoutPlayer(true);
        delete c.Session;
        c.Session = nullptr;
    }

    void Dismiss(Companion& c, bool final)
    {
        DropSession(c);
        c.State = final ? Stage::Failed : Stage::Offline;
        c.Retries = final ? 4 : 0;
        c.TicksInState = 0;
    }

    // server-side character creation, mirroring the char-create handler but
    // synchronous: build info, default appearance, Create, save, cache
    bool CreateCharacter(Companion& c)
    {
        RosterEntry const& e = *c.Entry;

        std::string name = e.Name;
        if (sObjectMgr->CheckPlayerName(name, LOCALE_ruRU, true) != CHAR_NAME_SUCCESS
            || sCharacterCache->GetCharacterCacheByName(name))
        {
            name += "us";   // deterministic fallback; roster names are curated to not need it
            if (sObjectMgr->CheckPlayerName(name, LOCALE_ruRU, true) != CHAR_NAME_SUCCESS
                || sCharacterCache->GetCharacterCacheByName(name))
                return false;
        }

        auto createInfo = std::make_shared<WorldPackets::Character::CharacterCreateInfo>();
        createInfo->Race = e.Race;
        createInfo->Class = e.Class;
        createInfo->Sex = e.Sex;
        createInfo->Name = name;

        // default appearance: first choice of every customization option for
        // this race/gender — the same body the client offers before any slider
        if (std::vector<ChrCustomizationOptionEntry const*> const* options =
                sDB2Manager.GetCustomiztionOptions(e.Race, e.Sex))
        {
            for (ChrCustomizationOptionEntry const* option : *options)
            {
                std::vector<ChrCustomizationChoiceEntry const*> const* choices =
                    sDB2Manager.GetCustomiztionChoices(option->ID);
                if (!choices || choices->empty())
                    continue;
                auto& choice = createInfo->Customizations.emplace_back();
                choice.ChrCustomizationOptionID = option->ID;
                choice.ChrCustomizationChoiceID = choices->front()->ID;
            }
        }

        std::shared_ptr<Player> newChar(new Player(c.Session), [](Player* ptr)
        {
            ptr->CleanupsBeforeDelete();
            delete ptr;
        });
        newChar->GetMotionMaster()->Initialize();
        if (!newChar->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), createInfo.get()))
        {
            TC_LOG_ERROR("server.worldserver", "Constellation: Player::Create failed for {} (race {}, class {})",
                name, e.Race, e.Class);
            return false;
        }
        newChar->setCinematic(1);

        CharacterDatabaseTransaction characterTransaction = CharacterDatabase.BeginTransaction();
        LoginDatabaseTransaction loginTransaction = LoginDatabase.BeginTransaction();
        newChar->SaveToDB(loginTransaction, characterTransaction, true);
        CharacterDatabase.CommitTransaction(characterTransaction);
        LoginDatabase.CommitTransaction(loginTransaction);

        sCharacterCache->AddCharacterCacheEntry(newChar->GetGUID(), _accountId, newChar->GetName(),
            e.Sex, e.Race, e.Class, 1, false);

        c.Guid = newChar->GetGUID();
        TC_LOG_INFO("server.worldserver", "Constellation: created {} ({}, race {}, class {})",
            name, c.Guid.ToString(), e.Race, e.Class);
        return true;
    }

    std::vector<Companion> _companions;
    uint32 _accountId = 0;
    uint32 _warmupMs = 0;
    uint32 _throttleMs = 0;
    bool _bootstrapped = false;
};
}

} // namespace Constellation

class constellation_worldscript : public WorldScript
{
public:
    constellation_worldscript() : WorldScript("constellation_worldscript") { }

    void OnStartup() override
    {
        TC_LOG_INFO("server.loading", "Constellation {} — {}", CONSTELLATION_VERSION,
            Constellation::IsEnabled() ? "enabled" : "present but disabled (Constellation.Enable = 0)");
    }

    void OnUpdate(uint32 diff) override
    {
        Constellation::Manager::Instance()->OnWorldUpdate(diff);
    }

    void OnShutdown() override
    {
        Constellation::Manager::Instance()->Shutdown();
    }
};

class constellation_commandscript : public CommandScript
{
public:
    constellation_commandscript() : CommandScript("constellation_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable constellationTable =
        {
            { "status",  HandleStatus,  rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "summon",  HandleSummon,  rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "dismiss", HandleDismiss, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
        };
        static ChatCommandTable commandTable =
        {
            { "constellation", constellationTable },
        };
        return commandTable;
    }

    static bool HandleStatus(ChatHandler* handler)
    {
        Constellation::Manager::Instance()->Status(handler);
        return true;
    }

    static bool HandleSummon(ChatHandler* handler)
    {
        return Constellation::Manager::Instance()->SummonAll(handler);
    }

    static bool HandleDismiss(ChatHandler* handler)
    {
        return Constellation::Manager::Instance()->DismissAll(handler);
    }
};

void AddConstellationScripts()
{
    new constellation_worldscript();
    new constellation_commandscript();
}
