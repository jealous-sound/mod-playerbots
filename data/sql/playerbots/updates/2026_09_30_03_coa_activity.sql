-- CoA activities: the log of the activity director (AiPlayerbot.CoaActivity.*), one row per activity (duel ring,
-- gang) and one per member of it, bot or real player. Written by the worldserver only; rows are kept LogKeepDays
-- (30 by default). A member row with restored = 0 holds a CoA ruleset (prev_ruleset, an aura id) the director gave
-- away and still has to put back: it is applied at that bot's next login, then the row is marked restored.
-- Without these two tables activities stay off (one error in the log). Never dropped.
CREATE TABLE IF NOT EXISTS `playerbots_coa_activity` (
  `id`          INT UNSIGNED      NOT NULL,              -- allocated in memory after MAX(id)
  `kind`        VARCHAR(24)       NOT NULL,
  `team`        TINYINT UNSIGNED  NOT NULL,
  `target`      VARCHAR(64)       NOT NULL DEFAULT '',
  `leader_guid` INT UNSIGNED      NOT NULL DEFAULT 0,
  `state`       TINYINT UNSIGNED  NOT NULL,              -- 0-6 live phases, 10 done, 11 aborted
  `started`     INT UNSIGNED      NOT NULL,
  `ended`       INT UNSIGNED      NOT NULL DEFAULT 0,
  `outcome`     TINYINT UNSIGNED  NOT NULL DEFAULT 0,
  `bots`        SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `players`     SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `summary`     VARCHAR(255)      NOT NULL DEFAULT '',
  PRIMARY KEY (`id`),
  KEY `idx_state` (`state`),
  KEY `idx_started` (`started`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `playerbots_coa_activity_member` (
  `activity_id`  INT UNSIGNED     NOT NULL,
  `guid`         INT UNSIGNED     NOT NULL,
  `is_bot`       TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `role`         TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `joined`       INT UNSIGNED     NOT NULL,
  `left_at`      INT UNSIGNED     NOT NULL DEFAULT 0,
  `prev_ruleset` INT UNSIGNED     NOT NULL DEFAULT 0,    -- aura id to restore; 0 = unchanged
  `restored`     TINYINT UNSIGNED NOT NULL DEFAULT 1,
  PRIMARY KEY (`activity_id`, `guid`),
  KEY `idx_guid` (`guid`),
  KEY `idx_restored` (`restored`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
