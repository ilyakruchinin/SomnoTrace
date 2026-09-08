# third_party/

Vendored **third-party** source code and resources live here, each in its own
subdirectory, kept separate from first-party code so provenance and licensing
stay clear.

## Rules

1. **Every** vendored item must include its **upstream LICENSE** file and a
   short note of its origin (URL + version/commit).
2. Only include material whose license is **compatible with GPLv3 and permits
   commercial use** (the project keeps a commercial-licensing option open):
   - Compatible code licenses: MIT, BSD-2/3-Clause, Apache-2.0 (Apache-2.0 is
     one-way compatible into GPLv3), zlib, ISC, public domain / CC0.
   - **Avoid:** anything non-commercial (e.g. CC BY-NC), proprietary, or
     GPLv3-incompatible.
3. Record each addition in the repo-root `THIRD-PARTY-NOTICES.md`.
4. **Do not** copy third-party source into first-party files. SomnoTrace's own
   code is a clean-room implementation (see `CONTRIBUTING.md`).

## Layout

```
third_party/
  <name>/
    LICENSE
    ORIGIN.md           # URL, version/commit, what it's used for
    ...source/resources...
```

Fonts and other binary assets belong under `assets/` (see `assets/fonts/`),
unless they ship as part of a self-contained third-party package, in which case
keep them with that package here.
