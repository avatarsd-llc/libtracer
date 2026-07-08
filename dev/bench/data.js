window.BENCHMARK_DATA = {
  "lastUpdate": 1783541713396,
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
          "id": "62765977e61c8b7647cec1f8d2b2403c4e42989c",
          "message": "Merge pull request #299 from avatarsd-llc/test/install-consume-ci\n\ntest(ci): guard the CMake install/export path (find_package regression test)",
          "timestamp": "2026-07-07T15:50:21+03:00",
          "tree_id": "6f6a4e8059c90cdd6e3be8afca4b84b76d5b689a",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/62765977e61c8b7647cec1f8d2b2403c4e42989c"
        },
        "date": 1783428665276,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 265,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 290,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 254.9,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 240,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 250,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 219.5,
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
          "id": "fb8addfd02b44c226545e4e747532043da88c6a2",
          "message": "Merge pull request #305 from avatarsd-llc/docs/changelog-version-consistency\n\ndocs(changelog): record the git-derived version scheme",
          "timestamp": "2026-07-07T17:04:30+03:00",
          "tree_id": "7ac26ce818ad6b18252580dc19bd034406a78318",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/fb8addfd02b44c226545e4e747532043da88c6a2"
        },
        "date": 1783433116211,
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
            "value": 246.7,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 230,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 260,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 214.7,
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
          "id": "5654baa94d7622b2d4d10e53c67b389fc38d5968",
          "message": "Merge pull request #308 from avatarsd-llc/release/v0.3.0-changelog\n\ndocs(changelog): cut the 0.3.0 release section",
          "timestamp": "2026-07-07T19:18:46+03:00",
          "tree_id": "4b6d00e7185d2c196d48e96734725938c16be196",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/5654baa94d7622b2d4d10e53c67b389fc38d5968"
        },
        "date": 1783441168346,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 245,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 276,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 225.1,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 211,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 231,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 197.3,
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
          "id": "f6165c611fa0c356a7837dec8f92a1424336a115",
          "message": "Merge pull request #309 from avatarsd-llc/chore/version-single-source-of-truth\n\nbuild: single source of truth for the core release version",
          "timestamp": "2026-07-07T20:03:46+03:00",
          "tree_id": "adae1b38f41415b88caff409ebc07cc895e1ffcd",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/f6165c611fa0c356a7837dec8f92a1424336a115"
        },
        "date": 1783443870339,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 235,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 276,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 218.7,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 211,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 250,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 194,
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
          "id": "2246eb521b1dc4793b773259540d82e0952a2cc8",
          "message": "Merge pull request #318 from avatarsd-llc/fix/esp-component-self-contained\n\nfix(esp): publish a self-contained ESP component archive",
          "timestamp": "2026-07-07T22:20:57+03:00",
          "tree_id": "fba1c316aa1c34669e0b0b9b229ee26e3b40928f",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/2246eb521b1dc4793b773259540d82e0952a2cc8"
        },
        "date": 1783452100087,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 255,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 266,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 242.5,
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
            "value": 208.6,
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
          "id": "b16a822ed2ea49e319670c5fbbf8bf1135b1fa37",
          "message": "Merge pull request #328 from avatarsd-llc/feat/vertex-handle\n\nfeat(graph): opaque vertex_handle_t + infallible register_vertex (ADR-0056) [HELD for 0.4.0]",
          "timestamp": "2026-07-08T00:47:25+03:00",
          "tree_id": "b13c47939afc6d8025ca8fd461923a09f6910888",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/b16a822ed2ea49e319670c5fbbf8bf1135b1fa37"
        },
        "date": 1783460901988,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 260,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 295,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 247.1,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 220,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 240,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 202.3,
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
          "id": "b04ab8d5781fd1c980a972f9bc7aff8414b92b4e",
          "message": "Merge pull request #329 from avatarsd-llc/feat/perf-tier-a\n\nperf(core): tier-A wins — hardware CRC-32C, transparent key lookup, receiver snapshot (0.3.0)",
          "timestamp": "2026-07-08T01:27:50+03:00",
          "tree_id": "c6d85d0376e321f1c34ca815fd09731e71bd3611",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/b04ab8d5781fd1c980a972f9bc7aff8414b92b4e"
        },
        "date": 1783463311848,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 245,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 280,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 224.9,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 220,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 281,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 190.7,
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
          "id": "6060f214a13fab323692ed2869822a95512cdff2",
          "message": "Merge pull request #330 from avatarsd-llc/feat/cmake-modularity\n\nfeat(build): per-module CMake modularity — compile only the modules you link (0.3.0)",
          "timestamp": "2026-07-08T01:55:13+03:00",
          "tree_id": "89f50d1793c6f9ce3056a833d7401884b5fa8fe5",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/6060f214a13fab323692ed2869822a95512cdff2"
        },
        "date": 1783464959752,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 260,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 296,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 236.8,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 221,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 241,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 206.8,
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
          "id": "fdf49f2ea5d9831561e22c8cb4c2782e39e7bb06",
          "message": "Merge pull request #332 from avatarsd-llc/feat/platformio-esp32-can\n\nfeat(platformio): best-effort ESP32 CAN via build.extraScript + integration-status honesty",
          "timestamp": "2026-07-08T08:18:49+03:00",
          "tree_id": "a53540c4833d92bab9c765bb5fed3ebe333ccaba",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/fdf49f2ea5d9831561e22c8cb4c2782e39e7bb06"
        },
        "date": 1783487976613,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 275,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 300,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 238.4,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 221,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 241,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 206.2,
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
          "id": "df676548a583cf42384d20625e69d1e07c505f66",
          "message": "Merge pull request #333 from avatarsd-llc/release/0.3.0-changelog-cut\n\ndocs(changelog): fold pre-tag work into [0.3.0] (release prep)",
          "timestamp": "2026-07-08T08:25:37+03:00",
          "tree_id": "2a749d03a39bdd81efd1119650de852066646135",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/df676548a583cf42384d20625e69d1e07c505f66"
        },
        "date": 1783488391306,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 240,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 260,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 220.9,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 211,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 241,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 193,
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
          "id": "c17aef77a2642f80a1487bb24b14ecedc6e8ec97",
          "message": "Merge pull request #337 from avatarsd-llc/feat/receiver-slot\n\nrefactor(net): receiver_slot_t — one delivery-tier slot, fn-ptr receivers",
          "timestamp": "2026-07-08T22:18:57+03:00",
          "tree_id": "9313dcf7c34ebda347dd7bb3750134988fd233d9",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/c17aef77a2642f80a1487bb24b14ecedc6e8ec97"
        },
        "date": 1783538384238,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 231,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 316,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 219.6,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 210,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 250,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 191.1,
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
          "id": "effacc445a48c4299b75cbf657c081d493450d77",
          "message": "Merge pull request #338 from avatarsd-llc/feat/vertex-verbs\n\nrefactor(graph): vertex_t verb interface — storage/readiness/edges behind one seam",
          "timestamp": "2026-07-08T22:40:47+03:00",
          "tree_id": "c8cf3e9f72ebe73a47f2022ee639c98b6c1ed2e0",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/effacc445a48c4299b75cbf657c081d493450d77"
        },
        "date": 1783539695566,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 325,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 346,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 311.7,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 300,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 311,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 278.7,
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
          "id": "00b68fdab591b1cba4437579a1115f84336eb6ed",
          "message": "Merge pull request #339 from avatarsd-llc/docs/doxygen-everywhere\n\ndocs(style): doxygen-capable /** @brief */ comments everywhere — .cpp and bindings too",
          "timestamp": "2026-07-08T22:50:47+03:00",
          "tree_id": "3482d218fa99543fb25b85a70851d5fe2d9c24dd",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/00b68fdab591b1cba4437579a1115f84336eb6ed"
        },
        "date": 1783540296358,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 321,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 360,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 311.5,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 300,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 320,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 278.7,
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
          "id": "ff1993946363167890298222138e6fa6d1d3e339",
          "message": "Merge pull request #340 from avatarsd-llc/feat/lazy-validation\n\nrefactor(wire): resource-bounded walk stack, lazy ingress, kMaxDepth deleted (RFC-0006)",
          "timestamp": "2026-07-08T23:14:06+03:00",
          "tree_id": "3034a17491017a183a02c0645fe9085db53bce36",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/ff1993946363167890298222138e6fa6d1d3e339"
        },
        "date": 1783541712998,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 296,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 320,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 284.5,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 266,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 303,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 259.1,
            "unit": "ns"
          }
        ]
      }
    ]
  }
}