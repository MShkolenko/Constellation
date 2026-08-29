/*
 * Constellation — server-side companion module for AlgalonCore.
 * Phase 0: registration, config surface, status command. No behavior yet.
 */

#include "Registration.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "Log.h"
#include "RBAC.h"
#include "ScriptMgr.h"

using namespace Trinity::ChatCommands;

namespace
{
bool IsEnabled()
{
    // Keys live in worldserver.conf under the Constellation.* prefix for now;
    // conf/constellation.conf.dist documents them. A dedicated file comes with
    // the first phase that actually needs bulk configuration.
    return sConfigMgr->GetBoolDefault("Constellation.Enable", false);
}
}

class constellation_worldscript : public WorldScript
{
public:
    constellation_worldscript() : WorldScript("constellation_worldscript") { }

    void OnStartup() override
    {
        TC_LOG_INFO("server.loading", "Constellation {} — {}", CONSTELLATION_VERSION,
            IsEnabled() ? "enabled" : "present but disabled (Constellation.Enable = 0)");
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
            { "status", HandleStatus, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
        };
        static ChatCommandTable commandTable =
        {
            { "constellation", constellationTable },
        };
        return commandTable;
    }

    static bool HandleStatus(ChatHandler* handler)
    {
        handler->PSendSysMessage("Constellation {}: {}", CONSTELLATION_VERSION,
            IsEnabled() ? "enabled" : "disabled");
        return true;
    }
};

void AddConstellationScripts()
{
    new constellation_worldscript();
    new constellation_commandscript();
}
