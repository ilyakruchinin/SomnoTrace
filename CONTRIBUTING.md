# Contributing to SomnoTrace

Thanks for your interest in contributing! SomnoTrace is led by
**Ilya Kruchinin** ([@ilyakruchinin](https://github.com/ilyakruchinin)), who
acts as project architect and maintainer. Community contributions are very
welcome.

## License of the project

SomnoTrace is released to the public under the **GNU General Public License
v3.0** with an additional **Section 7(b) attribution requirement** (see
[`LICENSE`](LICENSE) and [`NOTICE`](NOTICE)). By participating you agree your
contributions will be distributed under these terms.

## Contributor License Agreement (required)

Before your first contribution can be merged, you must agree to a Contributor
License Agreement (CLA). This is what allows the project to (a) stay open
source under GPLv3 and (b) preserve the maintainer's ability to offer
commercial licenses later.

- **Individuals:** [`CLA/individual-cla.md`](CLA/individual-cla.md)
- **Contributing on behalf of an employer:**
  [`CLA/corporate-cla.md`](CLA/corporate-cla.md)

You **keep the copyright** to your work. The CLA is a broad **license grant**
(not a copyright assignment) that lets the maintainer sublicense
contributions, including under commercial terms. It also includes a
moral-rights consent and is governed by the law of Victoria, Australia.

### How signing works

When you open your first pull request, the **CLA Assistant** bot will comment
with a link and instructions. Sign in with your GitHub account and confirm
your agreement (typically by posting the comment the bot asks for). The bot
records your GitHub username, the timestamp, and the CLA version, and turns
the PR status check green. You only sign once (until the CLA version changes).

A PR cannot be merged until the CLA check passes.

## Clean-room / third-party material policy

SomnoTrace is a **clean-room** implementation:

- You may study public protocol documentation and reference projects to
  understand **how a protocol works**.
- **Do not copy source code** from other projects into SomnoTrace, even
  MIT-licensed code, unless it is explicitly approved and recorded in
  [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md) with its license.
- Implement functionality in your own words/code based on your understanding
  of the protocol, not by transcribing someone else's implementation.

If your contribution was informed by a specific external source, mention it in
your PR description so it can be acknowledged appropriately.

## Source file headers

Add the standard license header to new source files. See
[`docs/source-header.txt`](docs/source-header.txt) for the template (adapt the
comment syntax to the file's language). Do not remove existing copyright or
attribution notices.

## Pull request guidelines

- Keep PRs focused; one logical change per PR where practical.
- Describe **what** changed and **why**.
- Match the existing code style of the area you're editing.
- Make sure the project builds and any tests/linters pass before requesting
  review.

## Reporting issues

Use GitHub Issues for bugs and feature proposals. For anything involving
personal health data, do not include real patient/medical data in issues or
test fixtures.

## Questions

Open a GitHub Discussion/Issue, or reach the maintainer via
[github.com/ilyakruchinin](https://github.com/ilyakruchinin).
