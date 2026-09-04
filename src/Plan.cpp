/*
 * Constellation — the hub-first quest plan, Stage 1. See Plan.h for the contract pointer.
 *
 * What runs when:
 *   Build()          once, at the module warm-up point, on the world thread, before Bootstrap.
 *   OnLogin()        once per companion entering the world: reconciliation, timed.
 *   OnConsult()      when the module is about to ask a giver for its menu (a consultation).
 *   OnMenuRead()     when the module has read the giver's quest menu (a decision point).
 *   OnTakeOrTurnIn() after a successful take or turn-in (observation only).
 *   Tick()           a millisecond counter for the two timing lines; no core calls.
 *   OnShutdown()     prints the login-timing histogram.
 * Nothing here changes what a companion does.
 *
 * Copyright (C) 2026 Constellation contributors. Licensed under the GNU AGPL v3 — see COPYING.
 */
#include "Plan.h"
#include "Creature.h"
#include "DB2Stores.h"
#include "DatabaseEnv.h"
#include "GossipDef.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "PhaseShift.h"
#include "PhasingHandler.h"
#include "Player.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "StringFormat.h"
#include "TerrainMgr.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace Constellation::Plan
{
    namespace
    {
        // Stage 1 scope (v4 §10): humans, Northshire and Elwynn, map 0.
        constexpr int32 ZoneSorts[] = { 6170, 12 };
        constexpr uint32 ZoneMap = 0;
        constexpr float EdgeYards = 150.0f;         // §2 an edge joins sites within this
        constexpr float BoxCapYards = 600.0f;       // §2 a hub never grows past this per axis
        constexpr float BoundsPad = 100.0f;         // v4 §2 Bounds = extent + 100
        constexpr uint8 PhaseMissLimit = 3;         // §3′ Deferred(phase) after three empty visits
        constexpr uint32 ReadCap = 4096;            // §8‴ the login read is bounded

        // The fixtures (v5, verified on world_1127 2026-09-04). A build that disagrees is a failure.
        struct Fixture { int32 Zone; std::vector<ObjectGuid::LowType> Spawns; float MinX, MaxX, MinY, MaxY; };
        Fixture const Fixtures[] =
        {
            { 6170, { 279961, 279904, 279895, 279975 }, -9024.f, -8714.f, -259.f, -36.f },
            { 12,   { 280689, 280705, 280685, 280682, 280692 }, -9596.f, -9336.f, -68.f, 188.f },
        };
        constexpr ObjectGuid::LowType FalkhaanSpawn = 452570;   // a road stop in zone 12

        char const* ObligationName(Obligation o)
        {
            switch (o)
            {
                case Obligation::Required:    return "required";
                case Obligation::Alternative: return "alternative";
                case Obligation::Deferred:    return "deferred";
                case Obligation::Ineligible:  return "ineligible";
                case Obligation::Unsupported: return "unsupported";
                case Obligation::Skipped:     return "skipped";
            }
            return "deferred";
        }

        char const* LeavesName(Leaves l)
        {
            switch (l)
            {
                case Leaves::Local:      return "local";
                case Leaves::Transition: return "transition";
                case Leaves::Unknown:    return "unknown";
            }
            return "unknown";
        }

        char const* AnnotationName(Annotation a)
        {
            switch (a)
            {
                case Annotation::Deferred:    return "deferred";
                case Annotation::Required:    return "required";
                case Annotation::Taken:       return "taken";
                case Annotation::Done:        return "done";
                case Annotation::Alternative: return "alternative";
                case Annotation::Ineligible:  return "ineligible";
                case Annotation::Unsupported: return "unsupported";
                case Annotation::Skipped:     return "skipped";
            }
            return "deferred";
        }

        bool ParseAnnotation(std::string const& s, Annotation& out)
        {
            static std::pair<char const*, Annotation> const table[] =
            {
                { "deferred", Annotation::Deferred }, { "required", Annotation::Required },
                { "taken", Annotation::Taken }, { "done", Annotation::Done },
                { "alternative", Annotation::Alternative }, { "ineligible", Annotation::Ineligible },
                { "unsupported", Annotation::Unsupported }, { "skipped", Annotation::Skipped },
            };
            for (auto const& [name, value] : table)
                if (s == name)
                    { out = value; return true; }
            return false;
        }

        Annotation InitialAnnotation(Obligation o)
        {
            switch (o)
            {
                case Obligation::Ineligible:  return Annotation::Ineligible;
                case Obligation::Unsupported: return Annotation::Unsupported;
                default:                      return Annotation::Deferred;
            }
        }

        // §2′ area normalisation: walk ParentAreaID up; the last non-zero before the root is the zone.
        uint32 ZoneAreaOf(uint32 leaf)
        {
            uint32 cur = leaf;
            for (uint32 guard = 0; guard < 16 && cur; ++guard)
            {
                AreaTableEntry const* a = sAreaTableStore.LookupEntry(cur);
                if (!a)
                    return 0;
                if (!a->ParentAreaID)
                    return cur;
                cur = a->ParentAreaID;
            }
            return 0;
        }

        std::string Escaped(std::string s)
        {
            CharacterDatabase.EscapeString(s);
            return s;
        }

        // §5⁗ one number per quest: status plus the rewarded bit, so a reward is a transition too.
        uint8 StatusWord(Player* self, uint32 quest)
        {
            return uint8(self->GetQuestStatus(quest)) | (self->GetQuestRewardStatus(quest) ? 0x80 : 0);
        }

        bool InLog(Player* self, uint32 quest)
        {
            return self->FindQuestSlot(quest) < MAX_QUEST_LOG_SIZE;
        }

        bool Inside(Site const& s, Hub const& h)
        {
            return s.X >= h.MinX && s.X <= h.MaxX && s.Y >= h.MinY && s.Y <= h.MaxY;
        }

        // Player::PrepareQuestMenu, starter branch: the icon the core gives a takeable starter.
        uint8 StarterIcon(Quest const* q)
        {
            if (q->IsTurnIn())
                return (!q->IsRepeatable() || q->IsDaily() || q->IsWeekly() || q->IsMonthly()) ? 0 : 4;
            return 2;
        }

        uint32 NowSeconds()
        {
            return uint32(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        // §2″ the partition itself, on plain points so the synthetic fixture runs the same code:
        // edges (d², a.kind, a.spawn, b.kind, b.spawn) in that total order, union-find with the
        // 600-yard box cap, skip-not-retry; components and their members ordered by site key.
        struct Pt { SiteKey Key; float X, Y; uint32 ZoneArea; };

        std::vector<std::vector<size_t>> Components(std::vector<Pt> const& pts)
        {
            struct Edge { double D2; size_t A, B; };
            std::vector<Edge> edges;
            for (size_t i = 0; i < pts.size(); ++i)
                for (size_t j = i + 1; j < pts.size(); ++j)
                {
                    if (pts[i].ZoneArea != pts[j].ZoneArea)
                        continue;
                    double const dx = double(pts[i].X) - double(pts[j].X), dy = double(pts[i].Y) - double(pts[j].Y);
                    double const d2 = dx * dx + dy * dy;
                    if (d2 > double(EdgeYards) * double(EdgeYards))
                        continue;
                    size_t a = i, b = j;
                    if (pts[b].Key < pts[a].Key)
                        std::swap(a, b);
                    edges.push_back({ d2, a, b });
                }
            std::sort(edges.begin(), edges.end(), [&](Edge const& l, Edge const& r)
            {
                if (l.D2 != r.D2) return l.D2 < r.D2;
                if (!(pts[l.A].Key == pts[r.A].Key)) return pts[l.A].Key < pts[r.A].Key;
                return pts[l.B].Key < pts[r.B].Key;
            });
            std::vector<size_t> parent(pts.size());
            for (size_t i = 0; i < parent.size(); ++i)
                parent[i] = i;
            auto find = [&](size_t x) { while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
            struct Box { float MinX, MaxX, MinY, MaxY; };
            std::vector<Box> box(pts.size());
            for (size_t i = 0; i < pts.size(); ++i)
                box[i] = { pts[i].X, pts[i].X, pts[i].Y, pts[i].Y };
            for (Edge const& e : edges)
            {
                size_t ra = find(e.A), rb = find(e.B);
                if (ra == rb)
                    continue;
                Box merged = { std::min(box[ra].MinX, box[rb].MinX), std::max(box[ra].MaxX, box[rb].MaxX),
                               std::min(box[ra].MinY, box[rb].MinY), std::max(box[ra].MaxY, box[rb].MaxY) };
                if (merged.MaxX - merged.MinX > BoxCapYards || merged.MaxY - merged.MinY > BoxCapYards)
                    continue;                                   // skipped, never retried
                parent[rb] = ra;
                box[ra] = merged;
            }
            std::map<size_t, std::vector<size_t>> comps;
            for (size_t i = 0; i < pts.size(); ++i)
                comps[find(i)].push_back(i);
            std::vector<std::vector<size_t>> ordered;
            for (auto& [root, members] : comps)
            {
                std::sort(members.begin(), members.end(), [&](size_t a, size_t b) { return pts[a].Key < pts[b].Key; });
                ordered.push_back(members);
            }
            std::sort(ordered.begin(), ordered.end(), [&](auto const& l, auto const& r) { return pts[l.front()].Key < pts[r.front()].Key; });
            return ordered;
        }

        // v4 §2 "road of camps": five sites 120 yards apart → one hub of five; five at 130 with a
        // sixth 700 yards further → one hub of five and one road stop. Pure, runs before the data.
        bool RoadOfCamps(std::string& why)
        {
            auto line = [](uint32 n, float step, std::vector<Pt>& out)
            {
                for (uint32 i = 0; i < n; ++i)
                    out.push_back({ SiteKey{ 0, ObjectGuid::LowType(900000 + i) }, -12000.f + step * float(i), 9000.f, 1 });
            };
            std::vector<Pt> a;
            line(5, 120.f, a);
            auto ca = Components(a);
            if (ca.size() != 1 || ca[0].size() != 5)
            {
                why = Trinity::StringFormat("дорога лагерей: пять точек через 120 — компонент {}", uint32(ca.size()));
                return false;
            }
            std::vector<Pt> b;
            line(5, 130.f, b);
            b.push_back({ SiteKey{ 0, ObjectGuid::LowType(900010) }, -12000.f + 130.f * 4.f + 700.f, 9000.f, 1 });
            auto cb = Components(b);
            if (cb.size() != 2 || cb[0].size() != 5 || cb[1].size() != 1)
            {
                why = Trinity::StringFormat("дорога лагерей: пять через 130 и шестая за 700 — компонентов {}", uint32(cb.size()));
                return false;
            }
            return true;
        }
    }

    Planner* Planner::Instance()
    {
        static Planner instance;
        return &instance;
    }

    // ------------------------------------------------------------------ build (§10‴ contract)

    bool Planner::Build()
    {
        if (_built)
            return true;
        std::string why;
        auto const t0 = std::chrono::steady_clock::now();
        _buildId = uint64(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

        if (!RoadOfCamps(why) || !CollectSites(why) || !LoadConditionDeps(why) || !Partition(why) || !CheckFixtures(why))
        {
            TC_LOG_ERROR("server.loading", "Constellation ПЛАН: сборка отклонена — {}; план выключен на этот запуск", why);
            return false;
        }
        BuildDeps();
        BuildSkeletons();
        if (!PersistObligations(why))
        {
            TC_LOG_ERROR("server.loading", "Constellation ПЛАН: запись обязательств отклонена — {}; план выключен на этот запуск", why);
            return false;
        }
        _built = true;
        uint32 roadStops = 0, unsupported = 0;
        for (Hub const& h : _hubs)
            if (h.RoadStop)
                ++roadStops;
        for (Site const& s : _sites)
            if (s.TerrainSwap != -1)
                ++unsupported;
        auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        TC_LOG_INFO("server.loading", "Constellation ПЛАН: сборка {} — квестов зон {}, точек {} (не поддержано {}), хабов {} (из них обочин {}), скелетов {}, {} мс",
            _buildId, uint32(_zoneQuests.size()), uint32(_sites.size()), unsupported, uint32(_hubs.size()), roadStops, uint32(_skeletons.size()), ms);
        for (Hub const& h : _hubs)
        {
            if (h.RoadStop)
                continue;
            std::string members;
            for (SiteKey const& k : h.Sites)
                members += Trinity::StringFormat("{}{}:{} ", k.Kind ? "go" : "c", _sites[_siteIndex.at(k)].Entry, k.Spawn);
            TC_LOG_INFO("server.loading", "Constellation ПЛАН: зона {} хаб {} — {} точек, границы x [{:.0f}, {:.0f}] y [{:.0f}, {:.0f}]: {}",
                h.Ref.Zone, h.Ref.Id, uint32(h.Sites.size()), h.MinX, h.MaxX, h.MinY, h.MaxY, members);
        }
        return true;
    }

    // §2/§2′: every spawn on the zone map that starts or ends a zone quest. This core keeps no
    // areaId in SpawnData (Maps/SpawnData.h), so the area always comes from the terrain layer —
    // which is what the contract prescribes for a row whose areaId is 0, and every fixture row is.
    bool Planner::CollectSites(std::string& why)
    {
        _zoneQuests.clear();
        _sortOf.clear();
        for (auto const& kv : sObjectMgr->GetQuestTemplates())
        {
            Quest const* q = sObjectMgr->GetQuestTemplate(kv.first);
            if (!q)
                continue;
            for (int32 sort : ZoneSorts)
                if (q->GetZoneOrSort() == sort)
                {
                    _zoneQuests.insert(q->GetQuestId());
                    _sortOf[q->GetQuestId()] = sort;
                }
        }
        if (_zoneQuests.empty())
        {
            why = "ни одного квеста с QuestSortID зон";
            return false;
        }

        PhaseShift const& base = PhasingHandler::GetEmptyPhaseShift();
        auto addSite = [&](uint8 kind, SpawnData const& d, std::vector<uint32>&& gives, std::vector<uint32>&& ends) -> bool
        {
            if (gives.empty() && ends.empty())
                return true;
            Site s;
            s.Key = { kind, d.spawnId };
            s.Entry = d.id;
            s.X = d.spawnPoint.GetPositionX();
            s.Y = d.spawnPoint.GetPositionY();
            s.Z = d.spawnPoint.GetPositionZ();
            s.PhaseUseFlags = d.phaseUseFlags;
            s.PhaseId = uint16(d.phaseId);
            s.PhaseGroup = d.phaseGroup;
            s.TerrainSwap = d.terrainSwapMap;
            s.LeafArea = sTerrainMgr.GetAreaId(base, ZoneMap, s.X, s.Y, s.Z);
            s.ZoneArea = ZoneAreaOf(s.LeafArea);
            if (!s.LeafArea || !s.ZoneArea)
            {
                why = Trinity::StringFormat("точка {}:{} ({} {:.0f} {:.0f}): зона по рельефу {} / {}", kind ? "go" : "c",
                    d.spawnId, d.id, s.X, s.Y, s.LeafArea, s.ZoneArea);
                return false;
            }
            s.Gives = std::move(gives);
            s.Ends = std::move(ends);
            std::sort(s.Gives.begin(), s.Gives.end());
            std::sort(s.Ends.begin(), s.Ends.end());
            _siteIndex[s.Key] = _sites.size();
            _sites.push_back(std::move(s));
            return true;
        };

        _sites.clear();
        _siteIndex.clear();
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            if (data.mapId != ZoneMap)
                continue;
            std::vector<uint32> gives, ends;
            for (uint32 q : sObjectMgr->GetCreatureQuestRelations(data.id))
                if (_zoneQuests.count(q))
                    gives.push_back(q);
            for (uint32 q : sObjectMgr->GetCreatureQuestInvolvedRelations(data.id))
                if (_zoneQuests.count(q))
                    ends.push_back(q);
            if (!addSite(0, data, std::move(gives), std::move(ends)))
                return false;
        }
        for (auto const& [spawnId, data] : sObjectMgr->GetAllGameObjectData())
        {
            if (data.mapId != ZoneMap)
                continue;
            std::vector<uint32> gives, ends;
            for (uint32 q : sObjectMgr->GetGOQuestRelations(data.id))
                if (_zoneQuests.count(q))
                    gives.push_back(q);
            for (uint32 q : sObjectMgr->GetGOQuestInvolvedRelations(data.id))
                if (_zoneQuests.count(q))
                    ends.push_back(q);
            if (!addSite(1, data, std::move(gives), std::move(ends)))
                return false;
        }
        if (_sites.empty())
        {
            why = "ни одной точки квестодателя на карте зон";
            return false;
        }
        return true;
    }

    // §5⁗ condition dependencies. ConditionMgr exposes no enumerator for quest-availability rows
    // (only Has*/IsObjectMeeting*), so the rows are read once, here, from the same table the core
    // loaded: source 19 = CONDITION_SOURCE_TYPE_QUEST_AVAILABLE; types 8/9/14/43/47 name a quest in
    // ConditionValue1 (QUESTREWARDED, QUESTTAKEN, QUEST_NONE, DAILY_QUEST_DONE, QUESTSTATE).
    bool Planner::LoadConditionDeps(std::string& why)
    {
        QueryResult rows = WorldDatabase.Query(
            "SELECT SourceEntry, ConditionValue1 FROM conditions WHERE SourceTypeOrReferenceId = 19 AND ConditionTypeOrReference IN (8, 9, 14, 43, 47)");
        uint32 n = 0;
        if (rows)
        {
            do
            {
                Field* f = rows->Fetch();
                uint32 const dependent = f[0].GetUInt32();
                uint32 const on = f[1].GetUInt32();
                if (_zoneQuests.count(dependent) && _zoneQuests.count(on))
                    { _deps[on].insert(dependent); ++n; }
            } while (rows->NextRow());
        }
        TC_LOG_INFO("server.loading", "Constellation ПЛАН: условий-зависимостей между квестами зон {}", n);
        (void)why;
        return true;
    }

    // §2″: per quest zone; giver sites only; edges by (d², a.kind, a.spawn, b.kind, b.spawn);
    // union-find in that order with the box cap, skip-not-retry; ids per zone by minimum site key;
    // bounds = extent + 100 (v4 §2). Terrain-swap sites never join (§10‴).
    bool Planner::Partition(std::string& why)
    {
        _hubs.clear();
        _hubOf.clear();
        for (int32 zone : ZoneSorts)
        {
            std::vector<size_t> givers;
            for (size_t i = 0; i < _sites.size(); ++i)
            {
                Site const& s = _sites[i];
                if (s.TerrainSwap != -1)
                    continue;
                bool inZone = false;
                for (uint32 q : s.Gives)
                    if (_sortOf[q] == zone)
                        { inZone = true; break; }
                if (inZone)
                    givers.push_back(i);
            }
            std::sort(givers.begin(), givers.end(), [&](size_t a, size_t b) { return _sites[a].Key < _sites[b].Key; });

            std::vector<Pt> pts;
            for (size_t i : givers)
                pts.push_back({ _sites[i].Key, _sites[i].X, _sites[i].Y, _sites[i].ZoneArea });
            std::vector<std::vector<size_t>> ordered = Components(pts);

            uint16 next = 1;
            for (auto const& membersPt : ordered)
            {
                std::vector<size_t> members;
                for (size_t p : membersPt)
                    members.push_back(givers[p]);
                Hub h;
                h.Ref = { zone, next++ };
                h.RoadStop = members.size() == 1;
                h.MinX = h.MaxX = _sites[members.front()].X;
                h.MinY = h.MaxY = _sites[members.front()].Y;
                for (size_t i : members)
                {
                    h.Sites.push_back(_sites[i].Key);
                    _hubOf[{ zone, _sites[i].Key }] = h.Ref;
                    h.MinX = std::min(h.MinX, _sites[i].X); h.MaxX = std::max(h.MaxX, _sites[i].X);
                    h.MinY = std::min(h.MinY, _sites[i].Y); h.MaxY = std::max(h.MaxY, _sites[i].Y);
                }
                h.MinX -= BoundsPad; h.MaxX += BoundsPad; h.MinY -= BoundsPad; h.MaxY += BoundsPad;
                _hubs.push_back(std::move(h));
            }
        }
        if (_hubs.empty())
        {
            why = "ни одного хаба";
            return false;
        }
        return true;
    }

    // v5 fixtures, closed: exact membership, exact bounds (±1 yard), Falkhaan a road stop.
    bool Planner::CheckFixtures(std::string& why)
    {
        for (Fixture const& f : Fixtures)
        {
            auto it = _hubOf.find({ f.Zone, SiteKey{ 0, f.Spawns.front() } });
            if (it == _hubOf.end())
            {
                why = Trinity::StringFormat("образец зоны {}: точка {} не в разбиении", f.Zone, f.Spawns.front());
                return false;
            }
            Hub const* h = FindHub(it->second);
            std::set<ObjectGuid::LowType> expect(f.Spawns.begin(), f.Spawns.end()), got;
            for (SiteKey const& k : h->Sites)
                got.insert(k.Spawn);
            if (expect != got)
            {
                std::string members;
                for (ObjectGuid::LowType s : got)
                    members += Trinity::StringFormat("{} ", s);
                why = Trinity::StringFormat("образец зоны {}: состав хаба {} = [{}], ожидалось {} точек", f.Zone, h->Ref.Id, members, uint32(expect.size()));
                return false;
            }
            if (std::abs(h->MinX - f.MinX) > 1.f || std::abs(h->MaxX - f.MaxX) > 1.f || std::abs(h->MinY - f.MinY) > 1.f || std::abs(h->MaxY - f.MaxY) > 1.f)
            {
                why = Trinity::StringFormat("образец зоны {}: границы x [{:.0f}, {:.0f}] y [{:.0f}, {:.0f}], ожидалось x [{:.0f}, {:.0f}] y [{:.0f}, {:.0f}]",
                    f.Zone, h->MinX, h->MaxX, h->MinY, h->MaxY, f.MinX, f.MaxX, f.MinY, f.MaxY);
                return false;
            }
        }
        auto fk = _hubOf.find({ 12, SiteKey{ 0, FalkhaanSpawn } });
        if (fk == _hubOf.end() || !FindHub(fk->second)->RoadStop)
        {
            why = "образец: Falkhaan (452570) должен быть обочиной зоны 12";
            return false;
        }
        return true;
    }

    // §5⁗ static dependencies from the templates, over all zone quests (not one hub's).
    void Planner::BuildDeps()
    {
        for (uint32 o : _zoneQuests)
        {
            Quest const* oq = sObjectMgr->GetQuestTemplate(o);
            if (!oq)
                continue;
            if (uint32 prev = uint32(std::abs(oq->GetPrevQuestId())))
                if (_zoneQuests.count(prev))
                    _deps[prev].insert(o);
            if (uint32 next = oq->GetNextQuestId())
                if (_zoneQuests.count(next))
                    { _deps[next].insert(o); _deps[o].insert(next); }
            if (uint32 crumb = uint32(std::abs(oq->GetBreadcrumbForQuestId())))
                if (_zoneQuests.count(crumb))
                    { _deps[crumb].insert(o); _deps[o].insert(crumb); }
            if (oq->GetExclusiveGroup())
                for (uint32 p : _zoneQuests)
                    if (p != o)
                        if (Quest const* pq = sObjectMgr->GetQuestTemplate(p))
                            if (pq->GetExclusiveGroup() == oq->GetExclusiveGroup())
                                _deps[o].insert(p);
        }
    }

    // v4 §6 as data (no phase): Local if any ender is inside the hub bounds; Transition if none
    // inside and one inside another hub's bounds (road stops included, they have ids); else Unknown.
    Leaves Planner::LeavesData(uint32 quest, Hub const& hub) const
    {
        bool inside = false, elsewhere = false;
        for (Site const& s : _sites)
        {
            if (!std::binary_search(s.Ends.begin(), s.Ends.end(), quest))
                continue;
            if (Inside(s, hub))
                { inside = true; continue; }
            for (Hub const& other : _hubs)
                if (!(other.Ref == hub.Ref) && Inside(s, other))
                    { elsewhere = true; break; }
        }
        if (inside) return Leaves::Local;
        if (elsewhere) return Leaves::Transition;
        return Leaves::Unknown;
    }

    // §3′ the same at decision time, enders phase-checked for this companion.
    Leaves Planner::LeavesNow(Player* self, uint32 quest, Hub const& hub) const
    {
        bool inside = false, elsewhere = false;
        for (Site const& s : _sites)
        {
            if (!std::binary_search(s.Ends.begin(), s.Ends.end(), quest))
                continue;
            if (!PhasingHandler::InDbPhaseShift(self, s.PhaseUseFlags, s.PhaseId, s.PhaseGroup))
                continue;
            if (Inside(s, hub))
                { inside = true; continue; }
            for (Hub const& other : _hubs)
                if (!(other.Ref == hub.Ref) && Inside(s, other))
                    { elsewhere = true; break; }
        }
        if (inside) return Leaves::Local;
        if (elsewhere) return Leaves::Transition;
        return Leaves::Unknown;
    }

    // §4′ static gates are the two masks and nothing else; everything else starts Deferred(unvisited).
    // Quests reachable only from a terrain-swap site are unsupported(terrain_swap), hub 0.
    void Planner::BuildSkeletons()
    {
        _skeletons.clear();
        for (uint8 cls = 1; cls < MAX_CLASSES; ++cls)
        {
            if (!sObjectMgr->GetPlayerInfo(RACE_HUMAN, cls))
                continue;
            Skeleton sk;
            sk.Race = RACE_HUMAN;
            sk.Class = cls;
            auto classify = [&](Quest const* q, ObligationRow& row)
            {
                if (!q->GetAllowableRaces().HasRace(RACE_HUMAN))
                    { row.State = Obligation::Ineligible; row.Reason = "race"; }
                else if (q->GetAllowableClasses() && !(q->GetAllowableClasses() & (1u << (cls - 1))))
                    { row.State = Obligation::Ineligible; row.Reason = "class"; }
                else
                    { row.State = Obligation::Deferred; row.Reason = "unvisited"; }
            };
            for (Hub const& h : _hubs)
            {
                std::set<uint32> quests;
                for (SiteKey const& k : h.Sites)
                    for (uint32 q : _sites[_siteIndex.at(k)].Gives)
                        if (_sortOf[q] == h.Ref.Zone)
                            quests.insert(q);
                for (uint32 qid : quests)
                {
                    Quest const* q = sObjectMgr->GetQuestTemplate(qid);
                    if (!q)
                        continue;
                    ObligationRow row;
                    row.Quest = qid;
                    row.Hub = h.Ref;
                    row.ExclusiveGroup = q->GetExclusiveGroup();
                    row.Leave = LeavesData(qid, h);
                    classify(q, row);
                    sk.Rows.push_back(std::move(row));
                }
            }
            // §4′ order: the masks first, then unsupported(terrain_swap); one row per (zone, quest).
            std::set<std::pair<int32, uint32>> swapSeen;
            for (Site const& s : _sites)
            {
                if (s.TerrainSwap == -1)
                    continue;
                for (uint32 qid : s.Gives)
                {
                    Quest const* q = sObjectMgr->GetQuestTemplate(qid);
                    if (!q || !swapSeen.insert({ _sortOf[qid], qid }).second)
                        continue;
                    ObligationRow row;
                    row.Quest = qid;
                    row.Hub = { _sortOf[qid], 0 };
                    row.ExclusiveGroup = q->GetExclusiveGroup();
                    classify(q, row);
                    if (row.State == Obligation::Deferred)
                        { row.State = Obligation::Unsupported; row.Reason = "terrain_swap"; }
                    sk.Rows.push_back(std::move(row));
                }
            }
            std::sort(sk.Rows.begin(), sk.Rows.end(), [](ObligationRow const& a, ObligationRow const& b)
                { return !(a.Hub == b.Hub) ? a.Hub < b.Hub : a.Quest < b.Quest; });
            _skeletons.push_back(std::move(sk));
        }
    }

    // §7″/§10″: one transaction per build, committed synchronously, then counted back — an
    // asynchronous commit cannot fail the build, so the build proves its rows exist.
    bool Planner::PersistObligations(std::string& why)
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        uint32 rows = 0;
        for (Skeleton const& sk : _skeletons)
            for (ObligationRow const& r : sk.Rows)
            {
                trans->Append(Trinity::StringFormat(
                    "INSERT INTO constellation.plan_obligation (build_id, built_at, race, class, zone, hub, quest, obligation, reason, exclusive_group, leaves) "
                    "VALUES ({}, FROM_UNIXTIME({}), {}, {}, {}, {}, {}, '{}', '{}', {}, '{}')",
                    _buildId, _buildId / 1000000, uint32(sk.Race), uint32(sk.Class), r.Hub.Zone, uint32(r.Hub.Id), r.Quest,
                    ObligationName(r.State), Escaped(r.Reason), r.ExclusiveGroup, LeavesName(r.Leave)).c_str());
                ++rows;
            }
        if (!rows)
        {
            why = "ни одной строки обязательств";
            return false;
        }
        CharacterDatabase.DirectCommitTransaction(trans);
        QueryResult back = CharacterDatabase.Query(Trinity::StringFormat(
            "SELECT COUNT(*) FROM constellation.plan_obligation WHERE build_id = {}", _buildId).c_str());
        uint64 const landed = back ? back->Fetch()[0].GetUInt64() : 0;
        if (landed != rows)
        {
            why = Trinity::StringFormat("обязательств записано {} из {} (сборка {})", landed, rows, _buildId);
            return false;
        }
        TC_LOG_INFO("server.loading", "Constellation ПЛАН: обязательств записано {} (сборка {}), проверено счётом", rows, _buildId);
        return true;
    }

    // ------------------------------------------------------------------ lookups

    Site const* Planner::FindSite(SiteKey key) const
    {
        auto it = _siteIndex.find(key);
        return it == _siteIndex.end() ? nullptr : &_sites[it->second];
    }

    Hub const* Planner::FindHub(HubRef ref) const
    {
        for (Hub const& h : _hubs)
            if (h.Ref == ref)
                return &h;
        return nullptr;
    }

    std::vector<SiteKey> Planner::Givers(uint32 quest, Hub const& hub) const
    {
        std::vector<SiteKey> out;
        for (SiteKey const& k : hub.Sites)
            if (Site const* s = FindSite(k))
                if (std::binary_search(s->Gives.begin(), s->Gives.end(), quest))
                    out.push_back(k);
        return out;
    }

    std::vector<SiteKey> Planner::EndersInside(uint32 quest, Hub const& hub) const
    {
        std::vector<SiteKey> out;
        for (Site const& s : _sites)
            if (std::binary_search(s.Ends.begin(), s.Ends.end(), quest) && Inside(s, hub))
                out.push_back(s.Key);
        return out;
    }

    // ------------------------------------------------------------------ runtime

    bool Planner::InScope(Player* self) const
    {
        return _built && self && self->IsInWorld() && self->GetRace() == RACE_HUMAN && self->GetMapId() == ZoneMap;
    }

    Skeleton const* Planner::SkeletonFor(Player* self) const
    {
        for (Skeleton const& sk : _skeletons)
            if (sk.Race == self->GetRace() && sk.Class == self->GetClass())
                return &sk;
        return nullptr;
    }

    CompanionPlan& Planner::StateOf(Player* self)
    {
        CompanionPlan& cp = _state[self->GetGUID()];
        if (!cp.Skel)
        {
            cp.Skel = SkeletonFor(self);
            if (cp.Skel)
                for (ObligationRow const& r : cp.Skel->Rows)
                    if (!cp.Notes.count(r.Quest))
                        cp.Notes[r.Quest] = Note{ InitialAnnotation(r.State), r.Reason, 0 };
        }
        return cp;
    }

    // §3‴ over what CanSee and GetTerrainMapId read. The shift's own Flags are protected in this
    // core (friend PhasingHandler only); the paths that set them for a player are SetGameMaster
    // (Player.cpp:2077) and SPELL_AURA_PHASE_ALWAYS_VISIBLE (Player.cpp:2082,
    // SpellAuraEffects.cpp:1910), so those two are read in their place; Inverse and NoCosmetic
    // have no player-side setter in the fork.
    size_t Planner::FingerprintOf(Player* self) const
    {
        size_t h = 0x9e3779b97f4a7c15ULL;
        auto mix = [&h](size_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
        PhaseShift const& ps = self->GetPhaseShift();
        for (PhaseShift::PhaseRef const& p : ps.GetPhases())
        {
            mix(size_t(p.Id));
            mix(size_t(p.Flags.AsUnderlyingType()));
        }
        mix(std::hash<ObjectGuid>()(ps.GetPersonalGuid()));
        for (auto const& [mapId, ref] : ps.GetVisibleMapIds())
            mix(size_t(mapId) * 31);
        mix(self->IsGameMaster() ? 7 : 3);
        mix(self->HasAuraType(SPELL_AURA_PHASE_ALWAYS_VISIBLE) ? 11 : 5);
        return h;
    }

    // Data-driven consultation (§5‴): a giver site the companion cannot open still gets its entry,
    // from InDbPhaseShift alone. Also the per-quest phase-miss accounting of §3′.
    void Planner::ConsultIncompatible(Player* self, CompanionPlan& cp, Hub const& hub)
    {
        for (SiteKey const& k : hub.Sites)
        {
            Site const* s = FindSite(k);
            if (!s || cp.SiteSeen.count(k))
                continue;
            if (PhasingHandler::InDbPhaseShift(self, s->PhaseUseFlags, s->PhaseId, s->PhaseGroup))
                continue;
            Seen& seen = cp.SiteSeen[k];
            seen.Tick = NowSeconds();
            seen.Compatible = false;
            seen.Offered.clear();
        }
    }

    // Hub arrival: Current changes, snapshots cleared, statuses baselined, phase misses counted.
    void Planner::Arrive(Player* self, CompanionPlan& cp, Hub const& hub)
    {
        cp.Current = hub.Ref;
        cp.SiteSeen.clear();
        cp.LastStatus.clear();
        if (!cp.Skel)
            return;
        for (ObligationRow const& r : cp.Skel->Rows)
        {
            if (!(r.Hub == hub.Ref))
                continue;
            cp.LastStatus[r.Quest] = StatusWord(self, r.Quest);
            Note const n = cp.Notes[r.Quest];                // a copy: Annotate rewrites the note
            if (n.State != Annotation::Deferred && n.State != Annotation::Required)
                continue;                                   // taken/done/ineligible/unsupported: no phase state
            bool anyCompatible = false;
            for (SiteKey const& k : Givers(r.Quest, hub))
                if (Site const* s = FindSite(k))
                    if (PhasingHandler::InDbPhaseShift(self, s->PhaseUseFlags, s->PhaseId, s->PhaseGroup))
                        { anyCompatible = true; break; }
            if (anyCompatible)
            {
                if (n.PhaseMisses || n.Reason == "phase")        // §3′ lifted, persisted
                    Annotate(self, cp, r.Quest, n.State, n.Reason == "phase" ? "unvisited" : n.Reason, 0);
            }
            else if (n.PhaseMisses + 1 >= PhaseMissLimit)
                Annotate(self, cp, r.Quest, Annotation::Deferred, "phase", PhaseMissLimit);
            else
                Annotate(self, cp, r.Quest, n.State, n.Reason, uint8(n.PhaseMisses + 1));
        }
        ConsultIncompatible(self, cp, hub);
        TC_LOG_INFO("server.worldserver", "Constellation ПЛАН {}: прибыл в хаб {}/{}", self->GetName(), hub.Ref.Zone, hub.Ref.Id);
    }

    // §5⁗ re-read every obligation status of the current hub; any change erases the acting quest's
    // giver and ender entries, then the dependency-scoped set. Fingerprint first (§3‴).
    void Planner::Observe(Player* self, CompanionPlan& cp)
    {
        Hub const* hub = FindHub(cp.Current);
        size_t const fp = FingerprintOf(self);
        if (fp != cp.Fingerprint)
        {
            if (cp.Fingerprint)
            {
                TC_LOG_INFO("server.worldserver", "Constellation ПЛАН {}: фаза сменилась — снимки хаба {}/{} сброшены", self->GetName(), cp.Current.Zone, cp.Current.Id);
                cp.SiteSeen.clear();
                // §3‴ the reset is a state change: persisted, and a "phase" deferral lifted to unvisited
                if (cp.Skel)
                    for (ObligationRow const& r : cp.Skel->Rows)
                    {
                        Note const n = cp.Notes[r.Quest];
                        if (n.PhaseMisses || n.Reason == "phase")
                            Annotate(self, cp, r.Quest, n.State, n.Reason == "phase" ? "unvisited" : n.Reason, 0);
                    }
            }
            cp.Fingerprint = fp;
        }
        if (!hub || !cp.Skel)
            return;
        for (ObligationRow const& r : cp.Skel->Rows)
        {
            if (!(r.Hub == hub->Ref))
                continue;
            uint32 const q = r.Quest;
            uint8 const now = StatusWord(self, q);
            auto it = cp.LastStatus.find(q);
            bool const changed = it != cp.LastStatus.end() && it->second != now;
            cp.LastStatus[q] = now;
            if (!changed)
                continue;
            std::set<SiteKey> erase;
            for (SiteKey const& k : Givers(q, *hub)) erase.insert(k);
            for (SiteKey const& k : EndersInside(q, *hub)) erase.insert(k);
            auto dep = _deps.find(q);
            if (dep != _deps.end())
                for (uint32 other : dep->second)
                    for (SiteKey const& k : Givers(other, *hub))
                        erase.insert(k);
            for (SiteKey const& k : erase)
                cp.SiteSeen.erase(k);
        }
        ConsultIncompatible(self, cp, *hub);
    }

    // §5‴ the consultation: the entry is written here, before the module asks for the menu.
    void Planner::OnConsult(Player* self, Creature* giver)
    {
        if (!InScope(self) || !giver)
            return;
        Site const* site = FindSite({ 0, giver->GetSpawnId() });
        if (!site || site->Gives.empty())
            return;
        CompanionPlan& cp = StateOf(self);
        // Which hub this consultation belongs to (Codex, eleventh pass): the exact current hub
        // if the site is its member; else the single non-road-stop partition the site is in; else
        // the single remaining one; on ambiguity Current stays and the site is still recorded.
        HubRef ref;
        bool ambiguous = false;
        if (cp.Current)
        {
            auto here = _hubOf.find({ cp.Current.Zone, site->Key });
            if (here != _hubOf.end() && here->second == cp.Current)
                ref = cp.Current;
        }
        if (!ref)
        {
            std::vector<HubRef> hubs, stops;
            for (int32 zone : ZoneSorts)
            {
                auto it = _hubOf.find({ zone, site->Key });
                if (it == _hubOf.end())
                    continue;
                if (Hub const* h = FindHub(it->second))
                    (h->RoadStop ? stops : hubs).push_back(it->second);
            }
            if (hubs.size() == 1)
                ref = hubs.front();
            else if (hubs.size() > 1)
                ambiguous = true;
            else if (stops.size() == 1)
                ref = stops.front();
            else if (!stops.empty())
                ambiguous = true;
        }
        Hub const* hub = ref ? FindHub(ref) : nullptr;
        if (!hub && !ambiguous)
            return;
        if (hub && !hub->RoadStop && !(cp.Current == hub->Ref))
            Arrive(self, cp, *hub);
        Observe(self, cp);
        Seen& seen = cp.SiteSeen[site->Key];
        seen.Tick = NowSeconds();
        seen.Compatible = PhasingHandler::InDbPhaseShift(self, site->PhaseUseFlags, site->PhaseId, site->PhaseGroup);
        seen.Offered.clear();
        if (seen.Compatible)                                // §3′ a compatible giver lifts deferred(phase) at once
            for (uint32 q : site->Gives)
            {
                auto nt = cp.Notes.find(q);
                if (nt != cp.Notes.end() && (nt->second.PhaseMisses || nt->second.Reason == "phase"))
                {
                    Note const n = nt->second;
                    Annotate(self, cp, q, n.State, n.Reason == "phase" ? "unvisited" : n.Reason, 0);
                }
            }
    }

    // The decision point: the menu the core built, against the mirror.
    void Planner::OnMenuRead(Player* self, Creature* giver, QuestMenu const& menu, GateFn const& gate)
    {
        if (!InScope(self) || !giver)
            return;
        Site const* site = FindSite({ 0, giver->GetSpawnId() });
        if (!site || site->Gives.empty())
            return;
        CompanionPlan& cp = StateOf(self);
        if (!cp.SiteSeen.count(site->Key))
            OnConsult(self, giver);                         // a read without a consultation: consult now
        Seen& seen = cp.SiteSeen[site->Key];
        seen.Offered.clear();
        for (uint8 i = 0; i < menu.GetMenuItemCount(); ++i)
            seen.Offered[menu.GetItem(i).QuestId] = menu.GetItem(i).QuestIcon;
        Audit(self, cp, *site, gate);
        Hub const* hub = FindHub(cp.Current);
        if (hub && !hub->RoadStop)
            Exhausted(self, cp, *hub, gate);
    }

    // The audit (v4): for each zone quest this site gives, the real menu against the mirror.
    // Mirror = Player::PrepareQuestMenu, starter branch: CanTakeQuest, then a turn-in quest always,
    // a normal one only at status NONE, with the icon the core assigns. An entry that comes from the
    // involved branch (this site also ends a quest the companion carries) is not a starter offer.
    void Planner::Audit(Player* self, CompanionPlan& cp, Site const& site, GateFn const& gate)
    {
        Seen const& seen = cp.SiteSeen[site.Key];
        for (uint32 qid : site.Gives)
        {
            Quest const* q = sObjectMgr->GetQuestTemplate(qid);
            if (!q || !cp.Notes.count(qid))
                continue;
            Note const& n = cp.Notes[qid];
            if (n.State == Annotation::Ineligible || n.State == Annotation::Unsupported)
                continue;
            bool const endsHere = std::binary_search(site.Ends.begin(), site.Ends.end(), qid);
            if (endsHere && InLog(self, qid))
                continue;                                   // the involved branch owns this entry
            bool const core = self->CanTakeQuest(q, false);
            bool const mirror = seen.Compatible && core && (q->IsTurnIn() || self->GetQuestStatus(qid) == QUEST_STATUS_NONE);
            uint8 const icon = StarterIcon(q);
            auto off = seen.Offered.find(qid);
            bool const present = off != seen.Offered.end();     // presence and icon are two facts
            bool const iconOk = present && off->second == icon;
            if (present == mirror && (!present || iconOk))
            {
                if (present && n.State == Annotation::Deferred)
                    Annotate(self, cp, qid, Annotation::Required, "", 0);
                continue;
            }
            char const* g = gate(self, q);
            std::string note = g ? g : "?";
            if (present && !iconOk)
                note += Trinity::StringFormat(" | icon {} != {}", uint32(off->second), uint32(icon));
            Mismatch(self, qid, site, present, note);
            if (present && n.State == Annotation::Deferred)
                Annotate(self, cp, qid, Annotation::Required, "menu", 0);   // behaviour follows the menu
        }
    }

    void Planner::Mismatch(Player* self, uint32 quest, Site const& site, bool offered, std::string const& gate)
    {
        CharacterDatabase.Execute(Trinity::StringFormat(
            "INSERT INTO constellation.plan_mismatch (seen_at, char_guid, char_name, quest, site_kind, site_entry, site_spawn, menu_offers, mirror_gate) "
            "VALUES (NOW(3), {}, '{}', {}, '{}', {}, {}, {}, '{}')",
            self->GetGUID().GetCounter(), Escaped(self->GetName()), quest, site.Key.Kind ? "gameobject" : "creature", site.Entry, site.Key.Spawn,
            offered ? 1 : 0, Escaped(gate)).c_str());
        TC_LOG_INFO("server.worldserver", "Constellation ПЛАН {}: расхождение — квест {} у {}:{} меню {} зеркало «{}»",
            self->GetName(), quest, site.Key.Kind ? "go" : "c", site.Key.Spawn, offered ? "даёт" : "не даёт", gate);
    }

    void Planner::Annotate(Player* self, CompanionPlan& cp, uint32 quest, Annotation state, std::string const& reason, uint8 misses)
    {
        Note& n = cp.Notes[quest];
        if (n.State == state && n.Reason == reason && n.PhaseMisses == misses)
            return;
        n.State = state;
        n.Reason = reason;
        n.PhaseMisses = misses;
        CharacterDatabase.Execute(Trinity::StringFormat(
            "INSERT INTO constellation.plan_annotation (char_guid, quest, state, reason, hub, phase_misses, updated_at) "
            "VALUES ({}, {}, '{}', '{}', {}, {}, NOW(3)) "
            "ON DUPLICATE KEY UPDATE state = VALUES(state), reason = VALUES(reason), hub = VALUES(hub), phase_misses = VALUES(phase_misses), updated_at = VALUES(updated_at)",
            self->GetGUID().GetCounter(), quest, AnnotationName(state), Escaped(reason), uint32(cp.Current.Id), uint32(misses)).c_str());
    }

    // v6 §5″ / v7 §5‴: a quest is absent only when every giver site has an entry and each is
    // incompatible or does not offer it; the deferral carries the mirror's gate, or
    // "unexplained" plus a mismatch row when the mirror says takeable. The hub is exhausted when
    // every giver site has an entry and no obligation is open. Stage 1 logs the outcome and Leaves.
    void Planner::Exhausted(Player* self, CompanionPlan& cp, Hub const& hub, GateFn const& gate)
    {
        if (!cp.Skel)
            return;
        for (SiteKey const& k : hub.Sites)
            if (!cp.SiteSeen.count(k))
                return;
        uint32 done = 0, deferred = 0, open = 0;
        std::vector<uint32> carried;
        for (ObligationRow const& r : cp.Skel->Rows)
        {
            if (!(r.Hub == hub.Ref))
                continue;
            uint32 const q = r.Quest;
            Note const& n = cp.Notes[q];
            if (n.State == Annotation::Ineligible || n.State == Annotation::Unsupported)
                continue;
            if (self->GetQuestRewardStatus(q))
                { ++done; Annotate(self, cp, q, Annotation::Done, "", 0); continue; }
            if (InLog(self, q))
                { ++open; carried.push_back(q); Annotate(self, cp, q, Annotation::Taken, "", 0); continue; }
            bool absent = true;
            for (SiteKey const& k : Givers(q, hub))
            {
                auto it = cp.SiteSeen.find(k);
                if (it == cp.SiteSeen.end() || (it->second.Compatible && it->second.Offered.count(q)))
                    { absent = false; break; }
            }
            if (!absent)
                { ++open; continue; }
            if (n.State == Annotation::Deferred && n.Reason == "phase")
                { ++deferred; continue; }
            Quest const* quest = sObjectMgr->GetQuestTemplate(q);
            if (!quest)
                continue;
            if (self->CanTakeQuest(quest, false))
            {
                std::vector<SiteKey> givers = Givers(q, hub);
                if (!givers.empty())
                    if (Site const* s = FindSite(givers.front()))
                        Mismatch(self, q, *s, false, gate(self, quest));
                Annotate(self, cp, q, Annotation::Deferred, "unexplained", n.PhaseMisses);
            }
            else
            {
                char const* g = gate(self, quest);
                Annotate(self, cp, q, Annotation::Deferred, g ? g : "?", n.PhaseMisses);
            }
            ++deferred;
        }
        if (open && carried.size() != open)
            return;                                         // something is still takeable here
        if (cp.ExhaustedLogged.count(hub.Ref))
            return;
        cp.ExhaustedLogged.insert(hub.Ref);
        std::string leaves;
        for (uint32 q : carried)
            leaves += Trinity::StringFormat("{}:{} ", q, LeavesName(LeavesNow(self, q, hub)));
        TC_LOG_INFO("server.worldserver", "Constellation ПЛАН {}: хаб {}/{} исчерпан — сдано {}, отложено {}, в журнале {} [{}]",
            self->GetName(), hub.Ref.Zone, hub.Ref.Id, done, deferred, uint32(carried.size()), leaves);
    }

    void Planner::OnTakeOrTurnIn(Player* self)
    {
        if (!InScope(self))
            return;
        CompanionPlan& cp = StateOf(self);
        if (!cp.Current)
            return;
        Observe(self, cp);
    }

    // §8″/§8‴: one synchronous SELECT, corrections in memory, one transaction; timed.
    // lazy: synchronous on purpose — 122 companions, one indexed read each, p99 bound 8 ms measured
    // by the histogram below; the upgrade path is CharacterDatabase.AsyncQuery with the callback
    // on the world thread and plan decisions held until it lands, if the bound is ever exceeded.
    void Planner::OnLogin(Player* self)
    {
        if (!InScope(self))
            return;
        CompanionPlan& cp = StateOf(self);
        if (cp.Reconciled || !cp.Skel)
            return;
        auto const t0 = std::chrono::steady_clock::now();
        std::set<uint32> known;
        for (ObligationRow const& r : cp.Skel->Rows)
            known.insert(r.Quest);
        QueryResult rows = CharacterDatabase.Query(Trinity::StringFormat(
            "SELECT quest, state, reason, hub, phase_misses FROM constellation.plan_annotation WHERE char_guid = {} ORDER BY quest LIMIT {}",
            self->GetGUID().GetCounter(), ReadCap).c_str());
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        uint32 loaded = 0, corrected = 0, dropped = 0;
        if (rows)
        {
            do
            {
                Field* f = rows->Fetch();
                uint32 const quest = f[0].GetUInt32();
                Annotation stored;
                if (!ParseAnnotation(f[1].GetString(), stored))
                    continue;
                ++loaded;
                if (!known.count(quest))
                {
                    ++dropped;
                    trans->Append(Trinity::StringFormat("DELETE FROM constellation.plan_annotation WHERE char_guid = {} AND quest = {}",
                        self->GetGUID().GetCounter(), quest).c_str());
                    continue;
                }
                Note n;
                n.State = stored;
                n.Reason = f[2].GetString();
                n.PhaseMisses = f[4].GetUInt8();
                Note actual = n;
                if (self->GetQuestRewardStatus(quest))
                    { actual.State = Annotation::Done; actual.Reason.clear(); }
                else if (InLog(self, quest))
                    { actual.State = Annotation::Taken; actual.Reason.clear(); }
                else if (stored == Annotation::Deferred && n.Reason == "phase")
                {
                    // §3′ a stored deferred(phase) is re-checked once against the phase now
                    for (Hub const& h : _hubs)
                        for (SiteKey const& k : Givers(quest, h))
                            if (Site const* s = FindSite(k))
                                if (PhasingHandler::InDbPhaseShift(self, s->PhaseUseFlags, s->PhaseId, s->PhaseGroup))
                                    { actual.Reason = "unvisited"; actual.PhaseMisses = 0; }
                }
                cp.Notes[quest] = actual;
                if (actual.State != n.State || actual.Reason != n.Reason || actual.PhaseMisses != n.PhaseMisses)
                {
                    ++corrected;
                    trans->Append(Trinity::StringFormat(
                        "UPDATE constellation.plan_annotation SET state = '{}', reason = '{}', phase_misses = {}, updated_at = NOW(3) WHERE char_guid = {} AND quest = {}",
                        AnnotationName(actual.State), Escaped(actual.Reason), uint32(actual.PhaseMisses), self->GetGUID().GetCounter(), quest).c_str());
                }
            } while (rows->NextRow());
        }
        if (corrected || dropped)
            CharacterDatabase.CommitTransaction(trans);      // idempotent: re-derived at the next login if lost
        cp.Reconciled = true;
        cp.Fingerprint = FingerprintOf(self);               // the observation the contract asks for at login
        double const ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++_loginCount;
        _sinceLoginMs = 0;
        _loginMaxMs = std::max(_loginMaxMs, ms);
        size_t bucket = size_t(ms * 10.0);
        if (bucket >= _loginHist.size())
            bucket = _loginHist.size() - 1;
        ++_loginHist[bucket];
        if (loaded || corrected || dropped)
            TC_LOG_INFO("server.worldserver", "Constellation ПЛАН {}: сверка при входе — строк {}, исправлено {}, удалено {}, {:.2f} мс",
                self->GetName(), loaded, corrected, dropped, ms);
    }

    void Planner::OnLogout(ObjectGuid guid)
    {
        _state.erase(guid);
    }

    void Planner::PrintLoginStats(char const* when) const
    {
        if (!_loginCount)
            return;
        uint32 seen = 0;
        double p99 = 0.0;
        for (size_t i = 0; i < _loginHist.size(); ++i)
        {
            seen += _loginHist[i];
            if (seen * 100 >= _loginCount * 99)
                { p99 = double(i) / 10.0; break; }
        }
        TC_LOG_INFO("server.worldserver", "Constellation ПЛАН: сверка при входе ({}) — {} входов, p99 {:.1f} мс, максимум {:.2f} мс (порог p99 8 мс)",
            when, _loginCount, p99, _loginMaxMs);
    }

    // §8‴ the line after Bootstrap (a minute with no further login) and then hourly. A counter
    // of the tick's own diff — no core call, nothing per companion.
    void Planner::Tick(uint32 diff)
    {
        if (!_built || !_loginCount)
            return;
        _sinceLoginMs += diff;
        _sinceStatsMs += diff;
        if (!_bootstrapStatsDone && _sinceLoginMs >= 60000)
        {
            _bootstrapStatsDone = true;
            _sinceStatsMs = 0;
            PrintLoginStats("после входа состава");
        }
        if (_bootstrapStatsDone && _sinceStatsMs >= 3600000)
        {
            _sinceStatsMs = 0;
            PrintLoginStats("час");
        }
    }

    void Planner::OnShutdown()
    {
        PrintLoginStats("остановка");
    }
}
