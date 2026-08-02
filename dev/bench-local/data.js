window.BENCHMARK_DATA = {
  "lastUpdate": 1785703952675,
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
    ]
  }
}