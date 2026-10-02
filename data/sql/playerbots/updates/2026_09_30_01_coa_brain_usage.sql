-- CoA brain: daily usage of each decision engine (UTC days), so the budget caps survive restarts.
-- Written by the worldserver (absolute totals, upserted every minute); rows older than 90 days are deleted.
CREATE TABLE IF NOT EXISTS `playerbots_coa_brain_usage` (
  `day`           INT UNSIGNED     NOT NULL,            -- UTC yyyymmdd
  `engine`        VARCHAR(32)      NOT NULL,            -- engine name, '_global' (all calls) or '_probe'
  `requests`      INT UNSIGNED     NOT NULL DEFAULT 0,
  `failures`      INT UNSIGNED     NOT NULL DEFAULT 0,
  `input_tokens`  BIGINT UNSIGNED  NOT NULL DEFAULT 0,
  `output_tokens` BIGINT UNSIGNED  NOT NULL DEFAULT 0,
  `usd_micros`    BIGINT UNSIGNED  NOT NULL DEFAULT 0,
  `exhausted`     TINYINT UNSIGNED NOT NULL DEFAULT 0,  -- 402/429/spend limit seen that day
  PRIMARY KEY (`day`, `engine`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
