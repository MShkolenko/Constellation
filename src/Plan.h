/*
 * Constellation — the hub-first quest plan. Stage 1: build at warm-up, audit against the real
 * menus, persist three tables. No routing yet: what the companions DO is unchanged.
 *
 * Contract: homelab/.agent/design/constellation-plan/plan-spec-v4.md as amended by v5..v8,
 * nine Codex passes, the last one BUILD THE FIRST CUT AS WRITTEN; the code itself reviewed in
 * plan-cut1-review.md. Every rule below names the spec section it implements; do not "improve"
 * a rule here without a new pass there.
 *
 * Copyright (C) 2026 Constellation contributors. Licensed under the GNU AGPL v3 — see COPYING.
 */
#ifndef CONSTELLATION_PLAN_H
#define CONSTELLATION_PLAN_H

#include "Define.h"
#include "ObjectGuid.h"
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

class Player;
class Creature;
class Quest;
class QuestMenu;

namespace Constellation::Plan
{
    // §2″ typed site key: kind 0 creature spawn, 1 gameobject spawn; total order.
    struct SiteKey
    {
        uint8 Kind = 0;
        ObjectGuid::LowType Spawn = 0;
        bool operator<(SiteKey const& o) const { return Kind != o.Kind ? Kind < o.Kind : Spawn < o.Spawn; }
        bool operator==(SiteKey const& o) const { return Kind == o.Kind && Spawn == o.Spawn; }
    };

    // A hub is addressed by its zone (QuestSortID) and its per-zone canonical id (§2″).
    struct HubRef
    {
        int32 Zone = 0;
        uint16 Id = 0;
        bool operator<(HubRef const& o) const { return Zone != o.Zone ? Zone < o.Zone : Id < o.Id; }
        bool operator==(HubRef const& o) const { return Zone == o.Zone && Id == o.Id; }
        explicit operator bool() const { return Id != 0; }
    };

    struct Site
    {
        SiteKey Key;
        uint32 Entry = 0;
        float X = 0.f, Y = 0.f, Z = 0.f;
        uint32 LeafArea = 0;        // §2′ from the terrain (this core keeps no areaId in SpawnData)
        uint32 ZoneArea = 0;        // last non-zero before the ParentAreaID root
        uint8 PhaseUseFlags = 0;    // §3 InDbPhaseShift inputs, read at each decision
        uint16 PhaseId = 0;
        uint32 PhaseGroup = 0;
        int32 TerrainSwap = -1;     // §10‴ != -1 → unsupported(terrain_swap), audited, never a hub member
        std::vector<uint32> Gives;  // zone quests started here, sorted
        std::vector<uint32> Ends;   // zone quests ended here, sorted
    };

    struct Hub
    {
        HubRef Ref;
        std::vector<SiteKey> Sites;
        float MinX = 0.f, MaxX = 0.f, MinY = 0.f, MaxY = 0.f;   // v4: extent + 100 yards
        bool RoadStop = false;      // one-site component; never Current (v4)
    };

    enum class Obligation : uint8 { Required, Alternative, Deferred, Ineligible, Unsupported, Skipped };
    enum class Leaves : uint8 { Local, Transition, Unknown };

    struct ObligationRow
    {
        uint32 Quest = 0;
        HubRef Hub;
        Obligation State = Obligation::Deferred;
        std::string Reason;         // "unvisited", "race", "class", "terrain_swap"
        int32 ExclusiveGroup = 0;
        Leaves Leave = Leaves::Unknown;     // data-only classification at build (v4 §6, no phase)
    };

    struct Skeleton
    {
        uint8 Race = 0, Class = 0;
        std::vector<ObligationRow> Rows;
    };

    // §5‴ SiteSeen entry — written at every consultation, before any menu attempt.
    struct Seen
    {
        uint32 Tick = 0;
        bool Compatible = false;
        std::map<uint32, uint8> Offered;            // quest → menu icon, as the core built it
    };

    enum class Annotation : uint8 { Deferred, Required, Taken, Done, Alternative, Ineligible, Unsupported, Skipped };

    struct Note
    {
        Annotation State = Annotation::Deferred;
        std::string Reason;
        uint8 PhaseMisses = 0;      // §3′ per quest
    };

    struct CompanionPlan
    {
        HubRef Current;
        Skeleton const* Skel = nullptr;
        std::map<SiteKey, Seen> SiteSeen;
        std::map<uint32, uint8> LastStatus;         // §5⁗ status word per obligation of Current
        size_t Fingerprint = 0;                     // §3‴
        std::map<uint32, Note> Notes;               // plan_annotation mirror
        std::set<HubRef> ExhaustedLogged;           // the exhaustion line once per hub
        bool Reconciled = false;
    };

    // The mirror's gate name: the module's own FirstFailingGate, passed in so Plan.cpp stays
    // free of Manager.
    using GateFn = std::function<char const* (Player*, Quest const*)>;

    class Planner
    {
    public:
        static Planner* Instance();

        // §10‴ at warm-up, world thread, before Bootstrap. False = hard failure, plan disabled.
        bool Build();
        bool Built() const { return _built; }
        uint64 BuildId() const { return _buildId; }

        // Decision points (§3″). All no-ops unless Built() and the player is in scope.
        void OnLogin(Player* self);                                   // §8″/§8‴ reconciliation, timed
        void OnConsult(Player* self, Creature* giver);                // §5‴ before the menu is requested
        void OnMenuRead(Player* self, Creature* giver, QuestMenu const& menu, GateFn const& gate);
        void OnTakeOrTurnIn(Player* self);                            // observation only (§5⁗)
        void OnLogout(ObjectGuid guid);
        void Tick(uint32 diff);                                       // §8‴ histogram lines, no core calls
        void OnShutdown();

    private:
        Planner() = default;

        bool CollectSites(std::string& why);
        bool LoadConditionDeps(std::string& why);
        bool Partition(std::string& why);
        bool CheckFixtures(std::string& why);
        void BuildDeps();
        void BuildSkeletons();
        bool PersistObligations(std::string& why);

        Site const* FindSite(SiteKey key) const;
        Hub const* FindHub(HubRef ref) const;
        std::vector<SiteKey> Givers(uint32 quest, Hub const& hub) const;
        std::vector<SiteKey> EndersInside(uint32 quest, Hub const& hub) const;
        Leaves LeavesData(uint32 quest, Hub const& hub) const;
        Leaves LeavesNow(Player* self, uint32 quest, Hub const& hub) const;

        bool InScope(Player* self) const;
        CompanionPlan& StateOf(Player* self);
        Skeleton const* SkeletonFor(Player* self) const;
        void Arrive(Player* self, CompanionPlan& cp, Hub const& hub);
        void Observe(Player* self, CompanionPlan& cp);
        size_t FingerprintOf(Player* self) const;
        void ConsultIncompatible(Player* self, CompanionPlan& cp, Hub const& hub);
        void Audit(Player* self, CompanionPlan& cp, Site const& site, GateFn const& gate);
        void Exhausted(Player* self, CompanionPlan& cp, Hub const& hub, GateFn const& gate);
        void Annotate(Player* self, CompanionPlan& cp, uint32 quest, Annotation state, std::string const& reason, uint8 misses);
        void Mismatch(Player* self, uint32 quest, Site const& site, bool offered, std::string const& gate);
        void PrintLoginStats(char const* when) const;

        bool _built = false;
        uint64 _buildId = 0;
        std::vector<Site> _sites;
        std::map<SiteKey, size_t> _siteIndex;
        std::vector<Hub> _hubs;
        std::map<std::pair<int32, SiteKey>, HubRef> _hubOf;     // (zone, site) → hub
        std::set<uint32> _zoneQuests;
        std::map<uint32, int32> _sortOf;                        // quest → QuestSortID
        std::map<uint32, std::set<uint32>> _deps;               // q → quests whose gates reference q
        std::vector<Skeleton> _skeletons;
        std::map<ObjectGuid, CompanionPlan> _state;

        uint32 _loginCount = 0;
        double _loginMaxMs = 0.0;
        std::vector<uint32> _loginHist = std::vector<uint32>(1024, 0);
        uint32 _sinceLoginMs = 0;
        uint32 _sinceStatsMs = 0;
        bool _bootstrapStatsDone = false;
    };
}

#endif
