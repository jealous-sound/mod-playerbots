# Bag upgrade regression checks

From the module root, with Python 3 and a C++20 compiler on PATH:

```sh
python -B apps/tests/bag_upgrades/run.py
```

On Windows, use a Visual Studio Developer shell. Set `CXX` to choose another
compiler. To reproduce a regression against an earlier local module revision:

```sh
python -B apps/tests/bag_upgrades/run.py --source-ref <revision>
```

The harness compiles the actual bag-size, destination-selection, bag-upgrade
and action-dispatch methods from the module. Inventory and packet APIs are
bounded substitutes; gear scoring and native item storage are not exercised.

Coverage includes the first equipped bag slot, an empty first slot, multiple
identical bags, no downgrades or equal-size swaps, unusable/traded/specialized
bags, temporary backpack staging, trade completion, the auto-equip setting,
item-receipt events and the manual `equip upgrade` command.
Successful swaps produce one localized equipment notice per pass, grouping
identical bags; skipped or rejected swaps must not produce success messages.

These checks do not validate native item-content migration, database persistence,
network delivery, gameplay, or performance with thousands of bots. Verify those
separately on a test server. No server or database is modified by this harness.
