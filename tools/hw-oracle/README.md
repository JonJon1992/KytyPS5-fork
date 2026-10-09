# AMD hardware oracle

Optional standalone instruction-semantics probe for a local AMD GPU. Kyty does
not import, build, or launch it during normal emulator operation.

See [the usage guide](../../docs/HW-ORACLE.md) for requirements, floating-point
controls, the register contract, examples, and limitations.

## Source and license

Imported from [AnyPS5](https://github.com/JonJon1992/AnyPS5/tree/72cf6c3c5a299f645f74a13945670ba4fb8b3cb8/tools/hw-oracle),
commit `72cf6c3c5a299f645f74a13945670ba4fb8b3cb8`, on 2026-10-09:

- `tools/hw-oracle/hw_oracle.py`
- `tools/hw-oracle/oracle.c`
- `tools/hw-oracle/template.s`
- `tools/hw-oracle/test_hw_oracle.py`
- `docs/dev/HW_ORACLE.md` (adapted as `docs/HW-ORACLE.md`)

These sources are distributed under the donor's GNU GPL version 2 license,
copied verbatim to [LICENSE](LICENSE). Original authorship remains with the
AnyPS5 contributors.

Local adaptations: the default cache directory is `kyty-hw-oracle`; documentation
targets Kyty's shader tests; `examples/` adds an integer smoke probe and calculated
expected output; the offline tests also assemble this probe in wave32 and wave64.
The C runner and assembly template are unchanged from the donor.
