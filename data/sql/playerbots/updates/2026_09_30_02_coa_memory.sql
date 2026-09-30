-- CoA memory: what each bot remembers about the characters it dealt with (AiPlayerbot.CoaMemory.*).
-- One row per (bot, other character) with ten feelings and ten counters, and a short diary of dated events.
-- Written by the worldserver only (write-behind flush); never a player's words. Retention, by default: events
-- 30 days (EventRetentionDays), relations 180 days without contact (RelationRetentionDays). Never dropped.
-- Axes are SMALLINT in -1000..1000 (= -100.0..100.0); familiarity and camaraderie use 0..1000.
-- Decay is applied lazily: v * 0.5^(days_since(decayed_at) / HalfLifeDays.<Axis>).
CREATE TABLE IF NOT EXISTS `playerbots_coa_memory_relation` (
  `bot_guid`      INT UNSIGNED      NOT NULL,
  `other_guid`    INT UNSIGNED      NOT NULL,
  `other_account` INT UNSIGNED      NOT NULL DEFAULT 0,  -- 0 for bots
  `other_is_bot`  TINYINT UNSIGNED  NOT NULL DEFAULT 0,
  `other_name`    VARCHAR(24)       NOT NULL DEFAULT '',
  `affinity`      SMALLINT          NOT NULL DEFAULT 0,  -- half-life 30 d
  `trust`         SMALLINT          NOT NULL DEFAULT 0,  -- 60 d
  `respect`       SMALLINT          NOT NULL DEFAULT 0,  -- 60 d
  `rivalry`       SMALLINT          NOT NULL DEFAULT 0,  -- 10 d
  `gratitude`     SMALLINT          NOT NULL DEFAULT 0,  -- 21 d
  `grudge`        SMALLINT          NOT NULL DEFAULT 0,  -- 14 d
  `fear`          SMALLINT          NOT NULL DEFAULT 0,  -- 7 d
  `annoyance`     SMALLINT          NOT NULL DEFAULT 0,  -- 3 d
  `familiarity`   SMALLINT          NOT NULL DEFAULT 0,  -- 90 d
  `camaraderie`   SMALLINT          NOT NULL DEFAULT 0,  -- 45 d
  `meetings`      SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `chats`         SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `helped_me`     SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `helped_them`   SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `killed_me`     SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `killed_them`   SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `duels_won`     SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `duels_lost`    SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `groups_shared` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `trades`        SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `first_seen`    INT UNSIGNED      NOT NULL,
  `last_seen`     INT UNSIGNED      NOT NULL,
  `decayed_at`    INT UNSIGNED      NOT NULL,
  PRIMARY KEY (`bot_guid`, `other_guid`),
  KEY `idx_other_guid` (`other_guid`),
  KEY `idx_other_account` (`other_account`),
  KEY `idx_last_seen` (`last_seen`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `playerbots_coa_memory_event` (
  `id`            BIGINT UNSIGNED   NOT NULL AUTO_INCREMENT,
  `bot_guid`      INT UNSIGNED      NOT NULL,
  `other_guid`    INT UNSIGNED      NOT NULL DEFAULT 0,
  `other_account` INT UNSIGNED      NOT NULL DEFAULT 0,
  `kind`          TINYINT UNSIGNED  NOT NULL,
  `importance`    TINYINT UNSIGNED  NOT NULL DEFAULT 0,
  `map_id`        SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `zone_id`       INT UNSIGNED      NOT NULL DEFAULT 0,
  `ts`            INT UNSIGNED      NOT NULL,
  `detail`        VARCHAR(160)      NOT NULL DEFAULT '',  -- built by code, never player words
  PRIMARY KEY (`id`),
  KEY `idx_bot_ts` (`bot_guid`, `ts`),
  KEY `idx_other_guid` (`other_guid`),
  KEY `idx_other_account` (`other_account`),
  KEY `idx_ts` (`ts`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
