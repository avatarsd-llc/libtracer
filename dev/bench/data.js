window.BENCHMARK_DATA = {
  "lastUpdate": 1783545307804,
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
          "id": "16c667f24589c5e6555c269c7d0e8a19cc287ef7",
          "message": "Merge pull request #342 from avatarsd-llc/feat/graph-composite\n\nrefactor(graph): Composite vertex tree replaces the flat full-key map (ADR-0057)",
          "timestamp": "2026-07-08T23:29:49+03:00",
          "tree_id": "10d15fe995cf37c215d88d3e7f57195775a74da5",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/16c667f24589c5e6555c269c7d0e8a19cc287ef7"
        },
        "date": 1783542630068,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 215,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 173.3,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 170,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 230,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 149.9,
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
          "id": "47a01ba032a9c9dccfae02bd9e07e7193f78bcf4",
          "message": "Merge pull request #343 from avatarsd-llc/feat/perf-docs-history\n\nci(perf)+docs: benchmark history surfaced in the docs site; rich per-commit results",
          "timestamp": "2026-07-08T23:44:08+03:00",
          "tree_id": "5e0eb1bd63f01c152351e14a571d94710749b2ba",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/47a01ba032a9c9dccfae02bd9e07e7193f78bcf4"
        },
        "date": 1783543491925,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 240,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 245,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 219.2,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p50 latency",
            "value": 371,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p99 latency",
            "value": 381,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep ns/delivery",
            "value": 44.4,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p50 latency",
            "value": 2984,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p99 latency",
            "value": 3124,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep ns/delivery",
            "value": 23.8,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p50 latency",
            "value": 24016,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p99 latency",
            "value": 32909,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep ns/delivery",
            "value": 23.6,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p50 latency",
            "value": 191833,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p99 latency",
            "value": 225163,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep ns/delivery",
            "value": 23.8,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p50 latency",
            "value": 240,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p99 latency",
            "value": 260,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep ns/delivery",
            "value": 216.8,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p50 latency",
            "value": 240,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p99 latency",
            "value": 251,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep ns/delivery",
            "value": 218.3,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p50 latency",
            "value": 250,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p99 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep ns/delivery",
            "value": 232.6,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p50 latency",
            "value": 360,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p99 latency",
            "value": 461,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep ns/delivery",
            "value": 340.2,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p50 latency",
            "value": 220,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p99 latency",
            "value": 221,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep ns/delivery",
            "value": 193.9,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p50 latency",
            "value": 220,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p99 latency",
            "value": 221,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep ns/delivery",
            "value": 193.7,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 220,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 221,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 193.9,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p50 latency",
            "value": 220,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p99 latency",
            "value": 221,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep ns/delivery",
            "value": 193.4,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p50 latency",
            "value": 220,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p99 latency",
            "value": 221,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep ns/delivery",
            "value": 193.3,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p50 latency",
            "value": 270,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p99 latency",
            "value": 291,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep ns/delivery",
            "value": 250.1,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p50 latency",
            "value": 280,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p99 latency",
            "value": 321,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep ns/delivery",
            "value": 266.3,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p50 latency",
            "value": 300,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p99 latency",
            "value": 371,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep ns/delivery",
            "value": 291.7,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p50 latency",
            "value": 330,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p99 latency",
            "value": 451,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep ns/delivery",
            "value": 328,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p50 latency",
            "value": 381,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p99 latency",
            "value": 601,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 386.3,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p50 latency",
            "value": 310,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p99 latency",
            "value": 942,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep ns/delivery",
            "value": 66.5,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p50 latency",
            "value": 201,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p99 latency",
            "value": 380,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep ns/delivery",
            "value": 176.9,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p50 latency",
            "value": 200,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p99 latency",
            "value": 240,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep ns/delivery",
            "value": 88.5,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p50 latency",
            "value": 381,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p99 latency",
            "value": 501,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep ns/delivery",
            "value": 84,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p50 latency",
            "value": 240,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p99 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep ns/delivery",
            "value": 224.8,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p50 latency",
            "value": 220,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p99 latency",
            "value": 230,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep ns/delivery",
            "value": 196.3,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p50 latency",
            "value": 270,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p99 latency",
            "value": 420,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep ns/delivery",
            "value": 257.2,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep p50 latency",
            "value": 50,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep p99 latency",
            "value": 70,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep ns/delivery",
            "value": 21.7,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p50 latency",
            "value": 50,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p99 latency",
            "value": 60,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep ns/delivery",
            "value": 24.9,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep p50 latency",
            "value": 60,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep p99 latency",
            "value": 80,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep ns/delivery",
            "value": 32.3,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p50 latency",
            "value": 80,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p99 latency",
            "value": 100,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep ns/delivery",
            "value": 51.5,
            "unit": "ns"
          },
          {
            "name": "heap bytes per forward (probe)",
            "value": 0,
            "unit": "bytes"
          },
          {
            "name": "heap allocs per forward (probe)",
            "value": 0,
            "unit": "allocs"
          },
          {
            "name": "heap bytes per terminus (probe)",
            "value": 929,
            "unit": "bytes"
          },
          {
            "name": "heap allocs per terminus (probe)",
            "value": 9,
            "unit": "allocs"
          },
          {
            "name": "bench_libtracer max RSS",
            "value": 29800,
            "unit": "KB"
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
          "id": "706fd475b91c4f258d6c2bc64500b8df659191e8",
          "message": "Merge pull request #344 from avatarsd-llc/feat/effective-acl\n\nfeat(graph): effective_acl_t + subtree-precise cached ACE merge (ADR-0050 completed)",
          "timestamp": "2026-07-09T00:13:52+03:00",
          "tree_id": "6c591495ed2a558c02f393e552652f6142b77571",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/706fd475b91c4f258d6c2bc64500b8df659191e8"
        },
        "date": 1783545281813,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 300,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 336,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 287.1,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p50 latency",
            "value": 441,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p99 latency",
            "value": 452,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep ns/delivery",
            "value": 54.3,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p50 latency",
            "value": 3266,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p99 latency",
            "value": 3427,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep ns/delivery",
            "value": 25.7,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p50 latency",
            "value": 26449,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p99 latency",
            "value": 36478,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep ns/delivery",
            "value": 26.2,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p50 latency",
            "value": 212438,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p99 latency",
            "value": 238757,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep ns/delivery",
            "value": 26.4,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p50 latency",
            "value": 301,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p99 latency",
            "value": 311,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep ns/delivery",
            "value": 287.4,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p50 latency",
            "value": 300,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p99 latency",
            "value": 311,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep ns/delivery",
            "value": 285.1,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p50 latency",
            "value": 311,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p99 latency",
            "value": 341,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep ns/delivery",
            "value": 302.1,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p50 latency",
            "value": 441,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p99 latency",
            "value": 511,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep ns/delivery",
            "value": 422.9,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p50 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p99 latency",
            "value": 281,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep ns/delivery",
            "value": 254.2,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p50 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p99 latency",
            "value": 281,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep ns/delivery",
            "value": 253.6,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 281,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 254,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p50 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p99 latency",
            "value": 281,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep ns/delivery",
            "value": 254.6,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p50 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p99 latency",
            "value": 281,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep ns/delivery",
            "value": 253.8,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p50 latency",
            "value": 341,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p99 latency",
            "value": 351,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep ns/delivery",
            "value": 326.3,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p50 latency",
            "value": 371,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p99 latency",
            "value": 391,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep ns/delivery",
            "value": 363.6,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p50 latency",
            "value": 401,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p99 latency",
            "value": 461,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep ns/delivery",
            "value": 390.2,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p50 latency",
            "value": 421,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p99 latency",
            "value": 551,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep ns/delivery",
            "value": 416.7,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p50 latency",
            "value": 441,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p99 latency",
            "value": 551,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 442.8,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p50 latency",
            "value": 391,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p99 latency",
            "value": 1151,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep ns/delivery",
            "value": 84.9,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p50 latency",
            "value": 261,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p99 latency",
            "value": 291,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep ns/delivery",
            "value": 242.3,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p50 latency",
            "value": 261,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p99 latency",
            "value": 350,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep ns/delivery",
            "value": 119,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p50 latency",
            "value": 441,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p99 latency",
            "value": 501,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep ns/delivery",
            "value": 97.5,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p50 latency",
            "value": 311,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p99 latency",
            "value": 330,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep ns/delivery",
            "value": 298.7,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p50 latency",
            "value": 290,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p99 latency",
            "value": 530,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep ns/delivery",
            "value": 263,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p50 latency",
            "value": 331,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p99 latency",
            "value": 381,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep ns/delivery",
            "value": 325.5,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p50 latency",
            "value": 90,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p99 latency",
            "value": 101,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep ns/delivery",
            "value": 69.3,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p99 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep ns/delivery",
            "value": 31.6,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep p50 latency",
            "value": 50,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep p99 latency",
            "value": 51,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep ns/delivery",
            "value": 25.6,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p50 latency",
            "value": 60,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p99 latency",
            "value": 71,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep ns/delivery",
            "value": 40.1,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep p50 latency",
            "value": 50,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep p99 latency",
            "value": 61,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep ns/delivery",
            "value": 29.9,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p50 latency",
            "value": 60,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p99 latency",
            "value": 70,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep ns/delivery",
            "value": 38.5,
            "unit": "ns"
          },
          {
            "name": "heap bytes per forward (probe)",
            "value": 0,
            "unit": "bytes"
          },
          {
            "name": "heap allocs per forward (probe)",
            "value": 0,
            "unit": "allocs"
          },
          {
            "name": "heap bytes per terminus (probe)",
            "value": 929,
            "unit": "bytes"
          },
          {
            "name": "heap allocs per terminus (probe)",
            "value": 9,
            "unit": "allocs"
          },
          {
            "name": "bench_libtracer max RSS",
            "value": 30052,
            "unit": "KB"
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
          "id": "7910d52853052cc9acc1e488beec05b3d9fc63b6",
          "message": "Merge pull request #346 from avatarsd-llc/feat/msquic-endpoint-base\n\nrefactor(net): msquic_endpoint_t — the QUIC-mechanical layer extracted from both msquic transports",
          "timestamp": "2026-07-09T00:13:56+03:00",
          "tree_id": "b1e512fe1c24d54f9d9a6cacaf7f45ae93477653",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/7910d52853052cc9acc1e488beec05b3d9fc63b6"
        },
        "date": 1783545303589,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 301,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 321,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 288.8,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p50 latency",
            "value": 450,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p99 latency",
            "value": 471,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep ns/delivery",
            "value": 54.9,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p50 latency",
            "value": 3256,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p99 latency",
            "value": 3436,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep ns/delivery",
            "value": 25.9,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p50 latency",
            "value": 26309,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p99 latency",
            "value": 36878,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep ns/delivery",
            "value": 26.2,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p50 latency",
            "value": 214391,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p99 latency",
            "value": 239599,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep ns/delivery",
            "value": 27,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p50 latency",
            "value": 301,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p99 latency",
            "value": 321,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep ns/delivery",
            "value": 288.2,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p50 latency",
            "value": 301,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p99 latency",
            "value": 321,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep ns/delivery",
            "value": 287.2,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p50 latency",
            "value": 320,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p99 latency",
            "value": 351,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep ns/delivery",
            "value": 302.5,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p50 latency",
            "value": 440,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p99 latency",
            "value": 651,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep ns/delivery",
            "value": 420,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p50 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p99 latency",
            "value": 301,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep ns/delivery",
            "value": 253.5,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p50 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p99 latency",
            "value": 301,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep ns/delivery",
            "value": 254.1,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 330,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 253.3,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p50 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p99 latency",
            "value": 301,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep ns/delivery",
            "value": 254,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p50 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p99 latency",
            "value": 301,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep ns/delivery",
            "value": 253.6,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p50 latency",
            "value": 350,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p99 latency",
            "value": 361,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep ns/delivery",
            "value": 332.7,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p50 latency",
            "value": 381,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p99 latency",
            "value": 401,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep ns/delivery",
            "value": 372,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p50 latency",
            "value": 410,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p99 latency",
            "value": 471,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep ns/delivery",
            "value": 400.9,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p50 latency",
            "value": 431,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p99 latency",
            "value": 531,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep ns/delivery",
            "value": 439.4,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p50 latency",
            "value": 451,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p99 latency",
            "value": 622,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 453,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p50 latency",
            "value": 491,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p99 latency",
            "value": 2094,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep ns/delivery",
            "value": 86.1,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p50 latency",
            "value": 260,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p99 latency",
            "value": 290,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep ns/delivery",
            "value": 236.9,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p50 latency",
            "value": 310,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p99 latency",
            "value": 420,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep ns/delivery",
            "value": 162.3,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p50 latency",
            "value": 421,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p99 latency",
            "value": 511,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep ns/delivery",
            "value": 97.2,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p50 latency",
            "value": 311,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p99 latency",
            "value": 490,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep ns/delivery",
            "value": 300.3,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p50 latency",
            "value": 281,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p99 latency",
            "value": 310,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep ns/delivery",
            "value": 265.2,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p50 latency",
            "value": 331,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p99 latency",
            "value": 401,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep ns/delivery",
            "value": 326.9,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p50 latency",
            "value": 90,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p99 latency",
            "value": 101,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep ns/delivery",
            "value": 66.5,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p99 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep ns/delivery",
            "value": 31.7,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep p50 latency",
            "value": 50,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep p99 latency",
            "value": 51,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep ns/delivery",
            "value": 25.5,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p50 latency",
            "value": 50,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p99 latency",
            "value": 71,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep ns/delivery",
            "value": 44.5,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep p50 latency",
            "value": 50,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep p99 latency",
            "value": 61,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep ns/delivery",
            "value": 30,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p50 latency",
            "value": 60,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p99 latency",
            "value": 70,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep ns/delivery",
            "value": 38.6,
            "unit": "ns"
          },
          {
            "name": "heap bytes per forward (probe)",
            "value": 0,
            "unit": "bytes"
          },
          {
            "name": "heap allocs per forward (probe)",
            "value": 0,
            "unit": "allocs"
          },
          {
            "name": "heap bytes per terminus (probe)",
            "value": 929,
            "unit": "bytes"
          },
          {
            "name": "heap allocs per terminus (probe)",
            "value": 9,
            "unit": "allocs"
          },
          {
            "name": "bench_libtracer max RSS",
            "value": 29996,
            "unit": "KB"
          }
        ]
      }
    ],
    "libtracer in-process throughput (deliveries/s, bigger is better)": [
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
          "id": "47a01ba032a9c9dccfae02bd9e07e7193f78bcf4",
          "message": "Merge pull request #343 from avatarsd-llc/feat/perf-docs-history\n\nci(perf)+docs: benchmark history surfaced in the docs site; rich per-commit results",
          "timestamp": "2026-07-08T23:44:08+03:00",
          "tree_id": "5e0eb1bd63f01c152351e14a571d94710749b2ba",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/47a01ba032a9c9dccfae02bd9e07e7193f78bcf4"
        },
        "date": 1783543493732,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep throughput",
            "value": 4563073.5,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8/1ep throughput",
            "value": 22527702,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan128/1ep throughput",
            "value": 42096333,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan1024/1ep throughput",
            "value": 42309005,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8192/1ep throughput",
            "value": 42095791,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1B/fan1/1ep throughput",
            "value": 4612296,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8B/fan1/1ep throughput",
            "value": 4580174,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1024B/fan1/1ep throughput",
            "value": 4298682,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8192B/fan1/1ep throughput",
            "value": 2939486,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep throughput",
            "value": 5156222,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep throughput",
            "value": 5163667,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep throughput",
            "value": 5158345,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep throughput",
            "value": 5169992,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep throughput",
            "value": 5172573,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1ep throughput",
            "value": 3998985,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8ep throughput",
            "value": 3754616,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/128ep throughput",
            "value": 3428211,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep throughput",
            "value": 3049183,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep throughput",
            "value": 2588798,
            "unit": "deliveries/s"
          },
          {
            "name": "mixed 0B/fan6/128ep throughput",
            "value": 15042757,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep throughput",
            "value": 5653334,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep throughput",
            "value": 11302170,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep throughput",
            "value": 11910478,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep throughput",
            "value": 4447417,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep throughput",
            "value": 5093386,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep throughput",
            "value": 3887804,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n1 512B/fan1/1ep throughput",
            "value": 46057460,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n2 512B/fan1/1ep throughput",
            "value": 40201020,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n4 512B/fan1/1ep throughput",
            "value": 30942917,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n8 512B/fan1/1ep throughput",
            "value": 19425468,
            "unit": "deliveries/s"
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
          "id": "706fd475b91c4f258d6c2bc64500b8df659191e8",
          "message": "Merge pull request #344 from avatarsd-llc/feat/effective-acl\n\nfeat(graph): effective_acl_t + subtree-precise cached ACE merge (ADR-0050 completed)",
          "timestamp": "2026-07-09T00:13:52+03:00",
          "tree_id": "6c591495ed2a558c02f393e552652f6142b77571",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/706fd475b91c4f258d6c2bc64500b8df659191e8"
        },
        "date": 1783545284500,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep throughput",
            "value": 3483485,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8/1ep throughput",
            "value": 18417222,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan128/1ep throughput",
            "value": 38899752,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan1024/1ep throughput",
            "value": 38167100,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8192/1ep throughput",
            "value": 37911924,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1B/fan1/1ep throughput",
            "value": 3479453,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8B/fan1/1ep throughput",
            "value": 3507282,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1024B/fan1/1ep throughput",
            "value": 3309955,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8192B/fan1/1ep throughput",
            "value": 2364414,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep throughput",
            "value": 3933208,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep throughput",
            "value": 3943969,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep throughput",
            "value": 3936565,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep throughput",
            "value": 3928488,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep throughput",
            "value": 3940278,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1ep throughput",
            "value": 3064791,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8ep throughput",
            "value": 2750183,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/128ep throughput",
            "value": 2562735,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep throughput",
            "value": 2399572,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep throughput",
            "value": 2258587,
            "unit": "deliveries/s"
          },
          {
            "name": "mixed 0B/fan6/128ep throughput",
            "value": 11778325,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep throughput",
            "value": 4126911,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep throughput",
            "value": 8399960,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep throughput",
            "value": 10259769,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep throughput",
            "value": 3348075,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep throughput",
            "value": 3802755,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep throughput",
            "value": 3072477,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep throughput",
            "value": 14426188,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep throughput",
            "value": 31671779,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n1 512B/fan1/1ep throughput",
            "value": 39076440,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n2 512B/fan1/1ep throughput",
            "value": 24925118,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n4 512B/fan1/1ep throughput",
            "value": 33471264,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n8 512B/fan1/1ep throughput",
            "value": 25948465,
            "unit": "deliveries/s"
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
          "id": "7910d52853052cc9acc1e488beec05b3d9fc63b6",
          "message": "Merge pull request #346 from avatarsd-llc/feat/msquic-endpoint-base\n\nrefactor(net): msquic_endpoint_t — the QUIC-mechanical layer extracted from both msquic transports",
          "timestamp": "2026-07-09T00:13:56+03:00",
          "tree_id": "b1e512fe1c24d54f9d9a6cacaf7f45ae93477653",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/7910d52853052cc9acc1e488beec05b3d9fc63b6"
        },
        "date": 1783545306910,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep throughput",
            "value": 3463135.5,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8/1ep throughput",
            "value": 18215569,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan128/1ep throughput",
            "value": 38618652,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan1024/1ep throughput",
            "value": 38204911,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8192/1ep throughput",
            "value": 37002393,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1B/fan1/1ep throughput",
            "value": 3469915,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8B/fan1/1ep throughput",
            "value": 3482403,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1024B/fan1/1ep throughput",
            "value": 3305395,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8192B/fan1/1ep throughput",
            "value": 2380894,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep throughput",
            "value": 3945408,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep throughput",
            "value": 3935027,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep throughput",
            "value": 3948473,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep throughput",
            "value": 3936916,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep throughput",
            "value": 3942511,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1ep throughput",
            "value": 3005741,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8ep throughput",
            "value": 2688044,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/128ep throughput",
            "value": 2494518,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep throughput",
            "value": 2275649,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep throughput",
            "value": 2207573,
            "unit": "deliveries/s"
          },
          {
            "name": "mixed 0B/fan6/128ep throughput",
            "value": 11608041,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep throughput",
            "value": 4220594,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep throughput",
            "value": 6159575,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep throughput",
            "value": 10283976,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep throughput",
            "value": 3330143,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep throughput",
            "value": 3771295,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep throughput",
            "value": 3059447,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep throughput",
            "value": 15045693,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep throughput",
            "value": 31547528,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n1 512B/fan1/1ep throughput",
            "value": 39163205,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n2 512B/fan1/1ep throughput",
            "value": 22456942,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n4 512B/fan1/1ep throughput",
            "value": 33353426,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n8 512B/fan1/1ep throughput",
            "value": 25901882,
            "unit": "deliveries/s"
          }
        ]
      }
    ]
  }
}