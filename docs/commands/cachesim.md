# cachesim

**a `/bin` program.**

**Category:** Diagnostics

## Synopsis

```
cachesim [-i SECONDS]
```

## Options

- `-i SECONDS`, `--interval SECONDS` -- count only what is read during
  the next SECONDS (1..86400), instead of everything since boot.
- `-h`, `--help` -- the options.

## Description

How many 4 KiB reads a cache would have served, if toy-os had one, for
caches of 4, 16, 64 and 256 MiB. There is no page cache
(`docs/pagecache-design.md`); this measures whether building one is
worth it, on the workload the machine actually runs.

The kernel keeps **ghost caches**: LRU lists of keys with no data
behind them, so their hit counts are exactly a real cache's and they
cost ~5 MiB rather than 256. Two levels:

- `file` -- a page cache ABOVE the filesystem: every read through the
  VFS, 4 KiB of one file per key, dropped when anything changes that
  file. What a `read()`, a program's image and a config load would hit.
- `block` -- a cache UNDER the filesystem: every read the block layer
  sends to a device, metadata included (bitmaps, inode tables, pointer
  tables), kept current by writes.

**It needs the `cachesim` boot word**, because the keys cost memory a
normal boot should not pay. Without it the command says so and exits 1.

```
/$ bootcfg try 0 +cachesim
... (the machine restarts) ...
/$ cachesim -i 60
Over 60 s:
level      size      reads       hits   hit%   resident
file      4 MiB       5120       3300  64.4%       1024
...
```

`reads` counts 4 KiB pages, so one 64 KiB `read()` is 16. `resident` is
how many pages the simulated cache holds now; below its size it is the
whole working set since boot. A hit is a read that a cache of that size,
evicting the least recently used page, would have answered from memory.

**What it does not see.** `/lib` is already served from the kernel's
image cache, so a shared library's pages are read through the VFS once
per boot and show up as one miss each. A renamed file's pages count as
new, and a second hard-linked name shares nothing with the first: both
can only make the numbers lower, never higher.

Exit status: 0 printed, 1 the kernel is not measuring, 2 a usage error.
