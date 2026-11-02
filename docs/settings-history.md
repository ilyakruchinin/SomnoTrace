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

OSCAR can record an unknown mode rather than treat the day as absent, and a
normal subsequent import may skip already imported days. Other destinations
may also keep existing settings instead of replacing them. Back up the
consumer's data and consult its recovery or re-import instructions before
attempting to repair history; generating a better STR alone may not update
an existing consumer record. These consumer recovery paths have not been
tested end-to-end.

## New captures and recovery

- A successful AS11 Get RPC is saved with its session and noon-day provenance.
  Cached device settings are marked separately and are not historical proof.
- For multiple sessions in one day, their exported settings must agree; if a
  session is missing a verified capture or the values differ, settings for
  that day are unknown. A capture reflects the Get response after therapy,
  not a guarantee that settings never changed during the session.
- An imported `STR.edf` supplies settings only from each record's own fields.
  Missing fields are not filled from `CurrentSettings.json`. The importer
  requires a machine serial and preserves conflicting source files and daily
  snapshots instead of overwriting another machine's data. A later local
  session makes the imported day's snapshot stale until re-imported.
- **Import a fresh, genuine ResMed card export for recovery.** The import
  treats its STR records as authoritative; the EDF format does not prove who
  generated the file. Importing an older SomnoTrace-generated STR can reintroduce
  previously fabricated history.
- AS10 imports preserve supported CPAP, AutoSet, bilevel, VAuto, ASV and common
  settings as physical values from the card's own EDF header, retaining AS10
  mode and enum values and labels. Unsupported fields (including some ST and
  iVAPS settings), unknown units and unrepresentable values remain unknown.
  This path has synthetic end-to-end tests but has not yet been checked against
  a real AS10 card or an actual OSCAR import.

Settings also remain unknown if the export's device identity or a relevant
session's identity cannot be verified. `SETTINGS/CurrentSettings.json` keeps
its existing current-settings fallback behavior and does not contain
SomnoTrace's internal provenance metadata. The
separate issue of missing calendar days and STR record-index alignment is not
addressed by this change.
