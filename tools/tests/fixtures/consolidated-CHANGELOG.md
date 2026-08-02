# Changelog — fixture (release-mechanics test input)

A miniature of `core/CHANGELOG.md` at tag time: an `[Unreleased]` section whose
`###` headings are interleaved one-per-landing-PR, plus two traps the parser
must survive even though today's real file happens to contain neither — a fenced
code block holding a line that *looks* like a `###` heading, and an
em-dash-bearing released heading below.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- **First added entry (#102).**

  A second paragraph, indented under the bullet.

- **Second added entry (#105).** Followed by a column-zero fence — the case an
  indented fence cannot exercise, because an indented line never matched the
  heading pattern in the first place:

```
### Not A Heading At Column Zero
## Also Not A Section At Column Zero
```

### Changed

- **First changed entry (#101).** Landed first.

- **Second changed entry (#104).** Its body contains a fence whose text would
  otherwise parse as a heading:

  ```
  ### Not A Heading
  ## Also Not A Heading
  ```

- **Third changed entry (#108).** Landed last.

### Removed

- **A removed entry (#107).**

### Fixed

- **First fixed entry (#103).**

### Security

- **A security entry (#106).**

### Notes for packagers

- An unrecognised category. Keep-a-Changelog does not name it, so it sorts after
  the six that it does.

## [0.1.0] — 2026-01-01

### Added

- The first cut.
