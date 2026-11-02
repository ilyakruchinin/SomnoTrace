# Settings history and upgrades

SomnoTrace writes one settings record per noon-to-noon day in `STR.edf`.
It no longer fills an older day's missing settings with the settings from
the session being exported. If the source is uncertain, the generated Mode
and JSON-backed settings are `-1` (unknown); summary statistics and the
spool-derived heated-tube and humidifier fields are retained.

## Upgrade impact

**Existing unmarked snapshots are not considered authoritative**, because
older versions could save borrowed settings under a session's own filename.
The files are left in place, but even correct older settings may now appear
as unknown in newly generated exports. This is intentional: the firmware
cannot distinguish genuine snapshots from previously borrowed ones.
Even a provenance-marked live settings file is not sufficient if its
session's identity was unmarked or copied from another session. Existing
imports without independently verified EDF-header identity are likewise
unverified. Imported daily snapshots without the verified-coverage marker
also remain unknown until a fresh re-import. These files remain intact; the
firmware does not infer identity from filenames or upgrade old markers
automatically.

OSCAR can record an unknown mode rather than treat the day as absent, and a
normal subsequent import may skip already imported days. Other destinations
may also keep existing settings instead of replacing them. Back up the
consumer's data and consult its recovery or re-import instructions before
attempting to repair history; generating a better STR alone may not update
an existing consumer record. These consumer recovery paths have not been
tested end-to-end.

## New captures and recovery

- A successful AS11 Get RPC is saved with its session and noon-day provenance.
  Identity and settings must both come from the same BLE connection; cached
  device settings and copied identities are not historical proof.
- For multiple sessions in one day, their exported settings must agree; if a
  session is missing a verified capture or the values differ, settings for
  that day are unknown. A capture reflects the Get response after therapy,
  not a guarantee that settings never changed during the session.
- An imported `STR.edf` supplies settings only from each record's own fields.
  Missing fields are not filled from `CurrentSettings.json`. The importer
  requires a machine serial and preserves conflicting source files and daily
  snapshots instead of overwriting another machine's data. A later local
  session makes the imported day's snapshot stale until re-imported.
- A malformed, incomplete, conflicting or identity-less root import remains
  in `IMPORT/` for a later retry; other valid STR days and independent DATALOG
  groups can still be processed. Check the import log, correct the conflicting
  files or session identities, then trigger another import. I/O failures retain
  their sources for a later run; only clearly invalid session EDFs are
  quarantined. Quarantined files retain their checksums and drift sidecars.
- **Import a fresh, genuine ResMed card export for recovery.** The import
  treats its STR records as authoritative; the EDF format does not prove who
  generated the file. Importing an older SomnoTrace-generated STR can reintroduce
  previously fabricated history.
- AS10 imports preserve supported CPAP, AutoSet, bilevel, VAuto, ASV and common
  settings as physical values from the card's own EDF header, retaining AS10
  mode and enum values and labels. Unsupported fields (including some ST and
  iVAPS settings), unknown units and unrepresentable values remain unknown.
  AS10 AutoSet-for-Her's shared AutoSet pressures are also exposed under the
  labels used by OSCAR. The generated signal order is not a native card image.
  This path has synthetic end-to-end tests but has not yet been checked against
  a real AS10 card or an actual OSCAR import.

Settings also remain unknown if the export's device identity or a relevant
session's identity cannot be verified. `SETTINGS/CurrentSettings.json` keeps
its existing current-settings fallback behavior and does not contain
SomnoTrace's internal provenance metadata. Those fallback settings are
approximate and should not be treated as historical evidence.

STR records cover consecutive calendar days: missing days carry a Date and
unknown values, so readers that assign dates by record index stay aligned.
The generator retains up to 365 distinct summaries. If their calendar span
exceeds 3,660 days, it refuses to replace the existing STR rather than
silently discard history or write an unbounded file.
