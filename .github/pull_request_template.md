## Summary

<!-- What changed and why? -->

## Validation

- [ ] `make check`
- [ ] Firmware builds for every board with no new warnings (`make dist`), if firmware changed
- [ ] Checked on hardware (screenshot via `tools/devctl.py shot` for UI changes), or why that doesn't apply
- [ ] Protocol changes update the parser, the agent, the tests and `docs/protocol.md` together
- [ ] No private data (passwords, hostnames, addresses, MAC addresses) in code, docs or screenshots

## User impact

<!-- What changes on the screen or on the Spark? Any reinstall or reflash needed? -->

<!--
New device? Keep this section and fill it in; otherwise delete it.
See CONTRIBUTING.md, "Adding a new device".
-->
## New device

Board: <!-- maker, model, link -->  ·  Issue: <!-- #123 -->

- [ ] `firmware/boards/<id>/` passes `tools/check_boards.py`, and the board is listed in `firmware/boards/README.md`
- [ ] The board README says where the pins and register values came from, and no vendor code is copied without a license that allows it
- [ ] `devctl.py status` shows `board=<id>` (output attached)
- [ ] `devctl.py demoshot` screenshot attached
- [ ] Photo of the physical screen attached: upright, not mirrored, not inverted
- [ ] Rotations: <!-- which work; which answer "not supported" -->
- [ ] Wi-Fi: `screen=online reason='via WI-FI'` with live values
- [ ] Five cold plug-ins, each answered
- [ ] Installer on a Spark (`sudo ./install.sh --board <id>`), or "no Spark" said here
- [ ] Existing boards untouched; the reference board is still the default
