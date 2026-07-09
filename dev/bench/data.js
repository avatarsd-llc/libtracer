window.BENCHMARK_DATA = {
  "lastUpdate": 1783628049089,
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
          "id": "d115aa09b444980b47a91c03ac2f74b93799f746",
          "message": "Merge pull request #337 from avatarsd-llc/feat/receiver-slot\n\nrefactor(net): receiver_slot_t — one delivery-tier slot, fn-ptr receivers",
          "timestamp": "2026-07-08T22:18:57+03:00",
          "tree_id": "9313dcf7c34ebda347dd7bb3750134988fd233d9",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/d115aa09b444980b47a91c03ac2f74b93799f746"
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
          "id": "5d5e7f3ff8b070e20a9a92ac6deae80fa6fbc872",
          "message": "Merge pull request #338 from avatarsd-llc/feat/vertex-verbs\n\nrefactor(graph): vertex_t verb interface — storage/readiness/edges behind one seam",
          "timestamp": "2026-07-08T22:40:47+03:00",
          "tree_id": "c8cf3e9f72ebe73a47f2022ee639c98b6c1ed2e0",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/5d5e7f3ff8b070e20a9a92ac6deae80fa6fbc872"
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
          "id": "98be7c941ecdb4264eb0cf8fd1b7d1359a539b41",
          "message": "Merge pull request #339 from avatarsd-llc/docs/doxygen-everywhere\n\ndocs(style): doxygen-capable /** @brief */ comments everywhere — .cpp and bindings too",
          "timestamp": "2026-07-08T22:50:47+03:00",
          "tree_id": "3482d218fa99543fb25b85a70851d5fe2d9c24dd",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/98be7c941ecdb4264eb0cf8fd1b7d1359a539b41"
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
          "id": "fc89040a0144e2175464ba0387a998917109ed0f",
          "message": "Merge pull request #340 from avatarsd-llc/feat/lazy-validation\n\nrefactor(wire): resource-bounded walk stack, lazy ingress, kMaxDepth deleted (RFC-0006)",
          "timestamp": "2026-07-08T23:14:06+03:00",
          "tree_id": "3034a17491017a183a02c0645fe9085db53bce36",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/fc89040a0144e2175464ba0387a998917109ed0f"
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
          "id": "65f0b8f0eabb27135726acbd9193f9b6edb8b3a1",
          "message": "Merge pull request #342 from avatarsd-llc/feat/graph-composite\n\nrefactor(graph): Composite vertex tree replaces the flat full-key map (ADR-0057)",
          "timestamp": "2026-07-08T23:29:49+03:00",
          "tree_id": "10d15fe995cf37c215d88d3e7f57195775a74da5",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/65f0b8f0eabb27135726acbd9193f9b6edb8b3a1"
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
          "id": "199d2158e39d738691fd115626ce4334e6ce01f4",
          "message": "Merge pull request #343 from avatarsd-llc/feat/perf-docs-history\n\nci(perf)+docs: benchmark history surfaced in the docs site; rich per-commit results",
          "timestamp": "2026-07-08T23:44:08+03:00",
          "tree_id": "5e0eb1bd63f01c152351e14a571d94710749b2ba",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/199d2158e39d738691fd115626ce4334e6ce01f4"
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
          "id": "fd1fb2da688d6cd15439bf797a001f49c1d3fb5b",
          "message": "Merge pull request #344 from avatarsd-llc/feat/effective-acl\n\nfeat(graph): effective_acl_t + subtree-precise cached ACE merge (ADR-0050 completed)",
          "timestamp": "2026-07-09T00:13:52+03:00",
          "tree_id": "6c591495ed2a558c02f393e552652f6142b77571",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/fd1fb2da688d6cd15439bf797a001f49c1d3fb5b"
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
          "id": "97070b0fc341b4948646ae2f924c649c730cdc5b",
          "message": "Merge pull request #346 from avatarsd-llc/feat/msquic-endpoint-base\n\nrefactor(net): msquic_endpoint_t — the QUIC-mechanical layer extracted from both msquic transports",
          "timestamp": "2026-07-09T00:13:56+03:00",
          "tree_id": "b1e512fe1c24d54f9d9a6cacaf7f45ae93477653",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/97070b0fc341b4948646ae2f924c649c730cdc5b"
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
          "id": "c9481238532063d902ec50a63910dcca7c72d643",
          "message": "Merge pull request #348 from avatarsd-llc/feat/perf-page-unified-history\n\ndocs(perf): unified performance page — in-page all-commit history alongside Zenoh + live test rollup",
          "timestamp": "2026-07-09T00:22:58+03:00",
          "tree_id": "ea6f4f662cb463bbdc9f0707bd66af833bc8fae8",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/c9481238532063d902ec50a63910dcca7c72d643"
        },
        "date": 1783545829829,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 300,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 321,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 288.9,
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
            "value": 59.5,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p50 latency",
            "value": 3256,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p99 latency",
            "value": 3417,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep ns/delivery",
            "value": 26.1,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p50 latency",
            "value": 26550,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p99 latency",
            "value": 38401,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep ns/delivery",
            "value": 27.6,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p50 latency",
            "value": 213800,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p99 latency",
            "value": 331029,
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
            "value": 320,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep ns/delivery",
            "value": 290.4,
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
            "value": 286.5,
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
            "value": 303.3,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p50 latency",
            "value": 440,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p99 latency",
            "value": 631,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep ns/delivery",
            "value": 421.3,
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
            "value": 253.4,
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
            "value": 253.2,
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
            "value": 253.6,
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
            "value": 253,
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
            "value": 329.3,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p50 latency",
            "value": 371,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p99 latency",
            "value": 410,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep ns/delivery",
            "value": 365.4,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p50 latency",
            "value": 401,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p99 latency",
            "value": 470,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep ns/delivery",
            "value": 391.5,
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
            "value": 419.8,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p50 latency",
            "value": 531,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p99 latency",
            "value": 992,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 500.6,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p50 latency",
            "value": 381,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p99 latency",
            "value": 1223,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep ns/delivery",
            "value": 86.4,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p50 latency",
            "value": 261,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p99 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep ns/delivery",
            "value": 237.1,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p50 latency",
            "value": 291,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p99 latency",
            "value": 361,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep ns/delivery",
            "value": 118.7,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p50 latency",
            "value": 411,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p99 latency",
            "value": 440,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep ns/delivery",
            "value": 96.8,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p50 latency",
            "value": 310,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p99 latency",
            "value": 321,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep ns/delivery",
            "value": 300.5,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p50 latency",
            "value": 281,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p99 latency",
            "value": 291,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep ns/delivery",
            "value": 262.3,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p50 latency",
            "value": 340,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p99 latency",
            "value": 381,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep ns/delivery",
            "value": 322.6,
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
            "value": 67.4,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p99 latency",
            "value": 161,
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
            "value": 25.2,
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
            "value": 27.7,
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
            "value": 38.8,
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
            "value": 30004,
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
          "id": "6a55d902cd9aaa00cae86cc759d0da91a06fb974",
          "message": "Merge pull request #347 from avatarsd-llc/feat/fwd-frame-view\n\nrefactor(net): fwd_frame_view — the FWD offset-dispatch cluster becomes a unit-testable public header",
          "timestamp": "2026-07-09T00:23:20+03:00",
          "tree_id": "7d59c421e52d4882e7d43ffcf09dc7fab3d9995c",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/6a55d902cd9aaa00cae86cc759d0da91a06fb974"
        },
        "date": 1783545852806,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 300,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 311,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 287.5,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p50 latency",
            "value": 450,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p99 latency",
            "value": 962,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep ns/delivery",
            "value": 59,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p50 latency",
            "value": 3246,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p99 latency",
            "value": 3406,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep ns/delivery",
            "value": 25.7,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p50 latency",
            "value": 26339,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p99 latency",
            "value": 40837,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep ns/delivery",
            "value": 26.2,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p50 latency",
            "value": 214082,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p99 latency",
            "value": 236104,
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
            "value": 287.6,
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
            "value": 285.8,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p50 latency",
            "value": 320,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p99 latency",
            "value": 531,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep ns/delivery",
            "value": 302,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p50 latency",
            "value": 440,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p99 latency",
            "value": 602,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep ns/delivery",
            "value": 419.6,
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
            "value": 281,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep ns/delivery",
            "value": 254.2,
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
            "value": 253.6,
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
            "value": 254.5,
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
            "value": 253.7,
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
            "value": 327,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p50 latency",
            "value": 380,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p99 latency",
            "value": 391,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep ns/delivery",
            "value": 366.3,
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
            "value": 393.6,
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
            "value": 422.2,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p50 latency",
            "value": 441,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p99 latency",
            "value": 571,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 449.7,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p50 latency",
            "value": 390,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p99 latency",
            "value": 1412,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep ns/delivery",
            "value": 86.2,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p50 latency",
            "value": 260,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p99 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep ns/delivery",
            "value": 236.4,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p50 latency",
            "value": 291,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p99 latency",
            "value": 351,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep ns/delivery",
            "value": 119,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p50 latency",
            "value": 411,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p99 latency",
            "value": 441,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep ns/delivery",
            "value": 97.1,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p50 latency",
            "value": 310,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p99 latency",
            "value": 321,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep ns/delivery",
            "value": 297.7,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p50 latency",
            "value": 281,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p99 latency",
            "value": 301,
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
            "value": 380,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep ns/delivery",
            "value": 321.1,
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
            "value": 68.7,
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
            "value": 32.1,
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
            "value": 37.8,
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
            "value": 30012,
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
          "id": "c15f7087aaba0b9775dbe421ffa0ace6053aa2d6",
          "message": "Merge pull request #349 from avatarsd-llc/feat/grab-bag-smalls\n\nrefactor: grab-bag — stream_endpoint_t, config_reader_t, kMaxFrame dedup, view_as_tlv fold",
          "timestamp": "2026-07-09T01:07:46+03:00",
          "tree_id": "dd5e9db4cde5bfc0849817ea9e53c9d7fed6a13b",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/c15f7087aaba0b9775dbe421ffa0ace6053aa2d6"
        },
        "date": 1783548514715,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 300,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 316,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 289.7,
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
            "value": 54.7,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p50 latency",
            "value": 3216,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p99 latency",
            "value": 3857,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep ns/delivery",
            "value": 25.4,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p50 latency",
            "value": 25958,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p99 latency",
            "value": 35596,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep ns/delivery",
            "value": 25.7,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p50 latency",
            "value": 211063,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p99 latency",
            "value": 258081,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep ns/delivery",
            "value": 26.7,
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
            "value": 291,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p50 latency",
            "value": 301,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p99 latency",
            "value": 361,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep ns/delivery",
            "value": 288.9,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p50 latency",
            "value": 311,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p99 latency",
            "value": 531,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep ns/delivery",
            "value": 303.1,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p50 latency",
            "value": 440,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p99 latency",
            "value": 741,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep ns/delivery",
            "value": 419.5,
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
            "value": 254.8,
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
            "value": 254.8,
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
            "value": 254.6,
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
            "value": 255.3,
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
            "value": 255.7,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p50 latency",
            "value": 341,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p99 latency",
            "value": 361,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep ns/delivery",
            "value": 331.8,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p50 latency",
            "value": 380,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p99 latency",
            "value": 391,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep ns/delivery",
            "value": 366.3,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p50 latency",
            "value": 401,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p99 latency",
            "value": 471,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep ns/delivery",
            "value": 398.2,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p50 latency",
            "value": 431,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p99 latency",
            "value": 561,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep ns/delivery",
            "value": 425,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p50 latency",
            "value": 451,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p99 latency",
            "value": 611,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 449.7,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p50 latency",
            "value": 400,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p99 latency",
            "value": 1153,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep ns/delivery",
            "value": 87.5,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p50 latency",
            "value": 260,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p99 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep ns/delivery",
            "value": 236.9,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p50 latency",
            "value": 291,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p99 latency",
            "value": 351,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep ns/delivery",
            "value": 120.6,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p50 latency",
            "value": 431,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p99 latency",
            "value": 531,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep ns/delivery",
            "value": 97.1,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p50 latency",
            "value": 311,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p99 latency",
            "value": 321,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep ns/delivery",
            "value": 300.7,
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
            "value": 263.4,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p50 latency",
            "value": 331,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p99 latency",
            "value": 371,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep ns/delivery",
            "value": 319.4,
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
            "value": 66.4,
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
            "value": 31.5,
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
            "value": 25.2,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p50 latency",
            "value": 50,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p99 latency",
            "value": 80,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep ns/delivery",
            "value": 27.3,
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
            "value": 30060,
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
          "id": "757d15858ca9a9b7243a9933a1a6215ad784e24a",
          "message": "Merge pull request #350 from avatarsd-llc/feat/perf-charts-unified\n\ndocs(perf): unified family trend charts with release markers (trend/sweep/heatmap/3D)",
          "timestamp": "2026-07-09T01:29:29+03:00",
          "tree_id": "55b46771d5d1869da788c9db38c6c8d1b15341b5",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/757d15858ca9a9b7243a9933a1a6215ad784e24a"
        },
        "date": 1783549818654,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 300,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 411,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 289.2,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p50 latency",
            "value": 441,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p99 latency",
            "value": 722,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep ns/delivery",
            "value": 54.9,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p50 latency",
            "value": 3125,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p99 latency",
            "value": 5210,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep ns/delivery",
            "value": 24.8,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p50 latency",
            "value": 25568,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p99 latency",
            "value": 39434,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep ns/delivery",
            "value": 25.4,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p50 latency",
            "value": 203511,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p99 latency",
            "value": 228228,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep ns/delivery",
            "value": 25.1,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p50 latency",
            "value": 301,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p99 latency",
            "value": 341,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep ns/delivery",
            "value": 290.2,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p50 latency",
            "value": 300,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p99 latency",
            "value": 341,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep ns/delivery",
            "value": 288.3,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p50 latency",
            "value": 311,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p99 latency",
            "value": 351,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep ns/delivery",
            "value": 304.9,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p50 latency",
            "value": 440,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p99 latency",
            "value": 1082,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep ns/delivery",
            "value": 423.7,
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
            "value": 255.3,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p50 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p99 latency",
            "value": 291,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep ns/delivery",
            "value": 254.9,
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
            "value": 253.8,
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
            "value": 258.2,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p50 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p99 latency",
            "value": 320,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep ns/delivery",
            "value": 254.4,
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
            "value": 328.9,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p50 latency",
            "value": 371,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p99 latency",
            "value": 661,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep ns/delivery",
            "value": 363.3,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p50 latency",
            "value": 401,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p99 latency",
            "value": 471,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep ns/delivery",
            "value": 393.5,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p50 latency",
            "value": 421,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p99 latency",
            "value": 602,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep ns/delivery",
            "value": 422.9,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p50 latency",
            "value": 441,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p99 latency",
            "value": 691,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 459.4,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p50 latency",
            "value": 381,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p99 latency",
            "value": 1193,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep ns/delivery",
            "value": 84.4,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p50 latency",
            "value": 260,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p99 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep ns/delivery",
            "value": 253.5,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p50 latency",
            "value": 260,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p99 latency",
            "value": 441,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep ns/delivery",
            "value": 119,
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
            "value": 96.8,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p50 latency",
            "value": 310,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p99 latency",
            "value": 321,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep ns/delivery",
            "value": 299.4,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p50 latency",
            "value": 281,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p99 latency",
            "value": 300,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep ns/delivery",
            "value": 263.7,
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
            "value": 322.7,
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
            "value": 66.8,
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
            "value": 32.4,
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
            "value": 25.3,
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
            "value": 36.9,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep p50 latency",
            "value": 50,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep p99 latency",
            "value": 70,
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
            "value": 71,
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
            "value": 30108,
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
          "id": "1c0d0e8d530902c6d96bf057b352d6e3915a8910",
          "message": "Merge pull request #351 from avatarsd-llc/test/conformance-opt-bit-vectors\n\ntest(conformance): opt-bit position coverage + negative-vector harness contract",
          "timestamp": "2026-07-09T01:35:55+03:00",
          "tree_id": "5f7d85ba8c58d4d449e0ebab731f0ff2d812cf3a",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/1c0d0e8d530902c6d96bf057b352d6e3915a8910"
        },
        "date": 1783550204937,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 300,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 311,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 287.9,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p50 latency",
            "value": 441,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p99 latency",
            "value": 511,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep ns/delivery",
            "value": 54.4,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p50 latency",
            "value": 3096,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p99 latency",
            "value": 5600,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep ns/delivery",
            "value": 24.4,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p50 latency",
            "value": 25437,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p99 latency",
            "value": 39213,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep ns/delivery",
            "value": 25.2,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p50 latency",
            "value": 202320,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p99 latency",
            "value": 222188,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep ns/delivery",
            "value": 25.1,
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
            "value": 289.5,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p50 latency",
            "value": 300,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p99 latency",
            "value": 341,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep ns/delivery",
            "value": 286.1,
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
            "value": 440,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p99 latency",
            "value": 541,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep ns/delivery",
            "value": 418,
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
            "value": 254.5,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p50 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p99 latency",
            "value": 290,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep ns/delivery",
            "value": 255.1,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 291,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 254.7,
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
            "value": 255.3,
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
            "value": 255.4,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p50 latency",
            "value": 341,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p99 latency",
            "value": 371,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep ns/delivery",
            "value": 327.6,
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
            "value": 365,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p50 latency",
            "value": 401,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p99 latency",
            "value": 481,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep ns/delivery",
            "value": 395.8,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p50 latency",
            "value": 421,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p99 latency",
            "value": 732,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep ns/delivery",
            "value": 423.4,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p50 latency",
            "value": 441,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p99 latency",
            "value": 581,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 447,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p50 latency",
            "value": 381,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p99 latency",
            "value": 1122,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep ns/delivery",
            "value": 83.8,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p50 latency",
            "value": 260,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p99 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep ns/delivery",
            "value": 236.5,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p50 latency",
            "value": 290,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p99 latency",
            "value": 341,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep ns/delivery",
            "value": 118.5,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p50 latency",
            "value": 421,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p99 latency",
            "value": 521,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep ns/delivery",
            "value": 96.8,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p50 latency",
            "value": 310,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p99 latency",
            "value": 321,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep ns/delivery",
            "value": 299.1,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p50 latency",
            "value": 281,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p99 latency",
            "value": 301,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep ns/delivery",
            "value": 264.3,
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
            "value": 321.9,
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
            "value": 66.2,
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
            "value": 70,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep ns/delivery",
            "value": 36.4,
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
            "value": 90,
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
            "value": 32116,
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
          "id": "17e2250a685bd5ce0814128fe45de88c188b8191",
          "message": "Merge pull request #352 from avatarsd-llc/fix/fan1-delivery-regression\n\nfix(graph): eliminate the fan-out snapshot zeroing stall — fan1 dispatch 143→103 ns",
          "timestamp": "2026-07-09T01:44:02+03:00",
          "tree_id": "8df984784af06b23abcfef1463380bdbb6795f97",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/17e2250a685bd5ce0814128fe45de88c188b8191"
        },
        "date": 1783550687283,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 190,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 195,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 178,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p50 latency",
            "value": 351,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p99 latency",
            "value": 361,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep ns/delivery",
            "value": 43,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p50 latency",
            "value": 3136,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p99 latency",
            "value": 3286,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep ns/delivery",
            "value": 32.6,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p50 latency",
            "value": 26630,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p99 latency",
            "value": 36418,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep ns/delivery",
            "value": 26.1,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p50 latency",
            "value": 211735,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p99 latency",
            "value": 239367,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep ns/delivery",
            "value": 26.3,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p50 latency",
            "value": 181,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p99 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep ns/delivery",
            "value": 178.6,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p50 latency",
            "value": 190,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p99 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep ns/delivery",
            "value": 175.7,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p50 latency",
            "value": 200,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p99 latency",
            "value": 211,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep ns/delivery",
            "value": 190.8,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p50 latency",
            "value": 330,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p99 latency",
            "value": 552,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep ns/delivery",
            "value": 305.7,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p99 latency",
            "value": 310,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep ns/delivery",
            "value": 141.8,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p99 latency",
            "value": 171,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep ns/delivery",
            "value": 138.5,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 171,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 138.8,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p99 latency",
            "value": 171,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep ns/delivery",
            "value": 138.5,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p99 latency",
            "value": 171,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep ns/delivery",
            "value": 138.2,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p50 latency",
            "value": 221,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p99 latency",
            "value": 231,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep ns/delivery",
            "value": 209.4,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p50 latency",
            "value": 261,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p99 latency",
            "value": 301,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep ns/delivery",
            "value": 248.2,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p50 latency",
            "value": 290,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p99 latency",
            "value": 360,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep ns/delivery",
            "value": 276.3,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p50 latency",
            "value": 311,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p99 latency",
            "value": 440,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep ns/delivery",
            "value": 303.2,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p50 latency",
            "value": 331,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p99 latency",
            "value": 501,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 330.7,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p50 latency",
            "value": 280,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p99 latency",
            "value": 1062,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep ns/delivery",
            "value": 66.9,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p99 latency",
            "value": 161,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep ns/delivery",
            "value": 118.3,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p99 latency",
            "value": 161,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep ns/delivery",
            "value": 59.3,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p50 latency",
            "value": 290,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p99 latency",
            "value": 361,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep ns/delivery",
            "value": 55.2,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p50 latency",
            "value": 190,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p99 latency",
            "value": 201,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep ns/delivery",
            "value": 181.2,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p50 latency",
            "value": 170,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p99 latency",
            "value": 181,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep ns/delivery",
            "value": 141.4,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p50 latency",
            "value": 221,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p99 latency",
            "value": 260,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep ns/delivery",
            "value": 207.2,
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
            "value": 69.9,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p99 latency",
            "value": 161,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep ns/delivery",
            "value": 32.3,
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
            "value": 50,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p99 latency",
            "value": 51,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep ns/delivery",
            "value": 27.7,
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
            "value": 30.9,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p50 latency",
            "value": 60,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p99 latency",
            "value": 71,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep ns/delivery",
            "value": 39.1,
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
            "value": 32084,
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
          "id": "0d032f9b5e1c58785b2976ad8ab644a845c75e5e",
          "message": "Merge pull request #354 from avatarsd-llc/perf/write-path-reductions\n\nperf(graph): deliver the published LKV pointer — fan1 dispatch ~101→~90 ns",
          "timestamp": "2026-07-09T02:13:55+03:00",
          "tree_id": "002b72020ff8fe2061b936da8db1080e03173092",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/0d032f9b5e1c58785b2976ad8ab644a845c75e5e"
        },
        "date": 1783552475994,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 205,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 149.4,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p50 latency",
            "value": 301,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p99 latency",
            "value": 341,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep ns/delivery",
            "value": 36,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p50 latency",
            "value": 2955,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p99 latency",
            "value": 5117,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep ns/delivery",
            "value": 23.5,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p50 latency",
            "value": 24577,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p99 latency",
            "value": 33630,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep ns/delivery",
            "value": 24.3,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p50 latency",
            "value": 197707,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p99 latency",
            "value": 220851,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep ns/delivery",
            "value": 24.5,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p99 latency",
            "value": 201,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep ns/delivery",
            "value": 148.2,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p99 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep ns/delivery",
            "value": 149.3,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p50 latency",
            "value": 171,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p99 latency",
            "value": 211,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep ns/delivery",
            "value": 159.9,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p50 latency",
            "value": 281,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p99 latency",
            "value": 390,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep ns/delivery",
            "value": 267.3,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p50 latency",
            "value": 140,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p99 latency",
            "value": 181,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep ns/delivery",
            "value": 124.8,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p50 latency",
            "value": 140,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p99 latency",
            "value": 181,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep ns/delivery",
            "value": 123.9,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 140,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 181,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 124.3,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p50 latency",
            "value": 140,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p99 latency",
            "value": 220,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep ns/delivery",
            "value": 123.6,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p50 latency",
            "value": 140,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p99 latency",
            "value": 181,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep ns/delivery",
            "value": 124.1,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p50 latency",
            "value": 200,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p99 latency",
            "value": 240,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep ns/delivery",
            "value": 181.2,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p50 latency",
            "value": 210,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p99 latency",
            "value": 251,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep ns/delivery",
            "value": 194.9,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p50 latency",
            "value": 231,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p99 latency",
            "value": 301,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep ns/delivery",
            "value": 231,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p50 latency",
            "value": 280,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p99 latency",
            "value": 481,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep ns/delivery",
            "value": 264.2,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p50 latency",
            "value": 320,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p99 latency",
            "value": 521,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 319.1,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p50 latency",
            "value": 241,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p99 latency",
            "value": 911,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep ns/delivery",
            "value": 56.3,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p50 latency",
            "value": 130,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p99 latency",
            "value": 161,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep ns/delivery",
            "value": 103.5,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p50 latency",
            "value": 130,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p99 latency",
            "value": 171,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep ns/delivery",
            "value": 52,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p50 latency",
            "value": 221,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p99 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep ns/delivery",
            "value": 48.6,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p50 latency",
            "value": 170,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p99 latency",
            "value": 201,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep ns/delivery",
            "value": 153.5,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p50 latency",
            "value": 141,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p99 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep ns/delivery",
            "value": 125.5,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p50 latency",
            "value": 200,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p99 latency",
            "value": 270,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep ns/delivery",
            "value": 185.5,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p50 latency",
            "value": 110,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p99 latency",
            "value": 120,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep ns/delivery",
            "value": 83,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p50 latency",
            "value": 140,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p99 latency",
            "value": 151,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep ns/delivery",
            "value": 31.9,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep p50 latency",
            "value": 40,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep p99 latency",
            "value": 51,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep ns/delivery",
            "value": 23,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p50 latency",
            "value": 50,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p99 latency",
            "value": 70,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep ns/delivery",
            "value": 24.8,
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
            "value": 28.2,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p50 latency",
            "value": 70,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p99 latency",
            "value": 90,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep ns/delivery",
            "value": 36.5,
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
            "value": 29980,
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
          "id": "36a9c7d8369f49dfc8b5cb1891a92aed136ec1fc",
          "message": "Merge pull request #353 from avatarsd-llc/bench/comparison-fairness\n\nbench: honest zenoh comparison — deliver-only row, symmetric bracketing, corrected narrative",
          "timestamp": "2026-07-09T02:44:16+03:00",
          "tree_id": "b38d8f566185f022d69f62e15aec3b81036e5132",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/36a9c7d8369f49dfc8b5cb1891a92aed136ec1fc"
        },
        "date": 1783554301796,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 241,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 156.5,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p50 latency",
            "value": 330,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p99 latency",
            "value": 581,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep ns/delivery",
            "value": 45.3,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p50 latency",
            "value": 2885,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p99 latency",
            "value": 3245,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep ns/delivery",
            "value": 24,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p50 latency",
            "value": 23944,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p99 latency",
            "value": 32468,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep ns/delivery",
            "value": 23.6,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p50 latency",
            "value": 191239,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p99 latency",
            "value": 215844,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep ns/delivery",
            "value": 23.9,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p99 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep ns/delivery",
            "value": 152.2,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p99 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep ns/delivery",
            "value": 152.3,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p50 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p99 latency",
            "value": 210,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep ns/delivery",
            "value": 162.1,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p50 latency",
            "value": 281,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p99 latency",
            "value": 400,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep ns/delivery",
            "value": 266.2,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p50 latency",
            "value": 140,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p99 latency",
            "value": 171,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep ns/delivery",
            "value": 129,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p50 latency",
            "value": 140,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p99 latency",
            "value": 171,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep ns/delivery",
            "value": 129.9,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 140,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 129.7,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p50 latency",
            "value": 140,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p99 latency",
            "value": 170,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep ns/delivery",
            "value": 129.4,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p50 latency",
            "value": 140,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p99 latency",
            "value": 171,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep ns/delivery",
            "value": 130.2,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p50 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p99 latency",
            "value": 231,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep ns/delivery",
            "value": 184.9,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p50 latency",
            "value": 210,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p99 latency",
            "value": 320,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep ns/delivery",
            "value": 201.2,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p50 latency",
            "value": 230,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p99 latency",
            "value": 301,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep ns/delivery",
            "value": 225.8,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p50 latency",
            "value": 270,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p99 latency",
            "value": 391,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep ns/delivery",
            "value": 264.7,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p50 latency",
            "value": 320,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p99 latency",
            "value": 570,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 325.6,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p50 latency",
            "value": 250,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p99 latency",
            "value": 891,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep ns/delivery",
            "value": 57.2,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p50 latency",
            "value": 130,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p99 latency",
            "value": 170,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep ns/delivery",
            "value": 103.5,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p99 latency",
            "value": 241,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep ns/delivery",
            "value": 59.5,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p50 latency",
            "value": 231,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p99 latency",
            "value": 290,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep ns/delivery",
            "value": 48.8,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p50 latency",
            "value": 170,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p99 latency",
            "value": 211,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep ns/delivery",
            "value": 155.6,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p50 latency",
            "value": 141,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p99 latency",
            "value": 171,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep ns/delivery",
            "value": 131.4,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p50 latency",
            "value": 220,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p99 latency",
            "value": 371,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep ns/delivery",
            "value": 191.5,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p50 latency",
            "value": 100,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p99 latency",
            "value": 101,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep ns/delivery",
            "value": 67.7,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p99 latency",
            "value": 161,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep ns/delivery",
            "value": 32.1,
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
            "value": 22.3,
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
            "value": 23.4,
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
            "value": 32.4,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p50 latency",
            "value": 70,
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
            "name": "inproc-deliver 64B/fan1/1ep p50 latency",
            "value": 90,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep p99 latency",
            "value": 101,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep ns/delivery",
            "value": 72.5,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p50 latency",
            "value": 261,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p99 latency",
            "value": 291,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep ns/delivery",
            "value": 29.8,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p50 latency",
            "value": 2865,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p99 latency",
            "value": 3976,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep ns/delivery",
            "value": 22.5,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p50 latency",
            "value": 24507,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p99 latency",
            "value": 34000,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep ns/delivery",
            "value": 24.1,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p50 latency",
            "value": 195570,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p99 latency",
            "value": 223331,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep ns/delivery",
            "value": 24.2,
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
            "value": 29932,
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
          "id": "ee71f0bc6e544ac64a13b9925109c35bfbd3b4ab",
          "message": "Merge pull request #357 from avatarsd-llc/docs/doxygen-sweep-core\n\ndocs(core): doxygen /** @brief */ sweep — src, tests, bench",
          "timestamp": "2026-07-09T03:07:17+03:00",
          "tree_id": "b6f12e980d6e5756044ec44c3fd821a768115190",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/ee71f0bc6e544ac64a13b9925109c35bfbd3b4ab"
        },
        "date": 1783555686644,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 196,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 166.6,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p50 latency",
            "value": 341,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p99 latency",
            "value": 401,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep ns/delivery",
            "value": 41.1,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p50 latency",
            "value": 3146,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p99 latency",
            "value": 3266,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep ns/delivery",
            "value": 24.9,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p50 latency",
            "value": 26699,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p99 latency",
            "value": 37831,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep ns/delivery",
            "value": 26.4,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p50 latency",
            "value": 214079,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p99 latency",
            "value": 236241,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep ns/delivery",
            "value": 26.5,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p50 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p99 latency",
            "value": 181,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep ns/delivery",
            "value": 164.7,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p50 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p99 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep ns/delivery",
            "value": 167.7,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p50 latency",
            "value": 190,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p99 latency",
            "value": 211,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep ns/delivery",
            "value": 184.6,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p50 latency",
            "value": 310,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p99 latency",
            "value": 381,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep ns/delivery",
            "value": 289.9,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p99 latency",
            "value": 151,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep ns/delivery",
            "value": 128.4,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p99 latency",
            "value": 270,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep ns/delivery",
            "value": 128,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 151,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 128,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p99 latency",
            "value": 200,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep ns/delivery",
            "value": 128.2,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p99 latency",
            "value": 151,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep ns/delivery",
            "value": 128.2,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p50 latency",
            "value": 210,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p99 latency",
            "value": 281,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep ns/delivery",
            "value": 197,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p50 latency",
            "value": 251,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p99 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep ns/delivery",
            "value": 242.2,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p50 latency",
            "value": 280,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p99 latency",
            "value": 340,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep ns/delivery",
            "value": 272.1,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p50 latency",
            "value": 311,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p99 latency",
            "value": 601,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep ns/delivery",
            "value": 301.9,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p50 latency",
            "value": 330,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p99 latency",
            "value": 561,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 331.4,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p50 latency",
            "value": 280,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p99 latency",
            "value": 992,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep ns/delivery",
            "value": 64.8,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p50 latency",
            "value": 130,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p99 latency",
            "value": 170,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep ns/delivery",
            "value": 105.1,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p50 latency",
            "value": 161,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p99 latency",
            "value": 251,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep ns/delivery",
            "value": 52.8,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p50 latency",
            "value": 240,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p99 latency",
            "value": 321,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep ns/delivery",
            "value": 50.4,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p50 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p99 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep ns/delivery",
            "value": 167.6,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p99 latency",
            "value": 161,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep ns/delivery",
            "value": 130.3,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p50 latency",
            "value": 211,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p99 latency",
            "value": 251,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep ns/delivery",
            "value": 201,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p50 latency",
            "value": 90,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p99 latency",
            "value": 141,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep ns/delivery",
            "value": 69.4,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p99 latency",
            "value": 161,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep ns/delivery",
            "value": 31.5,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep p50 latency",
            "value": 50,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep p99 latency",
            "value": 60,
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
            "value": 51,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep ns/delivery",
            "value": 38.6,
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
            "value": 31.1,
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
            "value": 40.4,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep p50 latency",
            "value": 100,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep p99 latency",
            "value": 101,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep ns/delivery",
            "value": 74.2,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p50 latency",
            "value": 250,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p99 latency",
            "value": 461,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep ns/delivery",
            "value": 27.9,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p50 latency",
            "value": 2896,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p99 latency",
            "value": 3066,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep ns/delivery",
            "value": 22.9,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p50 latency",
            "value": 24827,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p99 latency",
            "value": 34304,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep ns/delivery",
            "value": 24.6,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p50 latency",
            "value": 201858,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p99 latency",
            "value": 224239,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep ns/delivery",
            "value": 25.1,
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
            "value": 30076,
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
          "id": "94fc98d85dd5ddfc17262c59b6546c72e1fb4f97",
          "message": "Merge pull request #360 from avatarsd-llc/release/0.4.0-changelog-cut\n\nrelease: v0.4.0 changelog cut + version reconcile",
          "timestamp": "2026-07-09T09:39:43+03:00",
          "tree_id": "c7ebe15478d6901fe16c845711726e5d6cecff9c",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/94fc98d85dd5ddfc17262c59b6546c72e1fb4f97"
        },
        "date": 1783579240653,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 170,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 228,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 156.6,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p50 latency",
            "value": 317,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p99 latency",
            "value": 418,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep ns/delivery",
            "value": 37.8,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p50 latency",
            "value": 3251,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p99 latency",
            "value": 3485,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep ns/delivery",
            "value": 25.4,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p50 latency",
            "value": 29496,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p99 latency",
            "value": 35652,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep ns/delivery",
            "value": 29.2,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p50 latency",
            "value": 249117,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p99 latency",
            "value": 272843,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep ns/delivery",
            "value": 30.8,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p50 latency",
            "value": 169,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p99 latency",
            "value": 204,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep ns/delivery",
            "value": 153.6,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p50 latency",
            "value": 168,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p99 latency",
            "value": 202,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep ns/delivery",
            "value": 153.4,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p50 latency",
            "value": 179,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p99 latency",
            "value": 205,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep ns/delivery",
            "value": 165,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p50 latency",
            "value": 243,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p99 latency",
            "value": 287,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep ns/delivery",
            "value": 229,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p50 latency",
            "value": 142,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p99 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep ns/delivery",
            "value": 128.1,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p50 latency",
            "value": 142,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p99 latency",
            "value": 176,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep ns/delivery",
            "value": 127.8,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 142,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 176,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 128,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p50 latency",
            "value": 142,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p99 latency",
            "value": 175,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep ns/delivery",
            "value": 127.6,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p50 latency",
            "value": 142,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p99 latency",
            "value": 176,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep ns/delivery",
            "value": 128.7,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p50 latency",
            "value": 209,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p99 latency",
            "value": 235,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep ns/delivery",
            "value": 194.6,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p50 latency",
            "value": 247,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p99 latency",
            "value": 279,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep ns/delivery",
            "value": 244.5,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p50 latency",
            "value": 273,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p99 latency",
            "value": 341,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep ns/delivery",
            "value": 261,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p50 latency",
            "value": 288,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p99 latency",
            "value": 434,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep ns/delivery",
            "value": 278.8,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p50 latency",
            "value": 314,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p99 latency",
            "value": 904,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 357.3,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p50 latency",
            "value": 253,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p99 latency",
            "value": 856,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep ns/delivery",
            "value": 56.3,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p50 latency",
            "value": 156,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p99 latency",
            "value": 188,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep ns/delivery",
            "value": 141.5,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p50 latency",
            "value": 225,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p99 latency",
            "value": 566,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep ns/delivery",
            "value": 73,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p50 latency",
            "value": 302,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p99 latency",
            "value": 505,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep ns/delivery",
            "value": 70,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p50 latency",
            "value": 202,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p99 latency",
            "value": 267,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep ns/delivery",
            "value": 181.7,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p50 latency",
            "value": 178,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p99 latency",
            "value": 217,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep ns/delivery",
            "value": 157.2,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p50 latency",
            "value": 248,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p99 latency",
            "value": 302,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep ns/delivery",
            "value": 230.8,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p50 latency",
            "value": 119,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p99 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep ns/delivery",
            "value": 98.5,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p50 latency",
            "value": 204,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p99 latency",
            "value": 357,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep ns/delivery",
            "value": 48.3,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep p50 latency",
            "value": 46,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep p99 latency",
            "value": 62,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep ns/delivery",
            "value": 27.3,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p50 latency",
            "value": 48,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p99 latency",
            "value": 63,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep ns/delivery",
            "value": 30.2,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep p50 latency",
            "value": 51,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep p99 latency",
            "value": 71,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep ns/delivery",
            "value": 33.9,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p50 latency",
            "value": 58,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p99 latency",
            "value": 83,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep ns/delivery",
            "value": 42.9,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep p50 latency",
            "value": 118,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep p99 latency",
            "value": 161,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep ns/delivery",
            "value": 102,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p50 latency",
            "value": 263,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p99 latency",
            "value": 304,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep ns/delivery",
            "value": 31.2,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p50 latency",
            "value": 3119,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p99 latency",
            "value": 3860,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep ns/delivery",
            "value": 24.3,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p50 latency",
            "value": 27937,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p99 latency",
            "value": 33809,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep ns/delivery",
            "value": 27.5,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p50 latency",
            "value": 239573,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p99 latency",
            "value": 260247,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep ns/delivery",
            "value": 29.6,
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
            "value": 30144,
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
          "id": "94fc98d85dd5ddfc17262c59b6546c72e1fb4f97",
          "message": "Merge pull request #360 from avatarsd-llc/release/0.4.0-changelog-cut\n\nrelease: v0.4.0 changelog cut + version reconcile",
          "timestamp": "2026-07-09T09:39:43+03:00",
          "tree_id": "c7ebe15478d6901fe16c845711726e5d6cecff9c",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/94fc98d85dd5ddfc17262c59b6546c72e1fb4f97"
        },
        "date": 1783579790120,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 175,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 164.1,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p50 latency",
            "value": 320,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p99 latency",
            "value": 371,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep ns/delivery",
            "value": 38.7,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p50 latency",
            "value": 2946,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p99 latency",
            "value": 3096,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep ns/delivery",
            "value": 23.6,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p50 latency",
            "value": 24916,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p99 latency",
            "value": 38541,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep ns/delivery",
            "value": 24.7,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p50 latency",
            "value": 201517,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p99 latency",
            "value": 223538,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep ns/delivery",
            "value": 25,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p50 latency",
            "value": 171,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p99 latency",
            "value": 181,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep ns/delivery",
            "value": 161.5,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p50 latency",
            "value": 170,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p99 latency",
            "value": 181,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep ns/delivery",
            "value": 161.5,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p50 latency",
            "value": 190,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p99 latency",
            "value": 201,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep ns/delivery",
            "value": 179.8,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p50 latency",
            "value": 310,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p99 latency",
            "value": 391,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep ns/delivery",
            "value": 289.6,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p99 latency",
            "value": 240,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep ns/delivery",
            "value": 128.3,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p99 latency",
            "value": 151,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep ns/delivery",
            "value": 129,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 151,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 129.4,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p99 latency",
            "value": 151,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep ns/delivery",
            "value": 128.6,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p99 latency",
            "value": 151,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep ns/delivery",
            "value": 128.4,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p50 latency",
            "value": 210,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p99 latency",
            "value": 231,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep ns/delivery",
            "value": 197.2,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p50 latency",
            "value": 250,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p99 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep ns/delivery",
            "value": 240.3,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p50 latency",
            "value": 280,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p99 latency",
            "value": 341,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep ns/delivery",
            "value": 270,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p50 latency",
            "value": 301,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p99 latency",
            "value": 421,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep ns/delivery",
            "value": 301.5,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p50 latency",
            "value": 331,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p99 latency",
            "value": 531,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 330.6,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p50 latency",
            "value": 280,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p99 latency",
            "value": 1102,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep ns/delivery",
            "value": 67.7,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p50 latency",
            "value": 130,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p99 latency",
            "value": 141,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep ns/delivery",
            "value": 105.1,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p50 latency",
            "value": 130,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p99 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep ns/delivery",
            "value": 53.4,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p50 latency",
            "value": 240,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p99 latency",
            "value": 320,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep ns/delivery",
            "value": 53.8,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p50 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p99 latency",
            "value": 181,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep ns/delivery",
            "value": 165.3,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p99 latency",
            "value": 161,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep ns/delivery",
            "value": 130.5,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p50 latency",
            "value": 210,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p99 latency",
            "value": 241,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep ns/delivery",
            "value": 197.8,
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
            "value": 67.2,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p99 latency",
            "value": 161,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep ns/delivery",
            "value": 31.5,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep p50 latency",
            "value": 41,
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
            "value": 70,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep ns/delivery",
            "value": 32.7,
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
            "value": 31,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p50 latency",
            "value": 60,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p99 latency",
            "value": 71,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep ns/delivery",
            "value": 40.3,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep p50 latency",
            "value": 100,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep p99 latency",
            "value": 101,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep ns/delivery",
            "value": 74.1,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p50 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p99 latency",
            "value": 281,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep ns/delivery",
            "value": 30.5,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p50 latency",
            "value": 3086,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p99 latency",
            "value": 4949,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep ns/delivery",
            "value": 24.3,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p50 latency",
            "value": 26449,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p99 latency",
            "value": 35987,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep ns/delivery",
            "value": 26.1,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p50 latency",
            "value": 213529,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p99 latency",
            "value": 243054,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep ns/delivery",
            "value": 26.5,
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
            "value": 29908,
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
          "id": "7fc944a4a9e22cea3edaa0930961273308fa1be3",
          "message": "Merge pull request #363 from avatarsd-llc/feat/vertex-diet-hot-cold-split\n\nperf(graph): vertex_t hot/cold split — lazy vertex_ext_t, 536→248 B (#361 §1)",
          "timestamp": "2026-07-09T17:39:00+03:00",
          "tree_id": "29e6839269233dabca3175c8f8e34569fab10540",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/7fc944a4a9e22cea3edaa0930961273308fa1be3"
        },
        "date": 1783607982666,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 100,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 120,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 89,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p50 latency",
            "value": 211,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p99 latency",
            "value": 231,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep ns/delivery",
            "value": 25.2,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p50 latency",
            "value": 1943,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p99 latency",
            "value": 1993,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep ns/delivery",
            "value": 15.3,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p50 latency",
            "value": 15114,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p99 latency",
            "value": 20843,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep ns/delivery",
            "value": 14.9,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p50 latency",
            "value": 123086,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p99 latency",
            "value": 147272,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep ns/delivery",
            "value": 15.2,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p50 latency",
            "value": 100,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p99 latency",
            "value": 121,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep ns/delivery",
            "value": 86.9,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p50 latency",
            "value": 100,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p99 latency",
            "value": 111,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep ns/delivery",
            "value": 88.6,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p50 latency",
            "value": 110,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p99 latency",
            "value": 130,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep ns/delivery",
            "value": 97.9,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p50 latency",
            "value": 170,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p99 latency",
            "value": 220,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep ns/delivery",
            "value": 154.5,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p50 latency",
            "value": 91,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p99 latency",
            "value": 101,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep ns/delivery",
            "value": 75.2,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p50 latency",
            "value": 91,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p99 latency",
            "value": 101,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep ns/delivery",
            "value": 75.3,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 91,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 101,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 75.7,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p50 latency",
            "value": 91,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p99 latency",
            "value": 101,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep ns/delivery",
            "value": 75.1,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p50 latency",
            "value": 91,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p99 latency",
            "value": 101,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep ns/delivery",
            "value": 74.5,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p50 latency",
            "value": 120,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p99 latency",
            "value": 131,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep ns/delivery",
            "value": 108.3,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p50 latency",
            "value": 140,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p99 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep ns/delivery",
            "value": 125,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p99 latency",
            "value": 201,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep ns/delivery",
            "value": 143.8,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p99 latency",
            "value": 240,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep ns/delivery",
            "value": 148.9,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p50 latency",
            "value": 170,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p99 latency",
            "value": 241,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 158.8,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p99 latency",
            "value": 511,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep ns/delivery",
            "value": 33.6,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p50 latency",
            "value": 120,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p99 latency",
            "value": 130,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep ns/delivery",
            "value": 94.2,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p50 latency",
            "value": 120,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p99 latency",
            "value": 130,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep ns/delivery",
            "value": 46.2,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p99 latency",
            "value": 211,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep ns/delivery",
            "value": 34,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p50 latency",
            "value": 141,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p99 latency",
            "value": 161,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep ns/delivery",
            "value": 131.6,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p50 latency",
            "value": 131,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p99 latency",
            "value": 141,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep ns/delivery",
            "value": 117.2,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p50 latency",
            "value": 170,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p99 latency",
            "value": 181,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep ns/delivery",
            "value": 148.6,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p50 latency",
            "value": 91,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p99 latency",
            "value": 101,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep ns/delivery",
            "value": 75.5,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p50 latency",
            "value": 111,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p99 latency",
            "value": 140,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep ns/delivery",
            "value": 24.9,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep p50 latency",
            "value": 30,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep p99 latency",
            "value": 31,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep ns/delivery",
            "value": 11.9,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p50 latency",
            "value": 30,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p99 latency",
            "value": 31,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep ns/delivery",
            "value": 13.3,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep p50 latency",
            "value": 30,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep p99 latency",
            "value": 40,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep ns/delivery",
            "value": 15.3,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p50 latency",
            "value": 40,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p99 latency",
            "value": 41,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep ns/delivery",
            "value": 18.9,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep p50 latency",
            "value": 90,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep p99 latency",
            "value": 91,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep ns/delivery",
            "value": 69,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p50 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p99 latency",
            "value": 201,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep ns/delivery",
            "value": 22,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p50 latency",
            "value": 1963,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p99 latency",
            "value": 2033,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep ns/delivery",
            "value": 15.3,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p50 latency",
            "value": 14992,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p99 latency",
            "value": 20150,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep ns/delivery",
            "value": 14.7,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p50 latency",
            "value": 121052,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p99 latency",
            "value": 131178,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep ns/delivery",
            "value": 14.9,
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
            "value": 26880,
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
          "id": "afefd06d1d1ec729ddbe4bd5a4a068266324104f",
          "message": "Merge pull request #364 from avatarsd-llc/feat/ws-multi-peer\n\nfeat(transport_ws): multi-peer server + opt-in ADR-0044 bus facet (#362)",
          "timestamp": "2026-07-09T17:44:32+03:00",
          "tree_id": "196e774756afa864f1898c57373fc709f428818e",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/afefd06d1d1ec729ddbe4bd5a4a068266324104f"
        },
        "date": 1783608320025,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 130,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 170,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 122.1,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p50 latency",
            "value": 270,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p99 latency",
            "value": 311,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep ns/delivery",
            "value": 32.5,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p50 latency",
            "value": 4376,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p99 latency",
            "value": 6150,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep ns/delivery",
            "value": 22.1,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p50 latency",
            "value": 22944,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p99 latency",
            "value": 29654,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep ns/delivery",
            "value": 22.8,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p50 latency",
            "value": 183762,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p99 latency",
            "value": 197282,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep ns/delivery",
            "value": 22.7,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p50 latency",
            "value": 130,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p99 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep ns/delivery",
            "value": 121,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p50 latency",
            "value": 130,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p99 latency",
            "value": 171,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep ns/delivery",
            "value": 120.5,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p50 latency",
            "value": 141,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p99 latency",
            "value": 190,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep ns/delivery",
            "value": 144.1,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p50 latency",
            "value": 221,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p99 latency",
            "value": 281,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep ns/delivery",
            "value": 214.1,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p50 latency",
            "value": 110,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p99 latency",
            "value": 140,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep ns/delivery",
            "value": 98.8,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p50 latency",
            "value": 110,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p99 latency",
            "value": 190,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep ns/delivery",
            "value": 101.8,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 110,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 141,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 98.6,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p50 latency",
            "value": 110,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p99 latency",
            "value": 141,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep ns/delivery",
            "value": 98.3,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p50 latency",
            "value": 110,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p99 latency",
            "value": 141,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep ns/delivery",
            "value": 98.3,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p99 latency",
            "value": 181,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep ns/delivery",
            "value": 143,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p99 latency",
            "value": 210,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep ns/delivery",
            "value": 153.7,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p50 latency",
            "value": 170,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p99 latency",
            "value": 230,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep ns/delivery",
            "value": 169.8,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p50 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p99 latency",
            "value": 260,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep ns/delivery",
            "value": 186.8,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p50 latency",
            "value": 220,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p99 latency",
            "value": 371,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 222.1,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p50 latency",
            "value": 201,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p99 latency",
            "value": 741,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep ns/delivery",
            "value": 46.6,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p50 latency",
            "value": 100,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p99 latency",
            "value": 120,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep ns/delivery",
            "value": 79.6,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p50 latency",
            "value": 100,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p99 latency",
            "value": 120,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep ns/delivery",
            "value": 39.7,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p50 latency",
            "value": 200,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p99 latency",
            "value": 291,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep ns/delivery",
            "value": 37.8,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p50 latency",
            "value": 130,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p99 latency",
            "value": 171,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep ns/delivery",
            "value": 125.4,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p50 latency",
            "value": 110,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p99 latency",
            "value": 141,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep ns/delivery",
            "value": 100.7,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p50 latency",
            "value": 151,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p99 latency",
            "value": 201,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep ns/delivery",
            "value": 147.6,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p50 latency",
            "value": 80,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p99 latency",
            "value": 120,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep ns/delivery",
            "value": 53.5,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p50 latency",
            "value": 120,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p99 latency",
            "value": 130,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep ns/delivery",
            "value": 24.8,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep p50 latency",
            "value": 30,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep p99 latency",
            "value": 41,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep ns/delivery",
            "value": 18.4,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p50 latency",
            "value": 40,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p99 latency",
            "value": 60,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep ns/delivery",
            "value": 18.5,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep p50 latency",
            "value": 40,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep p99 latency",
            "value": 60,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep ns/delivery",
            "value": 26,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p50 latency",
            "value": 50,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p99 latency",
            "value": 60,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep ns/delivery",
            "value": 42.8,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep p50 latency",
            "value": 80,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep p99 latency",
            "value": 81,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep ns/delivery",
            "value": 59.3,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p50 latency",
            "value": 211,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p99 latency",
            "value": 221,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep ns/delivery",
            "value": 24.6,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p50 latency",
            "value": 2714,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p99 latency",
            "value": 2774,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep ns/delivery",
            "value": 21.4,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p50 latency",
            "value": 22964,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p99 latency",
            "value": 30646,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep ns/delivery",
            "value": 22.6,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p50 latency",
            "value": 184634,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p99 latency",
            "value": 202140,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep ns/delivery",
            "value": 22.8,
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
            "value": 26832,
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
          "id": "8c95ec1ee7eaae4e82fa897f92f89cd24d7afcc8",
          "message": "Merge pull request #366 from avatarsd-llc/feat/vertex-diet-striped-locks\n\nperf(graph): vertex diet §2+§3 — striped locks (248→160 B) + bearing-ancestor ACL state (#361)",
          "timestamp": "2026-07-09T18:00:30+03:00",
          "tree_id": "938a179e0ac30b3b8773c579e04b5cc721840e30",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/8c95ec1ee7eaae4e82fa897f92f89cd24d7afcc8"
        },
        "date": 1783609292510,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 185,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 211,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 173,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p50 latency",
            "value": 351,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p99 latency",
            "value": 381,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep ns/delivery",
            "value": 43.1,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p50 latency",
            "value": 3255,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p99 latency",
            "value": 5531,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep ns/delivery",
            "value": 26.3,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p50 latency",
            "value": 27231,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p99 latency",
            "value": 43521,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep ns/delivery",
            "value": 26.9,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p50 latency",
            "value": 215310,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p99 latency",
            "value": 252420,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep ns/delivery",
            "value": 26.8,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p50 latency",
            "value": 190,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p99 latency",
            "value": 200,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep ns/delivery",
            "value": 174.5,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p50 latency",
            "value": 181,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p99 latency",
            "value": 210,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep ns/delivery",
            "value": 172.5,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p50 latency",
            "value": 200,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p99 latency",
            "value": 221,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep ns/delivery",
            "value": 184.1,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p50 latency",
            "value": 330,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p99 latency",
            "value": 601,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep ns/delivery",
            "value": 303.1,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p99 latency",
            "value": 170,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep ns/delivery",
            "value": 137.3,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p99 latency",
            "value": 170,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep ns/delivery",
            "value": 138.1,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 171,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 137.7,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p99 latency",
            "value": 171,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep ns/delivery",
            "value": 137.4,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p99 latency",
            "value": 170,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep ns/delivery",
            "value": 141.9,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p50 latency",
            "value": 220,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p99 latency",
            "value": 240,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep ns/delivery",
            "value": 208,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p50 latency",
            "value": 260,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p99 latency",
            "value": 290,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep ns/delivery",
            "value": 243.6,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p50 latency",
            "value": 281,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p99 latency",
            "value": 341,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep ns/delivery",
            "value": 271.5,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p50 latency",
            "value": 301,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p99 latency",
            "value": 410,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep ns/delivery",
            "value": 291.2,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p50 latency",
            "value": 330,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p99 latency",
            "value": 421,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 321.4,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p50 latency",
            "value": 280,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p99 latency",
            "value": 1122,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep ns/delivery",
            "value": 65.1,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p99 latency",
            "value": 151,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep ns/delivery",
            "value": 122.7,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p99 latency",
            "value": 151,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep ns/delivery",
            "value": 58,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p50 latency",
            "value": 260,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p99 latency",
            "value": 341,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep ns/delivery",
            "value": 55.2,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p50 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p99 latency",
            "value": 221,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep ns/delivery",
            "value": 176.5,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p99 latency",
            "value": 171,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep ns/delivery",
            "value": 139.9,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p50 latency",
            "value": 230,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p99 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep ns/delivery",
            "value": 215.7,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p50 latency",
            "value": 100,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p99 latency",
            "value": 101,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep ns/delivery",
            "value": 74.6,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p50 latency",
            "value": 181,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p99 latency",
            "value": 3717,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep ns/delivery",
            "value": 141.9,
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
            "value": 26.1,
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
            "value": 27.7,
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
            "value": 31.3,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p50 latency",
            "value": 60,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p99 latency",
            "value": 71,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep ns/delivery",
            "value": 40.4,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep p50 latency",
            "value": 100,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep p99 latency",
            "value": 111,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep ns/delivery",
            "value": 80.6,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p50 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p99 latency",
            "value": 300,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep ns/delivery",
            "value": 31.1,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p50 latency",
            "value": 3136,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p99 latency",
            "value": 3476,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep ns/delivery",
            "value": 24.4,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p50 latency",
            "value": 26599,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p99 latency",
            "value": 36328,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep ns/delivery",
            "value": 26.2,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p50 latency",
            "value": 214850,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p99 latency",
            "value": 241941,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep ns/delivery",
            "value": 26.7,
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
            "value": 26204,
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
          "id": "3466741cd72e56020b82a27cb6a987cde2eec0cf",
          "message": "Merge pull request #367 from avatarsd-llc/feat/vertex-diet-pmr-lkv\n\nfeat(graph): ADR-0039 injected memory_resource — pmr LKV allocation seam (#361 §5)",
          "timestamp": "2026-07-09T18:13:27+03:00",
          "tree_id": "3050103f0c3d73d4bb14361e5d52468c4cad04aa",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/3466741cd72e56020b82a27cb6a987cde2eec0cf"
        },
        "date": 1783610051907,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 140,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 133.3,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p50 latency",
            "value": 261,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p99 latency",
            "value": 301,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep ns/delivery",
            "value": 31,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p50 latency",
            "value": 4256,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p99 latency",
            "value": 4587,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep ns/delivery",
            "value": 23.1,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p50 latency",
            "value": 23005,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p99 latency",
            "value": 30605,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep ns/delivery",
            "value": 22.8,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p50 latency",
            "value": 181611,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p99 latency",
            "value": 267699,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep ns/delivery",
            "value": 22.5,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p50 latency",
            "value": 140,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p99 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep ns/delivery",
            "value": 131.4,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p50 latency",
            "value": 140,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p99 latency",
            "value": 170,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep ns/delivery",
            "value": 128,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p50 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p99 latency",
            "value": 200,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep ns/delivery",
            "value": 146.5,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p50 latency",
            "value": 231,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p99 latency",
            "value": 300,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep ns/delivery",
            "value": 220.1,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p50 latency",
            "value": 120,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p99 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep ns/delivery",
            "value": 111.4,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p50 latency",
            "value": 120,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p99 latency",
            "value": 200,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep ns/delivery",
            "value": 110.4,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 120,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 151,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 111.5,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p50 latency",
            "value": 120,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p99 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep ns/delivery",
            "value": 110.6,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p50 latency",
            "value": 120,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p99 latency",
            "value": 150,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep ns/delivery",
            "value": 110.5,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p50 latency",
            "value": 161,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p99 latency",
            "value": 211,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep ns/delivery",
            "value": 156,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p50 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p99 latency",
            "value": 230,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep ns/delivery",
            "value": 171.1,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p50 latency",
            "value": 190,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p99 latency",
            "value": 250,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep ns/delivery",
            "value": 187.4,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p50 latency",
            "value": 220,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p99 latency",
            "value": 341,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep ns/delivery",
            "value": 206.8,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p50 latency",
            "value": 240,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p99 latency",
            "value": 380,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 241.9,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p50 latency",
            "value": 211,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p99 latency",
            "value": 741,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep ns/delivery",
            "value": 46.7,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p50 latency",
            "value": 110,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p99 latency",
            "value": 141,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep ns/delivery",
            "value": 90.4,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p50 latency",
            "value": 170,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p99 latency",
            "value": 260,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep ns/delivery",
            "value": 48,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p50 latency",
            "value": 241,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p99 latency",
            "value": 380,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep ns/delivery",
            "value": 48.9,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p50 latency",
            "value": 141,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p99 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep ns/delivery",
            "value": 134.4,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p50 latency",
            "value": 121,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p99 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep ns/delivery",
            "value": 119.2,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p50 latency",
            "value": 170,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p99 latency",
            "value": 210,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep ns/delivery",
            "value": 164.1,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p50 latency",
            "value": 80,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p99 latency",
            "value": 81,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep ns/delivery",
            "value": 54.8,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p50 latency",
            "value": 140,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p99 latency",
            "value": 8062,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep ns/delivery",
            "value": 132.5,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep p50 latency",
            "value": 40,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep p99 latency",
            "value": 41,
            "unit": "ns"
          },
          {
            "name": "fold-n1 512B/fan1/1ep ns/delivery",
            "value": 18.5,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p50 latency",
            "value": 40,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p99 latency",
            "value": 60,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep ns/delivery",
            "value": 20.9,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep p50 latency",
            "value": 40,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep p99 latency",
            "value": 60,
            "unit": "ns"
          },
          {
            "name": "fold-n4 512B/fan1/1ep ns/delivery",
            "value": 27.7,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p50 latency",
            "value": 60,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p99 latency",
            "value": 71,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep ns/delivery",
            "value": 40,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep p50 latency",
            "value": 70,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep p99 latency",
            "value": 81,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep ns/delivery",
            "value": 60.9,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p50 latency",
            "value": 200,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p99 latency",
            "value": 210,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep ns/delivery",
            "value": 22.3,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p50 latency",
            "value": 2694,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p99 latency",
            "value": 2785,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep ns/delivery",
            "value": 21,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p50 latency",
            "value": 22554,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p99 latency",
            "value": 29334,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep ns/delivery",
            "value": 22.2,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p50 latency",
            "value": 185848,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p99 latency",
            "value": 201748,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep ns/delivery",
            "value": 22.8,
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
            "value": 26276,
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
          "id": "905d3cd5bdb016763d8a579ea0695d7f979d9d5b",
          "message": "Merge pull request #368 from avatarsd-llc/feat/vertex-diet-heap-history\n\nfeat(bench): per-vertex steady-heap history probe (#361 §8)",
          "timestamp": "2026-07-09T18:24:58+03:00",
          "tree_id": "34e34af6686f259ec0b4f9ac4ae7ffa67b18062d",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/905d3cd5bdb016763d8a579ea0695d7f979d9d5b"
        },
        "date": 1783610752986,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 200,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 236,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 189.1,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p50 latency",
            "value": 361,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p99 latency",
            "value": 391,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep ns/delivery",
            "value": 44,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p50 latency",
            "value": 3286,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p99 latency",
            "value": 3647,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep ns/delivery",
            "value": 26,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p50 latency",
            "value": 26159,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p99 latency",
            "value": 35917,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep ns/delivery",
            "value": 26.2,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p50 latency",
            "value": 207249,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p99 latency",
            "value": 236093,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep ns/delivery",
            "value": 25.6,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p50 latency",
            "value": 200,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p99 latency",
            "value": 211,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep ns/delivery",
            "value": 190.4,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p50 latency",
            "value": 200,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p99 latency",
            "value": 220,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep ns/delivery",
            "value": 191.1,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p50 latency",
            "value": 211,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p99 latency",
            "value": 221,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep ns/delivery",
            "value": 199.7,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p50 latency",
            "value": 340,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p99 latency",
            "value": 481,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep ns/delivery",
            "value": 320.2,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p50 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p99 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep ns/delivery",
            "value": 166.6,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p50 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p99 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep ns/delivery",
            "value": 166,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 200,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 165.7,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p50 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p99 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep ns/delivery",
            "value": 166.5,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p50 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p99 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep ns/delivery",
            "value": 166.3,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p50 latency",
            "value": 231,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p99 latency",
            "value": 261,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep ns/delivery",
            "value": 224.8,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p50 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p99 latency",
            "value": 310,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep ns/delivery",
            "value": 267.4,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p50 latency",
            "value": 301,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p99 latency",
            "value": 361,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep ns/delivery",
            "value": 295.1,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p50 latency",
            "value": 321,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p99 latency",
            "value": 401,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep ns/delivery",
            "value": 313.5,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p50 latency",
            "value": 341,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p99 latency",
            "value": 441,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 344.2,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p50 latency",
            "value": 280,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p99 latency",
            "value": 1122,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep ns/delivery",
            "value": 64.7,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p99 latency",
            "value": 190,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep ns/delivery",
            "value": 130.8,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p99 latency",
            "value": 260,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep ns/delivery",
            "value": 65.5,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p50 latency",
            "value": 281,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p99 latency",
            "value": 371,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep ns/delivery",
            "value": 59.6,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p50 latency",
            "value": 201,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p99 latency",
            "value": 211,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep ns/delivery",
            "value": 190.1,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p50 latency",
            "value": 181,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p99 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep ns/delivery",
            "value": 169.4,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p50 latency",
            "value": 250,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p99 latency",
            "value": 281,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep ns/delivery",
            "value": 229.9,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p50 latency",
            "value": 100,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p99 latency",
            "value": 130,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep ns/delivery",
            "value": 69.8,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p50 latency",
            "value": 181,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p99 latency",
            "value": 3717,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep ns/delivery",
            "value": 128.8,
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
            "value": 25.7,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p50 latency",
            "value": 50,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p99 latency",
            "value": 51,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep ns/delivery",
            "value": 34.2,
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
            "value": 31.7,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p50 latency",
            "value": 60,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p99 latency",
            "value": 69,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep ns/delivery",
            "value": 40.1,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep p50 latency",
            "value": 100,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep p99 latency",
            "value": 111,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep ns/delivery",
            "value": 80.2,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p50 latency",
            "value": 270,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p99 latency",
            "value": 311,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep ns/delivery",
            "value": 30.8,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p50 latency",
            "value": 3056,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p99 latency",
            "value": 3567,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep ns/delivery",
            "value": 24.4,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p50 latency",
            "value": 26019,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p99 latency",
            "value": 36308,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep ns/delivery",
            "value": 25.8,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p50 latency",
            "value": 214242,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p99 latency",
            "value": 236264,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep ns/delivery",
            "value": 26.6,
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
            "name": "heap bytes per vertex (probe)",
            "value": 204,
            "unit": "bytes"
          },
          {
            "name": "heap allocs per vertex (probe)",
            "value": 8,
            "unit": "allocs"
          },
          {
            "name": "heap bytes per vertex_value (probe)",
            "value": 104,
            "unit": "bytes"
          },
          {
            "name": "heap allocs per vertex_value (probe)",
            "value": 6,
            "unit": "allocs"
          },
          {
            "name": "bench_libtracer max RSS",
            "value": 28348,
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
          "id": "ebb9f55f9678963949faf1ef8b71da8c5c4ab8c7",
          "message": "Merge pull request #369 from avatarsd-llc/feat/perf-ratchet\n\nfeat(perf): no-pullback ratchet + surface-mapped performance page",
          "timestamp": "2026-07-09T23:12:16+03:00",
          "tree_id": "f659c01e9a26a1138d83d4ddf3ddd42eb367a32d",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/ebb9f55f9678963949faf1ef8b71da8c5c4ab8c7"
        },
        "date": 1783628048792,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep p50 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep p99 latency",
            "value": 220,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1/1ep ns/delivery",
            "value": 170.3,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p50 latency",
            "value": 340,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep p99 latency",
            "value": 371,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8/1ep ns/delivery",
            "value": 39.8,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p50 latency",
            "value": 3615,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep p99 latency",
            "value": 3986,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan128/1ep ns/delivery",
            "value": 33.8,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p50 latency",
            "value": 29513,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep p99 latency",
            "value": 40831,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan1024/1ep ns/delivery",
            "value": 29.1,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p50 latency",
            "value": 236481,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep p99 latency",
            "value": 256100,
            "unit": "ns"
          },
          {
            "name": "inproc 64B/fan8192/1ep ns/delivery",
            "value": 29.1,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p50 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep p99 latency",
            "value": 221,
            "unit": "ns"
          },
          {
            "name": "inproc 1B/fan1/1ep ns/delivery",
            "value": 167.5,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p50 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep p99 latency",
            "value": 211,
            "unit": "ns"
          },
          {
            "name": "inproc 8B/fan1/1ep ns/delivery",
            "value": 164.9,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p50 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep p99 latency",
            "value": 230,
            "unit": "ns"
          },
          {
            "name": "inproc 1024B/fan1/1ep ns/delivery",
            "value": 182.5,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p50 latency",
            "value": 301,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep p99 latency",
            "value": 411,
            "unit": "ns"
          },
          {
            "name": "inproc 8192B/fan1/1ep ns/delivery",
            "value": 286.4,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep p99 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep ns/delivery",
            "value": 142.7,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep p99 latency",
            "value": 190,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep ns/delivery",
            "value": 141.7,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep p99 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep ns/delivery",
            "value": 142.1,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep p99 latency",
            "value": 191,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep ns/delivery",
            "value": 141.5,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep p99 latency",
            "value": 190,
            "unit": "ns"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep ns/delivery",
            "value": 141.5,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p50 latency",
            "value": 210,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep p99 latency",
            "value": 261,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1ep ns/delivery",
            "value": 200.1,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p50 latency",
            "value": 231,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep p99 latency",
            "value": 370,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8ep ns/delivery",
            "value": 220.2,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p50 latency",
            "value": 250,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep p99 latency",
            "value": 321,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/128ep ns/delivery",
            "value": 243.9,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p50 latency",
            "value": 280,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep p99 latency",
            "value": 371,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep ns/delivery",
            "value": 267.1,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p50 latency",
            "value": 331,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep p99 latency",
            "value": 551,
            "unit": "ns"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep ns/delivery",
            "value": 333.4,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p50 latency",
            "value": 260,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep p99 latency",
            "value": 1292,
            "unit": "ns"
          },
          {
            "name": "mixed 0B/fan6/128ep ns/delivery",
            "value": 65.2,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p50 latency",
            "value": 140,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep p99 latency",
            "value": 181,
            "unit": "ns"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep ns/delivery",
            "value": 116.1,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p50 latency",
            "value": 170,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep p99 latency",
            "value": 270,
            "unit": "ns"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep ns/delivery",
            "value": 58.7,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p50 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep p99 latency",
            "value": 410,
            "unit": "ns"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep ns/delivery",
            "value": 63.1,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p50 latency",
            "value": 180,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep p99 latency",
            "value": 220,
            "unit": "ns"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep ns/delivery",
            "value": 167.9,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p50 latency",
            "value": 160,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep p99 latency",
            "value": 200,
            "unit": "ns"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep ns/delivery",
            "value": 144.7,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p50 latency",
            "value": 220,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep p99 latency",
            "value": 271,
            "unit": "ns"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep ns/delivery",
            "value": 209.6,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p50 latency",
            "value": 100,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep p99 latency",
            "value": 111,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep ns/delivery",
            "value": 71,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p50 latency",
            "value": 171,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep p99 latency",
            "value": 9985,
            "unit": "ns"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep ns/delivery",
            "value": 168.7,
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
            "value": 22.7,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p50 latency",
            "value": 50,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep p99 latency",
            "value": 70,
            "unit": "ns"
          },
          {
            "name": "fold-n2 512B/fan1/1ep ns/delivery",
            "value": 23.6,
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
            "value": 32.5,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p50 latency",
            "value": 60,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep p99 latency",
            "value": 71,
            "unit": "ns"
          },
          {
            "name": "fold-n8 512B/fan1/1ep ns/delivery",
            "value": 50.4,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep p50 latency",
            "value": 100,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep p99 latency",
            "value": 111,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep ns/delivery",
            "value": 79.2,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p50 latency",
            "value": 250,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep p99 latency",
            "value": 261,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep ns/delivery",
            "value": 28.3,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p50 latency",
            "value": 3455,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep p99 latency",
            "value": 3585,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep ns/delivery",
            "value": 27,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p50 latency",
            "value": 29193,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep p99 latency",
            "value": 38096,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep ns/delivery",
            "value": 28.8,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p50 latency",
            "value": 235129,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep p99 latency",
            "value": 252535,
            "unit": "ns"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep ns/delivery",
            "value": 29,
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
            "name": "heap bytes per vertex (probe)",
            "value": 204,
            "unit": "bytes"
          },
          {
            "name": "heap allocs per vertex (probe)",
            "value": 8,
            "unit": "allocs"
          },
          {
            "name": "heap bytes per vertex_value (probe)",
            "value": 104,
            "unit": "bytes"
          },
          {
            "name": "heap allocs per vertex_value (probe)",
            "value": 6,
            "unit": "allocs"
          },
          {
            "name": "bench_libtracer max RSS",
            "value": 26188,
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
          "id": "199d2158e39d738691fd115626ce4334e6ce01f4",
          "message": "Merge pull request #343 from avatarsd-llc/feat/perf-docs-history\n\nci(perf)+docs: benchmark history surfaced in the docs site; rich per-commit results",
          "timestamp": "2026-07-08T23:44:08+03:00",
          "tree_id": "5e0eb1bd63f01c152351e14a571d94710749b2ba",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/199d2158e39d738691fd115626ce4334e6ce01f4"
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
          "id": "fd1fb2da688d6cd15439bf797a001f49c1d3fb5b",
          "message": "Merge pull request #344 from avatarsd-llc/feat/effective-acl\n\nfeat(graph): effective_acl_t + subtree-precise cached ACE merge (ADR-0050 completed)",
          "timestamp": "2026-07-09T00:13:52+03:00",
          "tree_id": "6c591495ed2a558c02f393e552652f6142b77571",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/fd1fb2da688d6cd15439bf797a001f49c1d3fb5b"
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
          "id": "97070b0fc341b4948646ae2f924c649c730cdc5b",
          "message": "Merge pull request #346 from avatarsd-llc/feat/msquic-endpoint-base\n\nrefactor(net): msquic_endpoint_t — the QUIC-mechanical layer extracted from both msquic transports",
          "timestamp": "2026-07-09T00:13:56+03:00",
          "tree_id": "b1e512fe1c24d54f9d9a6cacaf7f45ae93477653",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/97070b0fc341b4948646ae2f924c649c730cdc5b"
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
          "id": "c9481238532063d902ec50a63910dcca7c72d643",
          "message": "Merge pull request #348 from avatarsd-llc/feat/perf-page-unified-history\n\ndocs(perf): unified performance page — in-page all-commit history alongside Zenoh + live test rollup",
          "timestamp": "2026-07-09T00:22:58+03:00",
          "tree_id": "ea6f4f662cb463bbdc9f0707bd66af833bc8fae8",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/c9481238532063d902ec50a63910dcca7c72d643"
        },
        "date": 1783545831642,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep throughput",
            "value": 3460959.5,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8/1ep throughput",
            "value": 16801018,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan128/1ep throughput",
            "value": 38253799,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan1024/1ep throughput",
            "value": 36263586,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8192/1ep throughput",
            "value": 37077856,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1B/fan1/1ep throughput",
            "value": 3443306,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8B/fan1/1ep throughput",
            "value": 3490054,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1024B/fan1/1ep throughput",
            "value": 3297146,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8192B/fan1/1ep throughput",
            "value": 2373433,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep throughput",
            "value": 3946067,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep throughput",
            "value": 3943451,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep throughput",
            "value": 3948772,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep throughput",
            "value": 3943717,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep throughput",
            "value": 3951807,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1ep throughput",
            "value": 3037145,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8ep throughput",
            "value": 2736562,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/128ep throughput",
            "value": 2554164,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep throughput",
            "value": 2381973,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep throughput",
            "value": 1997771,
            "unit": "deliveries/s"
          },
          {
            "name": "mixed 0B/fan6/128ep throughput",
            "value": 11569669,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep throughput",
            "value": 4217913,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep throughput",
            "value": 8423881,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep throughput",
            "value": 10330684,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep throughput",
            "value": 3327777,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep throughput",
            "value": 3812562,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep throughput",
            "value": 3100052,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep throughput",
            "value": 14846705,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep throughput",
            "value": 31532188,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n1 512B/fan1/1ep throughput",
            "value": 39667555,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n2 512B/fan1/1ep throughput",
            "value": 36112954,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n4 512B/fan1/1ep throughput",
            "value": 33411190,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n8 512B/fan1/1ep throughput",
            "value": 25767513,
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
          "id": "6a55d902cd9aaa00cae86cc759d0da91a06fb974",
          "message": "Merge pull request #347 from avatarsd-llc/feat/fwd-frame-view\n\nrefactor(net): fwd_frame_view — the FWD offset-dispatch cluster becomes a unit-testable public header",
          "timestamp": "2026-07-09T00:23:20+03:00",
          "tree_id": "7d59c421e52d4882e7d43ffcf09dc7fab3d9995c",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/6a55d902cd9aaa00cae86cc759d0da91a06fb974"
        },
        "date": 1783545855837,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep throughput",
            "value": 3478828.5,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8/1ep throughput",
            "value": 16955017,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan128/1ep throughput",
            "value": 38932362,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan1024/1ep throughput",
            "value": 38152396,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8192/1ep throughput",
            "value": 37835001,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1B/fan1/1ep throughput",
            "value": 3477285,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8B/fan1/1ep throughput",
            "value": 3498972,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1024B/fan1/1ep throughput",
            "value": 3311379,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8192B/fan1/1ep throughput",
            "value": 2383219,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep throughput",
            "value": 3944101,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep throughput",
            "value": 3934350,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep throughput",
            "value": 3943730,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep throughput",
            "value": 3929952,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep throughput",
            "value": 3941102,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1ep throughput",
            "value": 3058033,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8ep throughput",
            "value": 2730105,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/128ep throughput",
            "value": 2540822,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep throughput",
            "value": 2368520,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep throughput",
            "value": 2223703,
            "unit": "deliveries/s"
          },
          {
            "name": "mixed 0B/fan6/128ep throughput",
            "value": 11596285,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep throughput",
            "value": 4229403,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep throughput",
            "value": 8405603,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep throughput",
            "value": 10293921,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep throughput",
            "value": 3359113,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep throughput",
            "value": 3802659,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep throughput",
            "value": 3114510,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep throughput",
            "value": 14563356,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep throughput",
            "value": 31126723,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n1 512B/fan1/1ep throughput",
            "value": 39120278,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n2 512B/fan1/1ep throughput",
            "value": 26460916,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n4 512B/fan1/1ep throughput",
            "value": 33362058,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n8 512B/fan1/1ep throughput",
            "value": 25904122,
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
          "id": "c15f7087aaba0b9775dbe421ffa0ace6053aa2d6",
          "message": "Merge pull request #349 from avatarsd-llc/feat/grab-bag-smalls\n\nrefactor: grab-bag — stream_endpoint_t, config_reader_t, kMaxFrame dedup, view_as_tlv fold",
          "timestamp": "2026-07-09T01:07:46+03:00",
          "tree_id": "dd5e9db4cde5bfc0849817ea9e53c9d7fed6a13b",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/c15f7087aaba0b9775dbe421ffa0ace6053aa2d6"
        },
        "date": 1783548515997,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep throughput",
            "value": 3452009,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8/1ep throughput",
            "value": 18265892,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan128/1ep throughput",
            "value": 39410533,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan1024/1ep throughput",
            "value": 38936388,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8192/1ep throughput",
            "value": 37463902,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1B/fan1/1ep throughput",
            "value": 3436024,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8B/fan1/1ep throughput",
            "value": 3461728,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1024B/fan1/1ep throughput",
            "value": 3299142,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8192B/fan1/1ep throughput",
            "value": 2384050,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep throughput",
            "value": 3924831,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep throughput",
            "value": 3924345,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep throughput",
            "value": 3928131,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep throughput",
            "value": 3917564,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep throughput",
            "value": 3911506,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1ep throughput",
            "value": 3013783,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8ep throughput",
            "value": 2729882,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/128ep throughput",
            "value": 2511340,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep throughput",
            "value": 2352730,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep throughput",
            "value": 2223588,
            "unit": "deliveries/s"
          },
          {
            "name": "mixed 0B/fan6/128ep throughput",
            "value": 11432266,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep throughput",
            "value": 4220543,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep throughput",
            "value": 8294978,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep throughput",
            "value": 10303337,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep throughput",
            "value": 3325790,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep throughput",
            "value": 3796831,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep throughput",
            "value": 3130599,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep throughput",
            "value": 15060287,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep throughput",
            "value": 31704281,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n1 512B/fan1/1ep throughput",
            "value": 39627837,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n2 512B/fan1/1ep throughput",
            "value": 36592154,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n4 512B/fan1/1ep throughput",
            "value": 33386096,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n8 512B/fan1/1ep throughput",
            "value": 25903214,
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
          "id": "757d15858ca9a9b7243a9933a1a6215ad784e24a",
          "message": "Merge pull request #350 from avatarsd-llc/feat/perf-charts-unified\n\ndocs(perf): unified family trend charts with release markers (trend/sweep/heatmap/3D)",
          "timestamp": "2026-07-09T01:29:29+03:00",
          "tree_id": "55b46771d5d1869da788c9db38c6c8d1b15341b5",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/757d15858ca9a9b7243a9933a1a6215ad784e24a"
        },
        "date": 1783549820608,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep throughput",
            "value": 3458328,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8/1ep throughput",
            "value": 18211298,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan128/1ep throughput",
            "value": 40316119,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan1024/1ep throughput",
            "value": 39350711,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8192/1ep throughput",
            "value": 39889413,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1B/fan1/1ep throughput",
            "value": 3446266,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8B/fan1/1ep throughput",
            "value": 3469167,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1024B/fan1/1ep throughput",
            "value": 3279463,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8192B/fan1/1ep throughput",
            "value": 2360057,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep throughput",
            "value": 3917511,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep throughput",
            "value": 3923039,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep throughput",
            "value": 3940841,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep throughput",
            "value": 3873039,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep throughput",
            "value": 3931416,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1ep throughput",
            "value": 3040427,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8ep throughput",
            "value": 2752462,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/128ep throughput",
            "value": 2541530,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep throughput",
            "value": 2364391,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep throughput",
            "value": 2176734,
            "unit": "deliveries/s"
          },
          {
            "name": "mixed 0B/fan6/128ep throughput",
            "value": 11842878,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep throughput",
            "value": 3944872,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep throughput",
            "value": 8402341,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep throughput",
            "value": 10330105,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep throughput",
            "value": 3340142,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep throughput",
            "value": 3791566,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep throughput",
            "value": 3098403,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep throughput",
            "value": 14979746,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep throughput",
            "value": 30858657,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n1 512B/fan1/1ep throughput",
            "value": 39466485,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n2 512B/fan1/1ep throughput",
            "value": 27132293,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n4 512B/fan1/1ep throughput",
            "value": 33473352,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n8 512B/fan1/1ep throughput",
            "value": 25920718,
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
          "id": "1c0d0e8d530902c6d96bf057b352d6e3915a8910",
          "message": "Merge pull request #351 from avatarsd-llc/test/conformance-opt-bit-vectors\n\ntest(conformance): opt-bit position coverage + negative-vector harness contract",
          "timestamp": "2026-07-09T01:35:55+03:00",
          "tree_id": "5f7d85ba8c58d4d449e0ebab731f0ff2d812cf3a",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/1c0d0e8d530902c6d96bf057b352d6e3915a8910"
        },
        "date": 1783550207646,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep throughput",
            "value": 3473537.5,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8/1ep throughput",
            "value": 18388058,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan128/1ep throughput",
            "value": 40971554,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan1024/1ep throughput",
            "value": 39752965,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8192/1ep throughput",
            "value": 39804527,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1B/fan1/1ep throughput",
            "value": 3454325,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8B/fan1/1ep throughput",
            "value": 3495165,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1024B/fan1/1ep throughput",
            "value": 3309657,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8192B/fan1/1ep throughput",
            "value": 2392410,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep throughput",
            "value": 3929201,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep throughput",
            "value": 3919406,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep throughput",
            "value": 3926845,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep throughput",
            "value": 3917235,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep throughput",
            "value": 3915624,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1ep throughput",
            "value": 3052450,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8ep throughput",
            "value": 2739955,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/128ep throughput",
            "value": 2526552,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep throughput",
            "value": 2361882,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep throughput",
            "value": 2237037,
            "unit": "deliveries/s"
          },
          {
            "name": "mixed 0B/fan6/128ep throughput",
            "value": 11939545,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep throughput",
            "value": 4228807,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep throughput",
            "value": 8435482,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep throughput",
            "value": 10332699,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep throughput",
            "value": 3343554,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep throughput",
            "value": 3784193,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep throughput",
            "value": 3106537,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep throughput",
            "value": 15105966,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep throughput",
            "value": 31583138,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n1 512B/fan1/1ep throughput",
            "value": 39219180,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n2 512B/fan1/1ep throughput",
            "value": 27478714,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n4 512B/fan1/1ep throughput",
            "value": 33376171,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n8 512B/fan1/1ep throughput",
            "value": 25915038,
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
          "id": "17e2250a685bd5ce0814128fe45de88c188b8191",
          "message": "Merge pull request #352 from avatarsd-llc/fix/fan1-delivery-regression\n\nfix(graph): eliminate the fan-out snapshot zeroing stall — fan1 dispatch 143→103 ns",
          "timestamp": "2026-07-09T01:44:02+03:00",
          "tree_id": "8df984784af06b23abcfef1463380bdbb6795f97",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/17e2250a685bd5ce0814128fe45de88c188b8191"
        },
        "date": 1783550689299,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep throughput",
            "value": 5616567,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8/1ep throughput",
            "value": 23262440,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan128/1ep throughput",
            "value": 30651689,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan1024/1ep throughput",
            "value": 38292577,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8192/1ep throughput",
            "value": 38007950,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1B/fan1/1ep throughput",
            "value": 5599576,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8B/fan1/1ep throughput",
            "value": 5692699,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1024B/fan1/1ep throughput",
            "value": 5241652,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8192B/fan1/1ep throughput",
            "value": 3271213,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep throughput",
            "value": 7054576,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep throughput",
            "value": 7220778,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep throughput",
            "value": 7202469,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep throughput",
            "value": 7217726,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep throughput",
            "value": 7235345,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1ep throughput",
            "value": 4775801,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8ep throughput",
            "value": 4029808,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/128ep throughput",
            "value": 3619335,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep throughput",
            "value": 3298402,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep throughput",
            "value": 3024336,
            "unit": "deliveries/s"
          },
          {
            "name": "mixed 0B/fan6/128ep throughput",
            "value": 14953432,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep throughput",
            "value": 8450671,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep throughput",
            "value": 16854778,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep throughput",
            "value": 18122108,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep throughput",
            "value": 5518566,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep throughput",
            "value": 7069936,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep throughput",
            "value": 4827264,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep throughput",
            "value": 14309838,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep throughput",
            "value": 30981485,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n1 512B/fan1/1ep throughput",
            "value": 39000809,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n2 512B/fan1/1ep throughput",
            "value": 36094185,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n4 512B/fan1/1ep throughput",
            "value": 32370112,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n8 512B/fan1/1ep throughput",
            "value": 25553415,
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
          "id": "0d032f9b5e1c58785b2976ad8ab644a845c75e5e",
          "message": "Merge pull request #354 from avatarsd-llc/perf/write-path-reductions\n\nperf(graph): deliver the published LKV pointer — fan1 dispatch ~101→~90 ns",
          "timestamp": "2026-07-09T02:13:55+03:00",
          "tree_id": "002b72020ff8fe2061b936da8db1080e03173092",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/0d032f9b5e1c58785b2976ad8ab644a845c75e5e"
        },
        "date": 1783552477868,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep throughput",
            "value": 6694202.5,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8/1ep throughput",
            "value": 27748595,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan128/1ep throughput",
            "value": 42504436,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan1024/1ep throughput",
            "value": 41153075,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8192/1ep throughput",
            "value": 40865840,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1B/fan1/1ep throughput",
            "value": 6746741,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8B/fan1/1ep throughput",
            "value": 6698618,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1024B/fan1/1ep throughput",
            "value": 6253316,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8192B/fan1/1ep throughput",
            "value": 3740416,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep throughput",
            "value": 8015075,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep throughput",
            "value": 8068104,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep throughput",
            "value": 8045953,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep throughput",
            "value": 8089697,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep throughput",
            "value": 8058291,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1ep throughput",
            "value": 5518603,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8ep throughput",
            "value": 5130796,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/128ep throughput",
            "value": 4328090,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep throughput",
            "value": 3784782,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep throughput",
            "value": 3134194,
            "unit": "deliveries/s"
          },
          {
            "name": "mixed 0B/fan6/128ep throughput",
            "value": 17753252,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep throughput",
            "value": 9660209,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep throughput",
            "value": 19238587,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep throughput",
            "value": 20582450,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep throughput",
            "value": 6515307,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep throughput",
            "value": 7966611,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep throughput",
            "value": 5390870,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep throughput",
            "value": 12054058,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep throughput",
            "value": 31393537,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n1 512B/fan1/1ep throughput",
            "value": 43565217,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n2 512B/fan1/1ep throughput",
            "value": 40391423,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n4 512B/fan1/1ep throughput",
            "value": 35464597,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n8 512B/fan1/1ep throughput",
            "value": 27381580,
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
          "id": "36a9c7d8369f49dfc8b5cb1891a92aed136ec1fc",
          "message": "Merge pull request #353 from avatarsd-llc/bench/comparison-fairness\n\nbench: honest zenoh comparison — deliver-only row, symmetric bracketing, corrected narrative",
          "timestamp": "2026-07-09T02:44:16+03:00",
          "tree_id": "b38d8f566185f022d69f62e15aec3b81036e5132",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/36a9c7d8369f49dfc8b5cb1891a92aed136ec1fc"
        },
        "date": 1783554304426,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep throughput",
            "value": 6389639.5,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8/1ep throughput",
            "value": 22072012,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan128/1ep throughput",
            "value": 41606348,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan1024/1ep throughput",
            "value": 42400130,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8192/1ep throughput",
            "value": 41882264,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1B/fan1/1ep throughput",
            "value": 6568728,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8B/fan1/1ep throughput",
            "value": 6566763,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1024B/fan1/1ep throughput",
            "value": 6168485,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8192B/fan1/1ep throughput",
            "value": 3756058,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep throughput",
            "value": 7753103,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep throughput",
            "value": 7695961,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep throughput",
            "value": 7710935,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep throughput",
            "value": 7726012,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep throughput",
            "value": 7680848,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1ep throughput",
            "value": 5408968,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8ep throughput",
            "value": 4971407,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/128ep throughput",
            "value": 4429281,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep throughput",
            "value": 3777330,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep throughput",
            "value": 3071116,
            "unit": "deliveries/s"
          },
          {
            "name": "mixed 0B/fan6/128ep throughput",
            "value": 17484621,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep throughput",
            "value": 9662071,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep throughput",
            "value": 16811617,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep throughput",
            "value": 20472513,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep throughput",
            "value": 6425377,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep throughput",
            "value": 7609086,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep throughput",
            "value": 5220695,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep throughput",
            "value": 14775937,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep throughput",
            "value": 31164483,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n1 512B/fan1/1ep throughput",
            "value": 44938469,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n2 512B/fan1/1ep throughput",
            "value": 42716967,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n4 512B/fan1/1ep throughput",
            "value": 30893490,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n8 512B/fan1/1ep throughput",
            "value": 19408430,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep throughput",
            "value": 13799772,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep throughput",
            "value": 33556152,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep throughput",
            "value": 44483328,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep throughput",
            "value": 41446557,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep throughput",
            "value": 41319891,
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
          "id": "ee71f0bc6e544ac64a13b9925109c35bfbd3b4ab",
          "message": "Merge pull request #357 from avatarsd-llc/docs/doxygen-sweep-core\n\ndocs(core): doxygen /** @brief */ sweep — src, tests, bench",
          "timestamp": "2026-07-09T03:07:17+03:00",
          "tree_id": "b6f12e980d6e5756044ec44c3fd821a768115190",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/ee71f0bc6e544ac64a13b9925109c35bfbd3b4ab"
        },
        "date": 1783555689297,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep throughput",
            "value": 6000953.5,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8/1ep throughput",
            "value": 24341195,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan128/1ep throughput",
            "value": 40195464,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan1024/1ep throughput",
            "value": 37932043,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8192/1ep throughput",
            "value": 37675983,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1B/fan1/1ep throughput",
            "value": 6071134,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8B/fan1/1ep throughput",
            "value": 5961514,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1024B/fan1/1ep throughput",
            "value": 5417865,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8192B/fan1/1ep throughput",
            "value": 3449278,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep throughput",
            "value": 7786168,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep throughput",
            "value": 7812135,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep throughput",
            "value": 7813312,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep throughput",
            "value": 7801523,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep throughput",
            "value": 7799679,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1ep throughput",
            "value": 5075367,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8ep throughput",
            "value": 4128185,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/128ep throughput",
            "value": 3674596,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep throughput",
            "value": 3311998,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep throughput",
            "value": 3017704,
            "unit": "deliveries/s"
          },
          {
            "name": "mixed 0B/fan6/128ep throughput",
            "value": 15443733,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep throughput",
            "value": 9516297,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep throughput",
            "value": 18953989,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep throughput",
            "value": 19829004,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep throughput",
            "value": 5966374,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep throughput",
            "value": 7674097,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep throughput",
            "value": 4975005,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep throughput",
            "value": 14402262,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep throughput",
            "value": 31795967,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n1 512B/fan1/1ep throughput",
            "value": 39146586,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n2 512B/fan1/1ep throughput",
            "value": 25933862,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n4 512B/fan1/1ep throughput",
            "value": 32185350,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n8 512B/fan1/1ep throughput",
            "value": 24776677,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep throughput",
            "value": 13470854,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep throughput",
            "value": 35860762,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep throughput",
            "value": 43737216,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep throughput",
            "value": 40686793,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep throughput",
            "value": 39814528,
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
          "id": "94fc98d85dd5ddfc17262c59b6546c72e1fb4f97",
          "message": "Merge pull request #360 from avatarsd-llc/release/0.4.0-changelog-cut\n\nrelease: v0.4.0 changelog cut + version reconcile",
          "timestamp": "2026-07-09T09:39:43+03:00",
          "tree_id": "c7ebe15478d6901fe16c845711726e5d6cecff9c",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/94fc98d85dd5ddfc17262c59b6546c72e1fb4f97"
        },
        "date": 1783579242631,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep throughput",
            "value": 6383834,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8/1ep throughput",
            "value": 26466071,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan128/1ep throughput",
            "value": 39316181,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan1024/1ep throughput",
            "value": 34262708,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8192/1ep throughput",
            "value": 32518294,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1B/fan1/1ep throughput",
            "value": 6511236,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8B/fan1/1ep throughput",
            "value": 6517059,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1024B/fan1/1ep throughput",
            "value": 6061393,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8192B/fan1/1ep throughput",
            "value": 4366971,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep throughput",
            "value": 7808673,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep throughput",
            "value": 7827075,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep throughput",
            "value": 7810738,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep throughput",
            "value": 7835110,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep throughput",
            "value": 7768403,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1ep throughput",
            "value": 5137943,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8ep throughput",
            "value": 4089789,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/128ep throughput",
            "value": 3831562,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep throughput",
            "value": 3586916,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep throughput",
            "value": 2798500,
            "unit": "deliveries/s"
          },
          {
            "name": "mixed 0B/fan6/128ep throughput",
            "value": 17746254,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep throughput",
            "value": 7064808,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep throughput",
            "value": 13699914,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep throughput",
            "value": 14276113,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep throughput",
            "value": 5504151,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep throughput",
            "value": 6363279,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep throughput",
            "value": 4333496,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep throughput",
            "value": 10152292,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep throughput",
            "value": 20698086,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n1 512B/fan1/1ep throughput",
            "value": 36584607,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n2 512B/fan1/1ep throughput",
            "value": 33115200,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n4 512B/fan1/1ep throughput",
            "value": 29489648,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n8 512B/fan1/1ep throughput",
            "value": 23285209,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep throughput",
            "value": 9801112,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep throughput",
            "value": 32013490,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep throughput",
            "value": 41076469,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep throughput",
            "value": 36424388,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep throughput",
            "value": 33815654,
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
          "id": "94fc98d85dd5ddfc17262c59b6546c72e1fb4f97",
          "message": "Merge pull request #360 from avatarsd-llc/release/0.4.0-changelog-cut\n\nrelease: v0.4.0 changelog cut + version reconcile",
          "timestamp": "2026-07-09T09:39:43+03:00",
          "tree_id": "c7ebe15478d6901fe16c845711726e5d6cecff9c",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/94fc98d85dd5ddfc17262c59b6546c72e1fb4f97"
        },
        "date": 1783579792120,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep throughput",
            "value": 6094563.5,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8/1ep throughput",
            "value": 25835517,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan128/1ep throughput",
            "value": 42453518,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan1024/1ep throughput",
            "value": 40526228,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8192/1ep throughput",
            "value": 40001217,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1B/fan1/1ep throughput",
            "value": 6192552,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8B/fan1/1ep throughput",
            "value": 6192462,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1024B/fan1/1ep throughput",
            "value": 5561058,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8192B/fan1/1ep throughput",
            "value": 3452831,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep throughput",
            "value": 7791945,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep throughput",
            "value": 7754880,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep throughput",
            "value": 7729822,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep throughput",
            "value": 7775892,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep throughput",
            "value": 7787516,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1ep throughput",
            "value": 5072004,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8ep throughput",
            "value": 4160602,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/128ep throughput",
            "value": 3703127,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep throughput",
            "value": 3316893,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep throughput",
            "value": 3025215,
            "unit": "deliveries/s"
          },
          {
            "name": "mixed 0B/fan6/128ep throughput",
            "value": 14774668,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep throughput",
            "value": 9512645,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep throughput",
            "value": 18709754,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep throughput",
            "value": 18603958,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep throughput",
            "value": 6050370,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep throughput",
            "value": 7664910,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep throughput",
            "value": 5055666,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep throughput",
            "value": 14887442,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep throughput",
            "value": 31778922,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n1 512B/fan1/1ep throughput",
            "value": 39206131,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n2 512B/fan1/1ep throughput",
            "value": 30557270,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n4 512B/fan1/1ep throughput",
            "value": 32286243,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n8 512B/fan1/1ep throughput",
            "value": 24791510,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep throughput",
            "value": 13498365,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep throughput",
            "value": 32816011,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep throughput",
            "value": 41191446,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep throughput",
            "value": 38316094,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep throughput",
            "value": 37780805,
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
          "id": "7fc944a4a9e22cea3edaa0930961273308fa1be3",
          "message": "Merge pull request #363 from avatarsd-llc/feat/vertex-diet-hot-cold-split\n\nperf(graph): vertex_t hot/cold split — lazy vertex_ext_t, 536→248 B (#361 §1)",
          "timestamp": "2026-07-09T17:39:00+03:00",
          "tree_id": "29e6839269233dabca3175c8f8e34569fab10540",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/7fc944a4a9e22cea3edaa0930961273308fa1be3"
        },
        "date": 1783607984595,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep throughput",
            "value": 11238023,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8/1ep throughput",
            "value": 39647946,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan128/1ep throughput",
            "value": 65170701,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan1024/1ep throughput",
            "value": 67301368,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8192/1ep throughput",
            "value": 65678079,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1B/fan1/1ep throughput",
            "value": 11509100,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8B/fan1/1ep throughput",
            "value": 11290448,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1024B/fan1/1ep throughput",
            "value": 10219466,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8192B/fan1/1ep throughput",
            "value": 6473163,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep throughput",
            "value": 13304169,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep throughput",
            "value": 13287730,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep throughput",
            "value": 13213935,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep throughput",
            "value": 13316820,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep throughput",
            "value": 13419137,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1ep throughput",
            "value": 9234900,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8ep throughput",
            "value": 8002851,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/128ep throughput",
            "value": 6953805,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep throughput",
            "value": 6717784,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep throughput",
            "value": 6297093,
            "unit": "deliveries/s"
          },
          {
            "name": "mixed 0B/fan6/128ep throughput",
            "value": 29719199,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep throughput",
            "value": 10614057,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep throughput",
            "value": 21649812,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep throughput",
            "value": 29426057,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep throughput",
            "value": 7596147,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep throughput",
            "value": 8531735,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep throughput",
            "value": 6730397,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep throughput",
            "value": 13247494,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep throughput",
            "value": 40117557,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n1 512B/fan1/1ep throughput",
            "value": 84032480,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n2 512B/fan1/1ep throughput",
            "value": 74963008,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n4 512B/fan1/1ep throughput",
            "value": 65447391,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n8 512B/fan1/1ep throughput",
            "value": 52813672,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep throughput",
            "value": 14501482,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep throughput",
            "value": 45553717,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep throughput",
            "value": 65501378,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep throughput",
            "value": 68028309,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep throughput",
            "value": 67111817,
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
          "id": "afefd06d1d1ec729ddbe4bd5a4a068266324104f",
          "message": "Merge pull request #364 from avatarsd-llc/feat/ws-multi-peer\n\nfeat(transport_ws): multi-peer server + opt-in ADR-0044 bus facet (#362)",
          "timestamp": "2026-07-09T17:44:32+03:00",
          "tree_id": "196e774756afa864f1898c57373fc709f428818e",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/afefd06d1d1ec729ddbe4bd5a4a068266324104f"
        },
        "date": 1783608322550,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep throughput",
            "value": 8186726,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8/1ep throughput",
            "value": 30780281,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan128/1ep throughput",
            "value": 45152035,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan1024/1ep throughput",
            "value": 43919994,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8192/1ep throughput",
            "value": 43960552,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1B/fan1/1ep throughput",
            "value": 8261867,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8B/fan1/1ep throughput",
            "value": 8297430,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1024B/fan1/1ep throughput",
            "value": 6941881,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8192B/fan1/1ep throughput",
            "value": 4671754,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep throughput",
            "value": 10118338,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep throughput",
            "value": 9820544,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep throughput",
            "value": 10147124,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep throughput",
            "value": 10175047,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep throughput",
            "value": 10169270,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1ep throughput",
            "value": 6995057,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8ep throughput",
            "value": 6504792,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/128ep throughput",
            "value": 5888850,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep throughput",
            "value": 5352462,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep throughput",
            "value": 4502867,
            "unit": "deliveries/s"
          },
          {
            "name": "mixed 0B/fan6/128ep throughput",
            "value": 21445502,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep throughput",
            "value": 12564117,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep throughput",
            "value": 25163749,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep throughput",
            "value": 26453059,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep throughput",
            "value": 7976263,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep throughput",
            "value": 9932894,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep throughput",
            "value": 6776393,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep throughput",
            "value": 18675578,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep throughput",
            "value": 40367959,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n1 512B/fan1/1ep throughput",
            "value": 54238531,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n2 512B/fan1/1ep throughput",
            "value": 54170648,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n4 512B/fan1/1ep throughput",
            "value": 38405065,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n8 512B/fan1/1ep throughput",
            "value": 23354272,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep throughput",
            "value": 16872484,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep throughput",
            "value": 40731535,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep throughput",
            "value": 46788299,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep throughput",
            "value": 44277757,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep throughput",
            "value": 43894842,
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
          "id": "8c95ec1ee7eaae4e82fa897f92f89cd24d7afcc8",
          "message": "Merge pull request #366 from avatarsd-llc/feat/vertex-diet-striped-locks\n\nperf(graph): vertex diet §2+§3 — striped locks (248→160 B) + bearing-ancestor ACL state (#361)",
          "timestamp": "2026-07-09T18:00:30+03:00",
          "tree_id": "938a179e0ac30b3b8773c579e04b5cc721840e30",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/8c95ec1ee7eaae4e82fa897f92f89cd24d7afcc8"
        },
        "date": 1783609294812,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep throughput",
            "value": 5780340,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8/1ep throughput",
            "value": 23203447,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan128/1ep throughput",
            "value": 37963454,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan1024/1ep throughput",
            "value": 37201511,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8192/1ep throughput",
            "value": 37303940,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1B/fan1/1ep throughput",
            "value": 5729750,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8B/fan1/1ep throughput",
            "value": 5796958,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1024B/fan1/1ep throughput",
            "value": 5431366,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8192B/fan1/1ep throughput",
            "value": 3298917,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep throughput",
            "value": 7282201,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep throughput",
            "value": 7239020,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep throughput",
            "value": 7260378,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep throughput",
            "value": 7276786,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep throughput",
            "value": 7049402,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1ep throughput",
            "value": 4806822,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8ep throughput",
            "value": 4105307,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/128ep throughput",
            "value": 3683816,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep throughput",
            "value": 3434567,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep throughput",
            "value": 3111199,
            "unit": "deliveries/s"
          },
          {
            "name": "mixed 0B/fan6/128ep throughput",
            "value": 15359706,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep throughput",
            "value": 8149988,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep throughput",
            "value": 17242982,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep throughput",
            "value": 18116826,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep throughput",
            "value": 5666974,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep throughput",
            "value": 7146382,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep throughput",
            "value": 4636343,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep throughput",
            "value": 13412135,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep throughput",
            "value": 7046344,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n1 512B/fan1/1ep throughput",
            "value": 38347950,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n2 512B/fan1/1ep throughput",
            "value": 36036526,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n4 512B/fan1/1ep throughput",
            "value": 31902033,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n8 512B/fan1/1ep throughput",
            "value": 24725422,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep throughput",
            "value": 12400697,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep throughput",
            "value": 32139097,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep throughput",
            "value": 40919492,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep throughput",
            "value": 38107541,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep throughput",
            "value": 37463264,
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
          "id": "3466741cd72e56020b82a27cb6a987cde2eec0cf",
          "message": "Merge pull request #367 from avatarsd-llc/feat/vertex-diet-pmr-lkv\n\nfeat(graph): ADR-0039 injected memory_resource — pmr LKV allocation seam (#361 §5)",
          "timestamp": "2026-07-09T18:13:27+03:00",
          "tree_id": "3050103f0c3d73d4bb14361e5d52468c4cad04aa",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/3466741cd72e56020b82a27cb6a987cde2eec0cf"
        },
        "date": 1783610054694,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep throughput",
            "value": 7500533,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8/1ep throughput",
            "value": 32252410,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan128/1ep throughput",
            "value": 43355623,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan1024/1ep throughput",
            "value": 43872485,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8192/1ep throughput",
            "value": 44512766,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1B/fan1/1ep throughput",
            "value": 7612165,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8B/fan1/1ep throughput",
            "value": 7811706,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1024B/fan1/1ep throughput",
            "value": 6826637,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8192B/fan1/1ep throughput",
            "value": 4544245,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep throughput",
            "value": 8973257,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep throughput",
            "value": 9059874,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep throughput",
            "value": 8970379,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep throughput",
            "value": 9040691,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep throughput",
            "value": 9047015,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1ep throughput",
            "value": 6412018,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8ep throughput",
            "value": 5844622,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/128ep throughput",
            "value": 5336649,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep throughput",
            "value": 4835031,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep throughput",
            "value": 4134734,
            "unit": "deliveries/s"
          },
          {
            "name": "mixed 0B/fan6/128ep throughput",
            "value": 21411058,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep throughput",
            "value": 11067318,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep throughput",
            "value": 20813057,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep throughput",
            "value": 20450864,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep throughput",
            "value": 7443158,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep throughput",
            "value": 8386808,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep throughput",
            "value": 6095280,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep throughput",
            "value": 18255566,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep throughput",
            "value": 7544329,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n1 512B/fan1/1ep throughput",
            "value": 53914410,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n2 512B/fan1/1ep throughput",
            "value": 47855443,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n4 512B/fan1/1ep throughput",
            "value": 36144546,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n8 512B/fan1/1ep throughput",
            "value": 25006668,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep throughput",
            "value": 16432433,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep throughput",
            "value": 44910235,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep throughput",
            "value": 47697151,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep throughput",
            "value": 45063034,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep throughput",
            "value": 43934469,
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
          "id": "905d3cd5bdb016763d8a579ea0695d7f979d9d5b",
          "message": "Merge pull request #368 from avatarsd-llc/feat/vertex-diet-heap-history\n\nfeat(bench): per-vertex steady-heap history probe (#361 §8)",
          "timestamp": "2026-07-09T18:24:58+03:00",
          "tree_id": "34e34af6686f259ec0b4f9ac4ae7ffa67b18062d",
          "url": "https://github.com/avatarsd-llc/libtracer/commit/905d3cd5bdb016763d8a579ea0695d7f979d9d5b"
        },
        "date": 1783610755831,
        "tool": "customBiggerIsBetter",
        "benches": [
          {
            "name": "inproc 64B/fan1/1ep throughput",
            "value": 5287634,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8/1ep throughput",
            "value": 22736473,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan128/1ep throughput",
            "value": 38515990,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan1024/1ep throughput",
            "value": 38144910,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 64B/fan8192/1ep throughput",
            "value": 39036393,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1B/fan1/1ep throughput",
            "value": 5252115,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8B/fan1/1ep throughput",
            "value": 5231962,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 1024B/fan1/1ep throughput",
            "value": 5006927,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc 8192B/fan1/1ep throughput",
            "value": 3122756,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1B/fan1/1ep throughput",
            "value": 6003820,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8B/fan1/1ep throughput",
            "value": 6022764,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 64B/fan1/1ep throughput",
            "value": 6035492,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 1024B/fan1/1ep throughput",
            "value": 6006142,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-borrow 8192B/fan1/1ep throughput",
            "value": 6014475,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1ep throughput",
            "value": 4447571,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8ep throughput",
            "value": 3739216,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/128ep throughput",
            "value": 3388342,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/1024ep throughput",
            "value": 3189563,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-path 64B/fan1/8192ep throughput",
            "value": 2905593,
            "unit": "deliveries/s"
          },
          {
            "name": "mixed 0B/fan6/128ep throughput",
            "value": 15458848,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt1 64B/fan1/1ep throughput",
            "value": 7644649,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt2 64B/fan1/2ep throughput",
            "value": 15264940,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-mt4 64B/fan1/4ep throughput",
            "value": 16781150,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean 64B/fan1/1ep throughput",
            "value": 5261266,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-lean-cached 64B/fan1/1ep throughput",
            "value": 5904522,
            "unit": "deliveries/s"
          },
          {
            "name": "eptype-stream 64B/fan1/1ep throughput",
            "value": 4350637,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4 64B/fan1/1ep throughput",
            "value": 14328671,
            "unit": "deliveries/s"
          },
          {
            "name": "acl-inherit-d4-mt4 64B/fan1/4ep throughput",
            "value": 7763157,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n1 512B/fan1/1ep throughput",
            "value": 38915399,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n2 512B/fan1/1ep throughput",
            "value": 29244832,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n4 512B/fan1/1ep throughput",
            "value": 31541332,
            "unit": "deliveries/s"
          },
          {
            "name": "fold-n8 512B/fan1/1ep throughput",
            "value": 24906633,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan1/1ep throughput",
            "value": 12475735,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan8/1ep throughput",
            "value": 32443133,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan128/1ep throughput",
            "value": 41060266,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan1024/1ep throughput",
            "value": 38775860,
            "unit": "deliveries/s"
          },
          {
            "name": "inproc-deliver 64B/fan8192/1ep throughput",
            "value": 37626670,
            "unit": "deliveries/s"
          }
        ]
      }
    ]
  }
}