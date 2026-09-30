-- CoA: per-account choices about the bots (.playerbots optout, optin). Never dropped: the rows belong to players.
CREATE TABLE IF NOT EXISTS `playerbots_coa_player_prefs` (
  `account_id`    INT UNSIGNED     NOT NULL,
  `ai_text`       TINYINT UNSIGNED NOT NULL DEFAULT 0,  -- 0 default, 1 opted in, 2 opted out
  `voice`         TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `memory`        TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `activity_mask` INT UNSIGNED     NOT NULL DEFAULT 0,  -- opted out of: 1 duel offers, 2 hostile, 4 whispers, 8 gambling
  `disclosed_at`  INT UNSIGNED     NOT NULL DEFAULT 0,
  `changed_at`    INT UNSIGNED     NOT NULL DEFAULT 0,
  PRIMARY KEY (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
