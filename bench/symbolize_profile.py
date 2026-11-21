#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Names the call stacks AtlasBench --load --profile sampled (Windows).

    python bench/symbolize_profile.py profile.txt --map Atlas.map [--stage "show editor"]

The plugin's functions are named from the linker's map file (link Atlas_VST3
with /MAP: configure with
-DCMAKE_MODULE_LINKER_FLAGS_RELEASE="/INCREMENTAL:NO /MAP:<file>"), and the
functions of system libraries from their export tables. For each stage it
prints where the message thread was: the functions the samples were in, the
plugin functions on the stacks, and the commonest stacks.
"""

import argparse
import bisect
import collections
import ctypes
import os
import re
import struct


def undecorate(name):
    if not name.startswith('?') or os.name != 'nt':
        return name
    buffer = ctypes.create_string_buffer(2048)
    # UNDNAME_NAME_ONLY: the qualified name without the return and parameter types.
    if ctypes.windll.dbghelp.UnDecorateSymbolName(name.encode('ascii', 'replace'), buffer, 2048, 0x1000):
        return buffer.value.decode('ascii', 'replace')
    return name


def read_map(path):
    """(rva, name) of every symbol in an MSVC map file, sorted by rva."""
    preferred = None
    symbols = []
    line_pattern = re.compile(r'^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s')
    with open(path, encoding='utf-8', errors='replace') as file:
        for line in file:
            if preferred is None:
                found = re.search(r'Preferred load address is ([0-9a-fA-F]+)', line)
                if found:
                    preferred = int(found.group(1), 16)
                continue
            found = line_pattern.match(line)
            if found:
                address = int(found.group(2), 16)
                if address >= preferred:
                    symbols.append((address - preferred, found.group(1)))
    symbols.sort()
    return symbols


def read_exports(path):
    """(rva, name) of a PE file's exported functions, sorted by rva."""
    try:
        with open(path, 'rb') as file:
            data = file.read()
    except OSError:
        return []
    try:
        pe = struct.unpack_from('<I', data, 0x3C)[0]
        sections_count = struct.unpack_from('<H', data, pe + 6)[0]
        optional_size = struct.unpack_from('<H', data, pe + 20)[0]
        optional = pe + 24
        magic = struct.unpack_from('<H', data, optional)[0]
        directories = optional + (112 if magic == 0x20B else 96)
        export_rva, export_size = struct.unpack_from('<II', data, directories)
        sections = []
        table = optional + optional_size
        for index in range(sections_count):
            virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from('<IIII', data, table + 40 * index + 8)
            sections.append((virtual_address, max(virtual_size, raw_size), raw_pointer))

        def offset(rva):
            for virtual_address, size, raw_pointer in sections:
                if virtual_address <= rva < virtual_address + size:
                    return rva - virtual_address + raw_pointer
            raise ValueError(rva)

        if export_rva == 0:
            return []
        directory = offset(export_rva)
        names_count = struct.unpack_from('<I', data, directory + 24)[0]
        functions, names, ordinals = struct.unpack_from('<III', data, directory + 28)
        symbols = []
        for index in range(names_count):
            name_rva = struct.unpack_from('<I', data, offset(names) + 4 * index)[0]
            ordinal = struct.unpack_from('<H', data, offset(ordinals) + 2 * index)[0]
            function = struct.unpack_from('<I', data, offset(functions) + 4 * ordinal)[0]
            if export_rva <= function < export_rva + export_size:
                continue  # forwarded to another library
            start = offset(name_rva)
            symbols.append((function, data[start:data.index(b'\0', start)].decode('ascii', 'replace')))
        symbols.sort()
        return symbols
    except (struct.error, ValueError):
        return []


class Module:
    def __init__(self, base, size, path):
        self.base, self.size, self.path = base, size, path
        self.name = os.path.basename(path)
        self.symbols = None
        self.starts = None
        self.named = {}

    def load(self, map_symbols):
        self.symbols = map_symbols if map_symbols is not None else read_exports(self.path)
        self.starts = [rva for rva, _ in self.symbols]

    def symbol(self, address):
        rva = address - self.base
        index = bisect.bisect_right(self.starts, rva) - 1
        if index < 0:
            return '%s+0x%x' % (self.name, rva)
        if index not in self.named:
            self.named[index] = undecorate(self.symbols[index][1])
        return self.named[index]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('profile')
    parser.add_argument('--map', help="the plugin's linker map file")
    parser.add_argument('--module', default='Atlas.vst3', help='the module the map file describes')
    parser.add_argument('--stage', action='append', help='only these stages (by name); default all with samples')
    parser.add_argument('--top', type=int, default=25)
    parser.add_argument('--stacks', type=int, default=6, help='how many of the commonest stacks to print')
    parser.add_argument('--depth', type=int, default=14, help='plugin frames shown of each stack')
    parser.add_argument('--min-samples', type=int, default=15, help='skip stages with fewer samples')
    parser.add_argument('--grep', help='print the commonest whole stacks (every library) with a function '
                                       'whose name contains this text, instead of the tables')
    options = parser.parse_args()

    stages, modules, samples = {}, [], []
    with open(options.profile, encoding='utf-8', errors='replace') as file:
        for line in file:
            kind, _, rest = line.rstrip('\n').partition(' ')
            if kind == 'stage':
                index, _, name = rest.partition(' ')
                stages[int(index)] = name
            elif kind == 'module':
                base, size, path = rest.split(' ', 2)
                modules.append(Module(int(base, 16), int(size, 16), path))
            elif kind == 'sample':
                fields = rest.split(' ')
                samples.append((int(fields[0]), float(fields[1]), [int(frame, 16) for frame in fields[2:]]))
    modules.sort(key=lambda module: module.base)
    bases = [module.base for module in modules]
    map_symbols = read_map(options.map) if options.map else None

    def find(address):
        index = bisect.bisect_right(bases, address) - 1
        if index < 0 or address >= modules[index].base + modules[index].size:
            return None
        module = modules[index]
        if module.symbols is None:
            module.load(map_symbols if module.name.lower() == options.module.lower() else None)
        return module

    def name(address, frame):
        module = find(address)
        if module is None:
            return None, '0x%x' % address
        # A return address belongs to the call before it.
        return module, module.symbol(address if frame == 0 else address - 1)

    by_stage = collections.defaultdict(list)
    for stage, ms, frames in samples:
        by_stage[stage].append(frames)

    for stage in sorted(by_stage):
        stage_name = stages.get(stage, str(stage))
        stacks = by_stage[stage]
        if options.stage:
            if stage_name not in options.stage:
                continue
        elif len(stacks) < options.min_samples:
            continue
        if options.grep:
            matching = collections.Counter()
            for frames in stacks:
                named = [name(address, index) for index, address in enumerate(frames)]
                labels = tuple(('%s!%s' % (module.name, symbol)) if module else symbol for module, symbol in named)
                if any(options.grep.lower() in label.lower() for label in labels):
                    matching[labels[:options.depth]] += 1
            print('\n=== %s: %d of %d samples have "%s" on the stack ===' %
                  (stage_name, sum(matching.values()), len(stacks), options.grep))
            for stack, samples_in in matching.most_common(options.stacks):
                print('  %d samples' % samples_in)
                for label in stack:
                    print('      %s' % label[:150])
            continue
        self_time = collections.Counter()
        inclusive = collections.Counter()
        innermost = collections.Counter()
        common = collections.Counter()
        for frames in stacks:
            named = [name(address, index) for index, address in enumerate(frames)]
            module, symbol = named[0]
            self_time[('%s!%s' % (module.name, symbol)) if module else symbol] += 1
            plugin = [symbol for module, symbol in named
                      if module is not None and module.name.lower() == options.module.lower()]
            for symbol in set(plugin):
                inclusive[symbol] += 1
            if plugin:
                innermost[plugin[0]] += 1
                common[tuple(plugin[:options.depth])] += 1
            else:
                innermost['(none)'] += 1
        total = len(stacks)
        print('\n=== %s: %d samples (about %d ms) ===' % (stage_name, total, total))

        def table(title, counter, count):
            print('\n%s' % title)
            for symbol, samples_in in counter.most_common(count):
                print('  %5d  %5.1f %%  %s' % (samples_in, 100.0 * samples_in / total, symbol[:150]))

        table('where the samples were (innermost function of any library)', self_time, options.top)
        table('innermost plugin function on the stack', innermost, options.top)
        table('plugin functions on the stack (each counted once a sample)', inclusive, options.top + 15)
        print('\ncommonest stacks, plugin frames, innermost first')
        for stack, samples_in in common.most_common(options.stacks):
            print('  %d samples (%.1f %%)' % (samples_in, 100.0 * samples_in / total))
            for symbol in stack:
                print('      %s' % symbol[:150])


if __name__ == '__main__':
    main()
