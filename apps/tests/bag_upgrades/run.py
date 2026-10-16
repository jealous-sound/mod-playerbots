# This file is part of the mod-playerbots module for AzerothCore.
# Released under GNU GPL v2, or (at your option) any later version.

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def extract_method(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for index in range(opening, len(source)):
        depth += (source[index] == '{') - (source[index] == '}')
        if depth == 0:
            return source[start:index + 1]
    raise ValueError('Unclosed method: ' + signature)


def main():
    parser = argparse.ArgumentParser(
        description='Compile actual bag-upgrade methods against bounded inventory/packet APIs.')
    parser.add_argument('--source-ref', help='Read production methods from a local Git revision.')
    args = parser.parse_args()

    def read_source(relative):
        if args.source_ref:
            return subprocess.check_output(
                ['git', 'show', f'{args.source_ref}:{relative}'], cwd=ROOT,
                text=True, encoding='utf-8')
        return (ROOT / relative).read_text(encoding='utf-8')

    values = read_source('src/Ai/Base/Value/ItemUsageValue.cpp')
    actions = read_source('src/Ai/Base/Actions/EquipAction.cpp')
    harness = (HERE / 'harness.cpp').read_text(encoding='utf-8')
    methods = [extract_method(values, 'uint32 ItemUsageValue::GetSmallestBagSize()'),
               extract_method(actions, 'uint8 EquipAction::GetSmallestBagSlot()'),
               extract_method(actions, 'bool EquipUpgradesPacketAction::Execute('),
               extract_method(actions, 'bool EquipUpgradeAction::Execute(')]
    if 'void EquipAction::EquipBagUpgrades()' in actions:
        methods.append(extract_method(actions, 'void EquipAction::EquipBagUpgrades()'))
        methods.append('#define HAS_BAG_UPGRADES 1')
    harness = harness.replace('// ACTUAL_METHODS', '\n'.join(methods))
    compiler = shutil.which(os.environ.get('CXX', 'cl.exe' if os.name == 'nt' else 'c++'))
    if not compiler:
        raise RuntimeError('Enable a C++20 compiler; use a VS Developer shell on Windows')
    with tempfile.TemporaryDirectory(prefix='playerbot-bags-') as directory:
        out = Path(directory)
        cpp = out / 'harness.cpp'
        cpp.write_text(harness, encoding='utf-8')
        exe = out / ('test.exe' if os.name == 'nt' else 'test')
        flags = (['/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', str(cpp), '/Fe' + str(exe)]
                 if Path(compiler).stem.lower() == 'cl' else
                 ['-std=c++20', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)])
        subprocess.run([compiler, *flags], cwd=out, check=True)
        subprocess.run([str(exe)], cwd=out, check=True)


if __name__ == '__main__':
    main()
