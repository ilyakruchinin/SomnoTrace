# components/

First-party, reusable ESP-IDF components developed **in-house** for SomnoTrace
live here. ESP-IDF automatically discovers any component directory under
`components/`.

Each component is its own directory with at least:

```
components/
  my_component/
    CMakeLists.txt      # idf_component_register(...)
    include/            # public headers
    my_component.c
```

Guidelines:

- Keep `main/` thin — push reusable logic (BLE transports, EDF writer, upload
  clients, etc.) into well-scoped components here.
- These are licensed under the project license (GPLv3 + §7(b)); add the
  standard source header (see `docs/source-header.txt`).
- **Vendored third-party** code does **not** go here — use `third_party/`.
- Components pulled from the ESP Component Registry are fetched into the
  git-ignored `managed_components/` directory at build time and declared via
  `idf_component.yml`, not committed here.
