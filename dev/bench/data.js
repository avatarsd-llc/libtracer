window.BENCHMARK_DATA = {
  "lastUpdate": 1783427604455,
  "repoUrl": "https://github.com/avatarsd-llc/libtracer",
  "entries": {
    "libtracer in-process latency (ns, smaller is better)": [
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
          "id": "ad6736e6f930c033af08322cca671e8fd40a1440",
          "message": "Merge pull request #286 from avatarsd-llc/test/transport-conformance-suite\n\ntest(net): parameterized transport_t seam-conformance suite",
          "timestamp": "2026-07-07T11:10:20+03:00",
          "tree_id": "77c9c2ed36df6d0eb69389f9af0cb422f8f201e9",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/ad6736e6f930c033af08322cca671e8fd40a1440"
        },
        "date": 1783415636285,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 255,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 276,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 247.5,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 221,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 231,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 209.1,
            "unit": "ns"
          }
        ]
      },
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
          "id": "ea632f307068d8e98b9f902b7b45354ea54408da",
          "message": "Merge pull request #289 from avatarsd-llc/feat/zenoh-absolute-comparison\n\nbench: CI-generated absolute libtracer-vs-Zenoh charts (in-process)",
          "timestamp": "2026-07-07T13:13:03+03:00",
          "tree_id": "54e9aca2d5e071fa6b560797c9158c930d92ca7d",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/ea632f307068d8e98b9f902b7b45354ea54408da"
        },
        "date": 1783419232588,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 256,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 276,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 244.5,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 230,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 250,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 211,
            "unit": "ns"
          }
        ]
      },
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
          "id": "7631b7b2a7a532599f11615899ccd81197af4ee6",
          "message": "Merge pull request #290 from avatarsd-llc/feat/zenoh-transport-matrix\n\nbench: network transport comparison — UDP/TCP/WS vs Zenoh (absolute)",
          "timestamp": "2026-07-07T13:29:13+03:00",
          "tree_id": "7a3362d7fc2e001970733be3098247c091264617",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/7631b7b2a7a532599f11615899ccd81197af4ee6"
        },
        "date": 1783420203023,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 250,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 243.1,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 230,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 240,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 209.9,
            "unit": "ns"
          }
        ]
      },
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
          "id": "8ec73906d4c7eb5c68a41fb7a7bef7ba0e5fe7a4",
          "message": "Merge pull request #291 from avatarsd-llc/fix/zenoh-transports-ci-establish\n\nbench: make the Zenoh transport comparison establish reliably in CI",
          "timestamp": "2026-07-07T13:52:51+03:00",
          "tree_id": "116f81467c027e2b0394af7cc9e6e6be315eee67",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/8ec73906d4c7eb5c68a41fb7a7bef7ba0e5fe7a4"
        },
        "date": 1783421616688,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 235,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 298,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 224.6,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 220,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 254,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 203.7,
            "unit": "ns"
          }
        ]
      },
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
          "id": "ee7cd4781b8e144e4dc501a16c6fcee2547fc046",
          "message": "Merge pull request #292 from avatarsd-llc/feat/zenoh-transports-two-process\n\nbench: two-process UDP/TCP transport comparison (Zenoh works in CI now)",
          "timestamp": "2026-07-07T14:16:27+03:00",
          "tree_id": "f336187a61e2aa51949c5dd4574b6cc191f79da5",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/ee7cd4781b8e144e4dc501a16c6fcee2547fc046"
        },
        "date": 1783423031005,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 261,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 320,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 251,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 230,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 241,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 211.4,
            "unit": "ns"
          }
        ]
      },
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
          "id": "8375759f342060c86bfa2a63ec610d8b1e5c7087",
          "message": "Merge pull request #293 from avatarsd-llc/fix/throughput-composition-batching\n\nbench: chart network throughput by composition (fix the batching methodology)",
          "timestamp": "2026-07-07T14:41:42+03:00",
          "tree_id": "517fad1eb41e6e654c14b69645f1130b40e4b865",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/8375759f342060c86bfa2a63ec610d8b1e5c7087"
        },
        "date": 1783424543960,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 256,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 300,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 245.8,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 230,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 250,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 211.2,
            "unit": "ns"
          }
        ]
      },
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
          "id": "f74d01eabbd57caff8c1a7f6c88c5c59697103fa",
          "message": "Merge pull request #294 from avatarsd-llc/feat/cmake-install-export\n\nbuild(core): CMake install/export — find_package(libtracer) + libtracer.a artifact",
          "timestamp": "2026-07-07T15:05:16+03:00",
          "tree_id": "770d907da5c52724f4fc73ac99d173759c9dd008",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/f74d01eabbd57caff8c1a7f6c88c5c59697103fa"
        },
        "date": 1783425964805,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 260,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 350,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 251.5,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 230,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 241,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 210.6,
            "unit": "ns"
          }
        ]
      },
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
          "id": "153880d619e237fb6d03e28ad66744e001cdc8e7",
          "message": "Merge pull request #295 from avatarsd-llc/feat/esp-idf-archive-name\n\nbuild(esp-idf): ship the component archive as libtracer.a (not liblibtracer.a)",
          "timestamp": "2026-07-07T15:05:56+03:00",
          "tree_id": "3754e6bc390a5eecdd2dbd5c4657c8da19f80864",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/153880d619e237fb6d03e28ad66744e001cdc8e7"
        },
        "date": 1783426005600,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 255,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 270,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 246.6,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 230,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 241,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 215.3,
            "unit": "ns"
          }
        ]
      },
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
          "id": "761eeaa292548cdb1844104b9723b10ba841bcc5",
          "message": "Merge pull request #298 from avatarsd-llc/feat/consume-ergonomics\n\nfeat(core): libtracer::libtracer build-tree alias + document consumption",
          "timestamp": "2026-07-07T15:32:31+03:00",
          "tree_id": "d65782e972e94566d129f8cb2f1857fd9ba4da07",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/761eeaa292548cdb1844104b9723b10ba841bcc5"
        },
        "date": 1783427604203,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 265,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 281,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 248.8,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 230,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 251,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 211.6,
            "unit": "ns"
          }
        ]
      }
    ]
  }
}