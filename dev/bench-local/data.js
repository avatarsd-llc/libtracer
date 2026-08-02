window.BENCHMARK_DATA = {
  "lastUpdate": 1785703955183,
  "repoUrl": "https://github.com/avatarsd-llc/libtracer",
  "entries": {
    "libtracer bench-local latency (ns, smaller is better, fixed pinned host)": [
      {
        "commit": {
          "author": {
            "email": "15184545+AvatarSD@users.noreply.github.com",
            "name": "avatarsd",
            "username": "AvatarSD"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "de42ac349da2b2e22d41607523fb8abc1ed62c74",
          "message": "Merge pull request #819 from avatarsd-llc/ci/perf-local-fixed-host-series\n\nci(perf-local): fixed-host bench series on the bench-local runner",
          "timestamp": "2026-08-02T23:50:59+03:00",
          "tree_id": "69e910ab197cf7b93153239e7523d5bfbac93a76",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/de42ac349da2b2e22d41607523fb8abc1ed62c74"
        },
        "date": 1785703947486,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 130,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 185,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 106.8,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-batch 64B/fan1/1ep p50 latency",
            "value": 104,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 64B/fan8/1ep p50 latency",
            "value": 260,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 64B/fan8/1ep p99 latency",
            "value": 380,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 64B/fan8/1ep ns/delivery",
            "value": 31,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-batch 64B/fan8/1ep p50 latency",
            "value": 247,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 64B/fan128/1ep p50 latency",
            "value": 1670,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 64B/fan128/1ep p99 latency",
            "value": 2510,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 64B/fan128/1ep ns/delivery",
            "value": 13.2,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-batch 64B/fan128/1ep p50 latency",
            "value": 1650,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 64B/fan1024/1ep p50 latency",
            "value": 12820,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 64B/fan1024/1ep p99 latency",
            "value": 25360,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 64B/fan1024/1ep ns/delivery",
            "value": 13,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-batch 64B/fan1024/1ep p50 latency",
            "value": 12785,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 64B/fan8192/1ep p50 latency",
            "value": 102110,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 64B/fan8192/1ep p99 latency",
            "value": 143430,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 64B/fan8192/1ep ns/delivery",
            "value": 13.1,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-batch 64B/fan8192/1ep p50 latency",
            "value": 101900,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 1B/fan1/1ep p50 latency",
            "value": 130,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 1B/fan1/1ep p99 latency",
            "value": 180,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 1B/fan1/1ep ns/delivery",
            "value": 103.8,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-batch 1B/fan1/1ep p50 latency",
            "value": 105,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 8B/fan1/1ep p50 latency",
            "value": 120,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 8B/fan1/1ep p99 latency",
            "value": 200,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 8B/fan1/1ep ns/delivery",
            "value": 105.4,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-batch 8B/fan1/1ep p50 latency",
            "value": 105,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 1024B/fan1/1ep p50 latency",
            "value": 130,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 1024B/fan1/1ep p99 latency",
            "value": 200,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 1024B/fan1/1ep ns/delivery",
            "value": 114,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-batch 1024B/fan1/1ep p50 latency",
            "value": 115,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 8192B/fan1/1ep p50 latency",
            "value": 190,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 8192B/fan1/1ep p99 latency",
            "value": 290,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 8192B/fan1/1ep ns/delivery",
            "value": 167.6,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-batch 8192B/fan1/1ep p50 latency",
            "value": 166,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p50 latency",
            "value": 120,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p99 latency",
            "value": 200,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep ns/delivery",
            "value": 96.4,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow-batch 1B/fan1/1ep p50 latency",
            "value": 97,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p50 latency",
            "value": 120,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p99 latency",
            "value": 170,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep ns/delivery",
            "value": 113.5,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow-batch 8B/fan1/1ep p50 latency",
            "value": 96,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 120,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 180,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 96.4,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow-batch 64B/fan1/1ep p50 latency",
            "value": 97,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p50 latency",
            "value": 120,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p99 latency",
            "value": 170,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep ns/delivery",
            "value": 101.7,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow-batch 1024B/fan1/1ep p50 latency",
            "value": 97,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p50 latency",
            "value": 120,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p99 latency",
            "value": 160,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep ns/delivery",
            "value": 95.9,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow-batch 8192B/fan1/1ep p50 latency",
            "value": 97,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p50 latency",
            "value": 150,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p99 latency",
            "value": 260,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path 64B/fan1/1ep ns/delivery",
            "value": 136.7,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path-batch 64B/fan1/1ep p50 latency",
            "value": 133,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p50 latency",
            "value": 170,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p99 latency",
            "value": 200,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path 64B/fan1/8ep ns/delivery",
            "value": 156.6,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path-batch 64B/fan1/8ep p50 latency",
            "value": 152,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p50 latency",
            "value": 190,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p99 latency",
            "value": 300,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path 64B/fan1/128ep ns/delivery",
            "value": 173.3,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path-batch 64B/fan1/128ep p50 latency",
            "value": 170,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p50 latency",
            "value": 190,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p99 latency",
            "value": 340,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep ns/delivery",
            "value": 186.1,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path-batch 64B/fan1/1024ep p50 latency",
            "value": 178,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p50 latency",
            "value": 200,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p99 latency",
            "value": 330,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 195,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path-batch 64B/fan1/8192ep p50 latency",
            "value": 195,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "mixed 0B/fan6/128ep p50 latency",
            "value": 180,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "mixed 0B/fan6/128ep p99 latency",
            "value": 600,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "mixed 0B/fan6/128ep ns/delivery",
            "value": 35,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "path-parse 2B/fan1/1ep p50 latency",
            "value": 14,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "path-parse 2B/fan1/1ep p99 latency",
            "value": 29,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "path-parse 2B/fan1/1ep ns/delivery",
            "value": 15.1,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "path-parse 12B/fan2/1ep p50 latency",
            "value": 23,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "path-parse 12B/fan2/1ep p99 latency",
            "value": 42,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "path-parse 12B/fan2/1ep ns/delivery",
            "value": 23.9,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "path-parse 23B/fan4/1ep p50 latency",
            "value": 32,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "path-parse 23B/fan4/1ep p99 latency",
            "value": 55,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "path-parse 23B/fan4/1ep ns/delivery",
            "value": 33.7,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "path-parse 16B/fan8/1ep p50 latency",
            "value": 46,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "path-parse 16B/fan8/1ep p99 latency",
            "value": 68,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "path-parse 16B/fan8/1ep ns/delivery",
            "value": 47.1,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p50 latency",
            "value": 140,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p99 latency",
            "value": 150,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep ns/delivery",
            "value": 105.2,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p50 latency",
            "value": 140,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p99 latency",
            "value": 150,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep ns/delivery",
            "value": 105.2,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p50 latency",
            "value": 140,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p99 latency",
            "value": 150,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep ns/delivery",
            "value": 105.4,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-mt8 64B/fan1/8ep p50 latency",
            "value": 140,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-mt8 64B/fan1/8ep p99 latency",
            "value": 150,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-mt8 64B/fan1/8ep ns/delivery",
            "value": 107.7,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p50 latency",
            "value": 149,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p99 latency",
            "value": 150,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep ns/delivery",
            "value": 125.3,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p50 latency",
            "value": 140,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p99 latency",
            "value": 190,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep ns/delivery",
            "value": 119.6,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p50 latency",
            "value": 210,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p99 latency",
            "value": 230,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep ns/delivery",
            "value": 190.7,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p50 latency",
            "value": 90,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p99 latency",
            "value": 90,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep ns/delivery",
            "value": 68,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p50 latency",
            "value": 90,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p99 latency",
            "value": 110,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep ns/delivery",
            "value": 72.4,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fold-b1 512B/fan1/1ep p50 latency",
            "value": 2,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fold-b1 512B/fan1/1ep p99 latency",
            "value": 2,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fold-b1 512B/fan1/1ep ns/delivery",
            "value": 1.8,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fold-b2 512B/fan1/1ep p50 latency",
            "value": 3,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fold-b2 512B/fan1/1ep p99 latency",
            "value": 3,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fold-b2 512B/fan1/1ep ns/delivery",
            "value": 3,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fold-b4 512B/fan1/1ep p50 latency",
            "value": 5,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fold-b4 512B/fan1/1ep p99 latency",
            "value": 10,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fold-b4 512B/fan1/1ep ns/delivery",
            "value": 4.9,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fold-b8 512B/fan1/1ep p50 latency",
            "value": 9,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fold-b8 512B/fan1/1ep p99 latency",
            "value": 15,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fold-b8 512B/fan1/1ep ns/delivery",
            "value": 8.9,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep p50 latency",
            "value": 110,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep p99 latency",
            "value": 110,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep ns/delivery",
            "value": 88.7,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p50 latency",
            "value": 240,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p99 latency",
            "value": 250,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep ns/delivery",
            "value": 28,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p50 latency",
            "value": 1660,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p99 latency",
            "value": 2530,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep ns/delivery",
            "value": 12.9,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p50 latency",
            "value": 12620,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p99 latency",
            "value": 18800,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep ns/delivery",
            "value": 12.7,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p50 latency",
            "value": 101980,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p99 latency",
            "value": 139580,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep ns/delivery",
            "value": 12.7,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-alloc-heap 64B/fan1/1ep p50 latency",
            "value": 19,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-alloc-heap 64B/fan1/1ep ns/delivery",
            "value": 19.5,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-alloc-pool 64B/fan1/1ep p50 latency",
            "value": 7,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-alloc-pool 64B/fan1/1ep ns/delivery",
            "value": 7.4,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-store-heap 64B/fan1/1ep p50 latency",
            "value": 25,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-store-heap 64B/fan1/1ep ns/delivery",
            "value": 25.5,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-store-pool 64B/fan1/1ep p50 latency",
            "value": 11,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-store-pool 64B/fan1/1ep ns/delivery",
            "value": 11.7,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-alloc-heap 1024B/fan1/1ep p50 latency",
            "value": 19,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-alloc-heap 1024B/fan1/1ep ns/delivery",
            "value": 19.6,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-alloc-pool 1024B/fan1/1ep p50 latency",
            "value": 7,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-alloc-pool 1024B/fan1/1ep ns/delivery",
            "value": 7.3,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-store-heap 1024B/fan1/1ep p50 latency",
            "value": 30,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-store-heap 1024B/fan1/1ep ns/delivery",
            "value": 30.5,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-store-pool 1024B/fan1/1ep p50 latency",
            "value": 15,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-store-pool 1024B/fan1/1ep ns/delivery",
            "value": 15.9,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool 1B/fan1/1ep p50 latency",
            "value": 150,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool 1B/fan1/1ep p99 latency",
            "value": 210,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool 1B/fan1/1ep ns/delivery",
            "value": 124.3,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool 8B/fan1/1ep p50 latency",
            "value": 150,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool 8B/fan1/1ep p99 latency",
            "value": 200,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool 8B/fan1/1ep ns/delivery",
            "value": 125.2,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool 64B/fan1/1ep p50 latency",
            "value": 150,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool 64B/fan1/1ep p99 latency",
            "value": 200,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool 64B/fan1/1ep ns/delivery",
            "value": 127.7,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool 1024B/fan1/1ep p50 latency",
            "value": 150,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool 1024B/fan1/1ep p99 latency",
            "value": 220,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool 1024B/fan1/1ep ns/delivery",
            "value": 128.3,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool 8192B/fan1/1ep p50 latency",
            "value": 230,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool 8192B/fan1/1ep p99 latency",
            "value": 340,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool 8192B/fan1/1ep ns/delivery",
            "value": 208.7,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool-borrow 1B/fan1/1ep p50 latency",
            "value": 140,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool-borrow 1B/fan1/1ep p99 latency",
            "value": 170,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool-borrow 1B/fan1/1ep ns/delivery",
            "value": 112.6,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool-borrow 8B/fan1/1ep p50 latency",
            "value": 140,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool-borrow 8B/fan1/1ep p99 latency",
            "value": 140,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool-borrow 8B/fan1/1ep ns/delivery",
            "value": 115.4,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool-borrow 64B/fan1/1ep p50 latency",
            "value": 140,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool-borrow 64B/fan1/1ep p99 latency",
            "value": 200,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool-borrow 64B/fan1/1ep ns/delivery",
            "value": 112.6,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool-borrow 1024B/fan1/1ep p50 latency",
            "value": 140,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool-borrow 1024B/fan1/1ep p99 latency",
            "value": 170,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool-borrow 1024B/fan1/1ep ns/delivery",
            "value": 114.1,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool-borrow 8192B/fan1/1ep p50 latency",
            "value": 140,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool-borrow 8192B/fan1/1ep p99 latency",
            "value": 190,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool-borrow 8192B/fan1/1ep ns/delivery",
            "value": 113.1,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "poolalloc-mt1 64B/fan1/1ep p50 latency",
            "value": 40,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "poolalloc-mt1 64B/fan1/1ep p99 latency",
            "value": 50,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "poolalloc-mt1 64B/fan1/1ep ns/delivery",
            "value": 71.3,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heapalloc-mt1 64B/fan1/1ep p50 latency",
            "value": 40,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heapalloc-mt1 64B/fan1/1ep p99 latency",
            "value": 50,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heapalloc-mt1 64B/fan1/1ep ns/delivery",
            "value": 70.9,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "poolalloc-mt2 64B/fan1/1ep p50 latency",
            "value": 40,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "poolalloc-mt2 64B/fan1/1ep p99 latency",
            "value": 50,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "poolalloc-mt2 64B/fan1/1ep ns/delivery",
            "value": 83.8,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heapalloc-mt2 64B/fan1/1ep p50 latency",
            "value": 40,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heapalloc-mt2 64B/fan1/1ep p99 latency",
            "value": 60,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heapalloc-mt2 64B/fan1/1ep ns/delivery",
            "value": 74,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "poolalloc-mt4 64B/fan1/1ep p50 latency",
            "value": 40,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "poolalloc-mt4 64B/fan1/1ep p99 latency",
            "value": 50,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "poolalloc-mt4 64B/fan1/1ep ns/delivery",
            "value": 142.3,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heapalloc-mt4 64B/fan1/1ep p50 latency",
            "value": 40,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heapalloc-mt4 64B/fan1/1ep p99 latency",
            "value": 60,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heapalloc-mt4 64B/fan1/1ep ns/delivery",
            "value": 72.5,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "poolalloc-mt8 64B/fan1/1ep p50 latency",
            "value": 40,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "poolalloc-mt8 64B/fan1/1ep p99 latency",
            "value": 50,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "poolalloc-mt8 64B/fan1/1ep ns/delivery",
            "value": 138.1,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heapalloc-mt8 64B/fan1/1ep p50 latency",
            "value": 40,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heapalloc-mt8 64B/fan1/1ep p99 latency",
            "value": 50,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heapalloc-mt8 64B/fan1/1ep ns/delivery",
            "value": 73.2,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-stored 64B/fan1/1ep p50 latency",
            "value": 230,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-stored 64B/fan1/1ep p99 latency",
            "value": 260,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-stored 64B/fan1/1ep ns/delivery",
            "value": 211.1,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-stored 64B/fan8/1ep p50 latency",
            "value": 980,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-stored 64B/fan8/1ep p99 latency",
            "value": 1080,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-stored 64B/fan8/1ep ns/delivery",
            "value": 124.1,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-stored 64B/fan128/1ep p50 latency",
            "value": 15270,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-stored 64B/fan128/1ep p99 latency",
            "value": 26930,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-stored 64B/fan128/1ep ns/delivery",
            "value": 120.8,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-stored 64B/fan1024/1ep p50 latency",
            "value": 136310,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-stored 64B/fan1024/1ep p99 latency",
            "value": 186430,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-stored 64B/fan1024/1ep ns/delivery",
            "value": 135.5,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-stored 64B/fan8192/1ep p50 latency",
            "value": 1197089,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-stored 64B/fan8192/1ep p99 latency",
            "value": 1385318,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-stored 64B/fan8192/1ep ns/delivery",
            "value": 146.4,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-handler 64B/fan1/1ep p50 latency",
            "value": 200,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-handler 64B/fan1/1ep p99 latency",
            "value": 290,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-handler 64B/fan1/1ep ns/delivery",
            "value": 179.8,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-handler 64B/fan8/1ep p50 latency",
            "value": 710,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-handler 64B/fan8/1ep p99 latency",
            "value": 970,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-handler 64B/fan8/1ep ns/delivery",
            "value": 87.8,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-handler 64B/fan128/1ep p50 latency",
            "value": 9410,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-handler 64B/fan128/1ep p99 latency",
            "value": 15280,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-handler 64B/fan128/1ep ns/delivery",
            "value": 75,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-handler 64B/fan1024/1ep p50 latency",
            "value": 101200,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-handler 64B/fan1024/1ep p99 latency",
            "value": 132880,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-handler 64B/fan1024/1ep ns/delivery",
            "value": 101,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-handler 64B/fan8192/1ep p50 latency",
            "value": 974259,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-handler 64B/fan8192/1ep p99 latency",
            "value": 1071958,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-handler 64B/fan8192/1ep ns/delivery",
            "value": 117,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan1/1ep p50 latency",
            "value": 130,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan1/1ep p99 latency",
            "value": 195,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan1/1ep ns/delivery",
            "value": 140.1,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan2/1ep p50 latency",
            "value": 128,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan2/1ep p99 latency",
            "value": 186,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan2/1ep ns/delivery",
            "value": 134.1,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan4/1ep p50 latency",
            "value": 131,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan4/1ep p99 latency",
            "value": 190,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan4/1ep ns/delivery",
            "value": 141.1,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan8/1ep p50 latency",
            "value": 133,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan8/1ep p99 latency",
            "value": 197,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan8/1ep ns/delivery",
            "value": 143.9,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan16/1ep p50 latency",
            "value": 136,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan16/1ep p99 latency",
            "value": 208,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan16/1ep ns/delivery",
            "value": 144.7,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan32/1ep p50 latency",
            "value": 145,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan32/1ep p99 latency",
            "value": 233,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan32/1ep ns/delivery",
            "value": 160.5,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan64/1ep p50 latency",
            "value": 161,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan64/1ep p99 latency",
            "value": 238,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan64/1ep ns/delivery",
            "value": 172.3,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan1/1ep p50 latency",
            "value": 130,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan1/1ep p99 latency",
            "value": 178,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan1/1ep ns/delivery",
            "value": 139.7,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan2/2ep p50 latency",
            "value": 130,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan2/2ep p99 latency",
            "value": 183,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan2/2ep ns/delivery",
            "value": 139.2,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan4/4ep p50 latency",
            "value": 131,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan4/4ep p99 latency",
            "value": 180,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan4/4ep ns/delivery",
            "value": 140,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan8/8ep p50 latency",
            "value": 135,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan8/8ep p99 latency",
            "value": 187,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan8/8ep ns/delivery",
            "value": 143.9,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan16/16ep p50 latency",
            "value": 136,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan16/16ep p99 latency",
            "value": 205,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan16/16ep ns/delivery",
            "value": 143.7,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan32/32ep p50 latency",
            "value": 145,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan32/32ep p99 latency",
            "value": 221,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan32/32ep ns/delivery",
            "value": 155.6,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan64/64ep p50 latency",
            "value": 161,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan64/64ep p99 latency",
            "value": 262,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan64/64ep ns/delivery",
            "value": 173,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-resolve 79B/fan1/1ep p50 latency",
            "value": 52,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-resolve 79B/fan1/1ep p99 latency",
            "value": 80,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-rebuild 79B/fan1/1ep p50 latency",
            "value": 17,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-rebuild 79B/fan1/1ep p99 latency",
            "value": 26,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-terminus 4B/fan1/1ep p50 latency",
            "value": 118,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-terminus 4B/fan1/1ep p99 latency",
            "value": 193,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-terminus 4B/fan1/1ep ns/delivery",
            "value": 131.1,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-forward 4B/fan1/1ep p50 latency",
            "value": 45,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-forward 4B/fan1/1ep p99 latency",
            "value": 61,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-forward 4B/fan1/1ep ns/delivery",
            "value": 49.5,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-terminus 64B/fan1/1ep p50 latency",
            "value": 118,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-terminus 64B/fan1/1ep p99 latency",
            "value": 210,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-terminus 64B/fan1/1ep ns/delivery",
            "value": 133.5,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-forward 64B/fan1/1ep p50 latency",
            "value": 45,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-forward 64B/fan1/1ep p99 latency",
            "value": 77,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-forward 64B/fan1/1ep ns/delivery",
            "value": 51.5,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-terminus 512B/fan1/1ep p50 latency",
            "value": 120,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-terminus 512B/fan1/1ep p99 latency",
            "value": 192,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-terminus 512B/fan1/1ep ns/delivery",
            "value": 129.7,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-forward 512B/fan1/1ep p50 latency",
            "value": 45,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-forward 512B/fan1/1ep p99 latency",
            "value": 61,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-forward 512B/fan1/1ep ns/delivery",
            "value": 49.3,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 64B/fan1/1ep p50 latency",
            "value": 210,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 64B/fan1/1ep p99 latency",
            "value": 250,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 64B/fan1/1ep ns/delivery",
            "value": 189.5,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 64B/fan8/1ep p50 latency",
            "value": 780,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 64B/fan8/1ep p99 latency",
            "value": 910,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 64B/fan8/1ep ns/delivery",
            "value": 96.3,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 64B/fan128/1ep p50 latency",
            "value": 9660,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 64B/fan128/1ep p99 latency",
            "value": 14540,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 64B/fan128/1ep ns/delivery",
            "value": 76.4,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 64B/fan1024/1ep p50 latency",
            "value": 75540,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 64B/fan1024/1ep p99 latency",
            "value": 88230,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 64B/fan1024/1ep ns/delivery",
            "value": 74.2,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 64B/fan8192/1ep p50 latency",
            "value": 620329,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 64B/fan8192/1ep p99 latency",
            "value": 651670,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 64B/fan8192/1ep ns/delivery",
            "value": 75.4,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 1B/fan1/1ep p50 latency",
            "value": 220,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 1B/fan1/1ep p99 latency",
            "value": 230,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 1B/fan1/1ep ns/delivery",
            "value": 197.8,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 8B/fan1/1ep p50 latency",
            "value": 220,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 8B/fan1/1ep p99 latency",
            "value": 240,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 8B/fan1/1ep ns/delivery",
            "value": 198.1,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 1024B/fan1/1ep p50 latency",
            "value": 220,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 1024B/fan1/1ep p99 latency",
            "value": 240,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 1024B/fan1/1ep ns/delivery",
            "value": 191.1,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 8192B/fan1/1ep p50 latency",
            "value": 290,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 8192B/fan1/1ep p99 latency",
            "value": 340,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 8192B/fan1/1ep ns/delivery",
            "value": 273.2,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc-path 64B/fan1/1ep p50 latency",
            "value": 210,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc-path 64B/fan1/1ep p99 latency",
            "value": 260,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc-path 64B/fan1/1ep ns/delivery",
            "value": 187,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc-path 64B/fan1/8ep p50 latency",
            "value": 210,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc-path 64B/fan1/8ep p99 latency",
            "value": 230,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc-path 64B/fan1/8ep ns/delivery",
            "value": 189.4,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc-path 64B/fan1/128ep p50 latency",
            "value": 210,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc-path 64B/fan1/128ep p99 latency",
            "value": 220,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc-path 64B/fan1/128ep ns/delivery",
            "value": 193.8,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc-path 64B/fan1/1024ep p50 latency",
            "value": 210,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc-path 64B/fan1/1024ep p99 latency",
            "value": 280,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc-path 64B/fan1/1024ep ns/delivery",
            "value": 192.9,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc-path 64B/fan1/8192ep p50 latency",
            "value": 220,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc-path 64B/fan1/8192ep p99 latency",
            "value": 450,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 255.6,
            "unit": "ns",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heap bytes per forward (probe)",
            "value": 0,
            "unit": "bytes",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heap allocs per forward (probe)",
            "value": 0,
            "unit": "allocs",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heap bytes per terminus (probe)",
            "value": 601,
            "unit": "bytes",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heap allocs per terminus (probe)",
            "value": 6,
            "unit": "allocs",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heap bytes per fanout_wide (probe)",
            "value": 26,
            "unit": "bytes",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heap allocs per fanout_wide (probe)",
            "value": 2,
            "unit": "allocs",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heap bytes per vertex (probe)",
            "value": 136,
            "unit": "bytes",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heap allocs per vertex (probe)",
            "value": 3,
            "unit": "allocs",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heap bytes per vertex_value (probe)",
            "value": 104,
            "unit": "bytes",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heap allocs per vertex_value (probe)",
            "value": 2,
            "unit": "allocs",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heap bytes per vertex_app5 (probe)",
            "value": 648,
            "unit": "bytes",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heap allocs per vertex_app5 (probe)",
            "value": 13,
            "unit": "allocs",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heap bytes per vertex_app5_static (probe)",
            "value": 344,
            "unit": "bytes",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heap allocs per vertex_app5_static (probe)",
            "value": 5,
            "unit": "allocs",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heap bytes per reg_escape (probe)",
            "value": 336,
            "unit": "bytes",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heap allocs per reg_escape (probe)",
            "value": 4,
            "unit": "allocs",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "bench_libtracer max RSS",
            "value": 96648,
            "unit": "KB",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          }
        ]
      }
    ],
    "libtracer bench-local throughput (deliveries/s, bigger is better, fixed pinned host)": [
      {
        "commit": {
          "author": {
            "email": "15184545+AvatarSD@users.noreply.github.com",
            "name": "avatarsd",
            "username": "AvatarSD"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "de42ac349da2b2e22d41607523fb8abc1ed62c74",
          "message": "Merge pull request #819 from avatarsd-llc/ci/perf-local-fixed-host-series\n\nci(perf-local): fixed-host bench series on the bench-local runner",
          "timestamp": "2026-08-02T23:50:59+03:00",
          "tree_id": "69e910ab197cf7b93153239e7523d5bfbac93a76",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/de42ac349da2b2e22d41607523fb8abc1ed62c74"
        },
        "date": 1785703954200,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep throughput",
            "value": 9362999,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 64B/fan8/1ep throughput",
            "value": 32217198,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 64B/fan128/1ep throughput",
            "value": 75851453,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 64B/fan1024/1ep throughput",
            "value": 76839565,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 64B/fan8192/1ep throughput",
            "value": 76328921,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 1B/fan1/1ep throughput",
            "value": 9636607,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 8B/fan1/1ep throughput",
            "value": 9486675,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 1024B/fan1/1ep throughput",
            "value": 8770968,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc 8192B/fan1/1ep throughput",
            "value": 5967339,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep throughput",
            "value": 10370391,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep throughput",
            "value": 8810681,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep throughput",
            "value": 10371504,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep throughput",
            "value": 9831370,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep throughput",
            "value": 10424901,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path 64B/fan1/1ep throughput",
            "value": 7313338,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path 64B/fan1/8ep throughput",
            "value": 6386820,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path 64B/fan1/128ep throughput",
            "value": 5770496,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep throughput",
            "value": 5373391,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep throughput",
            "value": 5128846,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "mixed 0B/fan6/128ep throughput",
            "value": 28601475,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "path-parse 2B/fan1/1ep throughput",
            "value": 66247740,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "path-parse 12B/fan2/1ep throughput",
            "value": 41890638,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "path-parse 23B/fan4/1ep throughput",
            "value": 29689851,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "path-parse 16B/fan8/1ep throughput",
            "value": 21242558,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep throughput",
            "value": 9505329,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep throughput",
            "value": 9505222,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep throughput",
            "value": 9485467,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-mt8 64B/fan1/8ep throughput",
            "value": 9281947,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep throughput",
            "value": 7981809,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep throughput",
            "value": 8360767,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep throughput",
            "value": 5245183,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep throughput",
            "value": 14713453,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep throughput",
            "value": 13816818,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fold-b1 512B/fan1/1ep throughput",
            "value": 542757632,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fold-b2 512B/fan1/1ep throughput",
            "value": 335094703,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fold-b4 512B/fan1/1ep throughput",
            "value": 202409339,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fold-b8 512B/fan1/1ep throughput",
            "value": 112732156,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep throughput",
            "value": 11278041,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep throughput",
            "value": 35696308,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep throughput",
            "value": 77307485,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep throughput",
            "value": 78624543,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep throughput",
            "value": 78809388,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-alloc-heap 64B/fan1/1ep throughput",
            "value": 51175829,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-alloc-pool 64B/fan1/1ep throughput",
            "value": 135995305,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-store-heap 64B/fan1/1ep throughput",
            "value": 39279897,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-store-pool 64B/fan1/1ep throughput",
            "value": 85315242,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-alloc-heap 1024B/fan1/1ep throughput",
            "value": 51020864,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-alloc-pool 1024B/fan1/1ep throughput",
            "value": 136388620,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-store-heap 1024B/fan1/1ep throughput",
            "value": 32781608,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "lkv-store-pool 1024B/fan1/1ep throughput",
            "value": 62974354,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool 1B/fan1/1ep throughput",
            "value": 8046684,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool 8B/fan1/1ep throughput",
            "value": 7986558,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool 64B/fan1/1ep throughput",
            "value": 7827973,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool 1024B/fan1/1ep throughput",
            "value": 7794716,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool 8192B/fan1/1ep throughput",
            "value": 4792194,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool-borrow 1B/fan1/1ep throughput",
            "value": 8883026,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool-borrow 8B/fan1/1ep throughput",
            "value": 8668655,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool-borrow 64B/fan1/1ep throughput",
            "value": 8883988,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool-borrow 1024B/fan1/1ep throughput",
            "value": 8767427,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-pool-borrow 8192B/fan1/1ep throughput",
            "value": 8845553,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "poolalloc-mt1 64B/fan1/1ep throughput",
            "value": 14020416,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heapalloc-mt1 64B/fan1/1ep throughput",
            "value": 14094530,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "poolalloc-mt2 64B/fan1/1ep throughput",
            "value": 11939070,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heapalloc-mt2 64B/fan1/1ep throughput",
            "value": 13510477,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "poolalloc-mt4 64B/fan1/1ep throughput",
            "value": 7026393,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heapalloc-mt4 64B/fan1/1ep throughput",
            "value": 13787350,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "poolalloc-mt8 64B/fan1/1ep throughput",
            "value": 7239964,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "heapalloc-mt8 64B/fan1/1ep throughput",
            "value": 13668956,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-stored 64B/fan1/1ep throughput",
            "value": 4738167,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-stored 64B/fan8/1ep throughput",
            "value": 8059638,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-stored 64B/fan128/1ep throughput",
            "value": 8278258,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-stored 64B/fan1024/1ep throughput",
            "value": 7380793,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-stored 64B/fan8192/1ep throughput",
            "value": 6828542,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-handler 64B/fan1/1ep throughput",
            "value": 5562688,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-handler 64B/fan8/1ep throughput",
            "value": 11392520,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-handler 64B/fan128/1ep throughput",
            "value": 13335597,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-handler 64B/fan1024/1ep throughput",
            "value": 9905447,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "inproc-target-handler 64B/fan8192/1ep throughput",
            "value": 8549175,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan1/1ep throughput",
            "value": 7136763,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan2/1ep throughput",
            "value": 7455285,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan4/1ep throughput",
            "value": 7084734,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan8/1ep throughput",
            "value": 6948166,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan16/1ep throughput",
            "value": 6909338,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan32/1ep throughput",
            "value": 6231677,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-fixed 79B/fan64/1ep throughput",
            "value": 5803526,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan1/1ep throughput",
            "value": 7156378,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan2/2ep throughput",
            "value": 7186032,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan4/4ep throughput",
            "value": 7143474,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan8/8ep throughput",
            "value": 6950493,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan16/16ep throughput",
            "value": 6961241,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan32/32ep throughput",
            "value": 6425384,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "fwd-demux-scan 79B/fan64/64ep throughput",
            "value": 5779231,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-terminus 4B/fan1/1ep throughput",
            "value": 7627259,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-forward 4B/fan1/1ep throughput",
            "value": 20182115,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-terminus 64B/fan1/1ep throughput",
            "value": 7489193,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-forward 64B/fan1/1ep throughput",
            "value": 19428750,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-terminus 512B/fan1/1ep throughput",
            "value": 7710341,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "compact-forward 512B/fan1/1ep throughput",
            "value": 20272324,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 64B/fan1/1ep throughput",
            "value": 5277695.5,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 64B/fan8/1ep throughput",
            "value": 10386740,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 64B/fan128/1ep throughput",
            "value": 13085429,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 64B/fan1024/1ep throughput",
            "value": 13480535,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 64B/fan8192/1ep throughput",
            "value": 13271285,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 1B/fan1/1ep throughput",
            "value": 5054366,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 8B/fan1/1ep throughput",
            "value": 5047292,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 1024B/fan1/1ep throughput",
            "value": 5233985,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc 8192B/fan1/1ep throughput",
            "value": 3660981,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc-path 64B/fan1/1ep throughput",
            "value": 5346352,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc-path 64B/fan1/8ep throughput",
            "value": 5280249,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc-path 64B/fan1/128ep throughput",
            "value": 5159889,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc-path 64B/fan1/1024ep throughput",
            "value": 5183750,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          },
          {
            "name": "zenoh inproc-path 64B/fan1/8192ep throughput",
            "value": 3911725,
            "unit": "deliveries/s",
            "extra": "studio · pinned cpu2 · AMD EPYC 9115 16-Core Processor · 31 cpus · governor not-exposed · 7.0.11-76070011-generic"
          }
        ]
      }
    ]
  }
}