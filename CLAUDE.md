# CLAUDE.md: NG2 Xbox 360 → PC recomp

- Target: the original Ninja Gaiden II (Xbox 360, 2008) `game\default.xex`, supplied by the user. Never commit or upload anything in `game\`.
- Recompiler: ReXGlue SDK (`tools\rexglue-sdk`), with the project in `project\`. Reference implementation: `reference\ninjas-dawn`.
- Run `. .\env.ps1` before running tools in a shell.
- REA: follow `skills\reverse-engineer-anything\SKILL.md`. REA cannot load XEX/Xbox 360 PPC, so use Ghidra + XEXLoaderWV for guest code. Use REA for the recompiled x64 PE and other Windows artifacts.
- Record every finding in `notes\findings.md` with its evidence (REA Evidence ID, Ghidra address, or codegen log) and mark it observed / inferred / unknown.
