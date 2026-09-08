/*
 * Constellation — names for the engine's typed ids, and the translation unit that makes the
 * header actually compile. A header nobody includes is not proven by a build.
 *
 * Contract: homelab/.agent/design/constellation-engine/engine-spec-v1.md as amended by v2 and
 * v3. §3.5 is the reason these tables exist: ids are typed so a rename is a build error, and the
 * name survives only for the operator-facing РЕШЕНИЕ line of §5.
 *
 * Copyright (C) 2026 Constellation contributors. Licensed under the GNU AGPL v3 — see COPYING.
 */
#include "Primitives.h"

namespace Constellation::Ai
{
    namespace
    {
        char const* const ACTION_NAMES[] =
        {
#define CONSTELLATION_ACTION_NAME(name, text) text,
            CONSTELLATION_ACTIONS(CONSTELLATION_ACTION_NAME)
#undef CONSTELLATION_ACTION_NAME
        };

        char const* const TRIGGER_NAMES[] =
        {
#define CONSTELLATION_TRIGGER_NAME(name, text) text,
            CONSTELLATION_TRIGGERS(CONSTELLATION_TRIGGER_NAME)
#undef CONSTELLATION_TRIGGER_NAME
        };

        char const* const STRATEGY_NAMES[] =
        {
#define CONSTELLATION_STRATEGY_NAME(name, text) text,
            CONSTELLATION_STRATEGIES(CONSTELLATION_STRATEGY_NAME)
#undef CONSTELLATION_STRATEGY_NAME
        };

        char const* const VALUE_NAMES[] =
        {
#define CONSTELLATION_VALUE_NAME(name, text, type) text,
            CONSTELLATION_VALUES(CONSTELLATION_VALUE_NAME)
#undef CONSTELLATION_VALUE_NAME
        };

        // The X-macro generates the enum and this table from ONE list, so they cannot drift.
        // These assertions are what make that guarantee real rather than a comment.
        static_assert(std::size(ACTION_NAMES)  == size_t(ActionId::Count),
                      "имена действий разошлись с перечислением");
        static_assert(std::size(TRIGGER_NAMES) == size_t(TriggerId::Count),
                      "имена триггеров разошлись с перечислением");
        static_assert(std::size(STRATEGY_NAMES) == size_t(StrategyId::Count),
                      "имена стратегий разошлись с перечислением");
        static_assert(std::size(VALUE_NAMES) == size_t(ValueId::Count),
                      "имена значений разошлись с перечислением");
    }

    char const* NameOf(ActionId id)
    {
        return id < ActionId::Count ? ACTION_NAMES[size_t(id)] : "?";
    }

    char const* NameOf(TriggerId id)
    {
        return id < TriggerId::Count ? TRIGGER_NAMES[size_t(id)] : "?";
    }

    char const* NameOf(ValueId id)
    {
        return id < ValueId::Count ? VALUE_NAMES[size_t(id)] : "?";
    }

    char const* NameOf(StrategyId id)
    {
        return id < StrategyId::Count ? STRATEGY_NAMES[size_t(id)] : "?";
    }
}
