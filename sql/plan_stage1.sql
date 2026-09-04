-- Constellation: the hub-first quest plan, Stage 1 (humans, Northshire, Goldshire; audit only).
-- Contract: homelab/.agent/design/constellation-plan/plan-spec-v4.md as amended by v5..v8.
-- Additive: three new tables in the module's own schema, plus the grant the module's DB user
-- needs to reach them through its characters connection. Nothing in the core's schemas changes.

CREATE DATABASE IF NOT EXISTS constellation;
GRANT ALL PRIVILEGES ON constellation.* TO 'algalon'@'localhost';

CREATE TABLE IF NOT EXISTS constellation.plan_obligation (
  build_id BIGINT UNSIGNED NOT NULL,            -- microseconds since the Unix epoch at build start
  built_at DATETIME NOT NULL,                   -- informational
  race TINYINT UNSIGNED NOT NULL,
  class TINYINT UNSIGNED NOT NULL,
  zone INT NOT NULL,                            -- QuestSortID of the quest (6170 Northshire, 12 Elwynn)
  hub SMALLINT UNSIGNED NOT NULL,               -- canonical hub id (1..n by minimum site key); road stops numbered too
  quest INT UNSIGNED NOT NULL,
  obligation ENUM('required','alternative','deferred','ineligible','unsupported','skipped') NOT NULL,
  reason VARCHAR(96) NOT NULL DEFAULT '',
  exclusive_group INT NOT NULL DEFAULT 0,
  leaves ENUM('local','transition','unknown') NOT NULL,
  PRIMARY KEY (build_id, race, class, zone, hub, quest)
) ENGINE=InnoDB;
-- one INSERT per (race, class) per module start, in one transaction; never updated.
-- A duplicate key on the first insert of a build aborts the build with one log line.

CREATE TABLE IF NOT EXISTS constellation.plan_mismatch (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  seen_at DATETIME(3) NOT NULL,
  char_guid BIGINT UNSIGNED NOT NULL,
  char_name VARCHAR(24) NOT NULL,
  quest INT UNSIGNED NOT NULL,
  site_kind ENUM('creature','gameobject') NOT NULL,
  site_entry INT UNSIGNED NOT NULL,
  site_spawn BIGINT UNSIGNED NOT NULL,
  menu_offers TINYINT NOT NULL,                 -- 1 the real menu offered it, 0 it did not
  mirror_gate VARCHAR(96) NOT NULL,             -- FirstFailingGate of the mirror (+ ' | icon a != b' on an icon mismatch)
  disposition ENUM('open','mirror_defect','manifest_miss','data_finding') NOT NULL DEFAULT 'open',
  disposition_note VARCHAR(160) NOT NULL DEFAULT '',
  KEY by_quest (quest, seen_at)
) ENGINE=InnoDB;
-- append-only from the module; disposition columns are written by hand during the audit, never by the module.
-- Migration for a table created by the first cut of this file (mirror_gate was VARCHAR(32)); idempotent.
ALTER TABLE constellation.plan_mismatch MODIFY mirror_gate VARCHAR(96) NOT NULL;

CREATE TABLE IF NOT EXISTS constellation.plan_annotation (
  char_guid BIGINT UNSIGNED NOT NULL,
  quest INT UNSIGNED NOT NULL,
  state ENUM('deferred','required','taken','done','alternative','ineligible','unsupported','skipped') NOT NULL,
  reason VARCHAR(96) NOT NULL DEFAULT '',
  hub SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  phase_misses TINYINT UNSIGNED NOT NULL DEFAULT 0,
  updated_at DATETIME(3) NOT NULL,
  PRIMARY KEY (char_guid, quest)
) ENGINE=InnoDB;
-- INSERT ... ON DUPLICATE KEY UPDATE, one statement per state change, from the world thread only.
-- Login reconciliation: one synchronous SELECT, corrections as one transaction; idempotent.
