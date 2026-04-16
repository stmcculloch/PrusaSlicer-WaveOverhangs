# Repository Guidelines

## Agent Expectations
- This repository is a PrusaSlicer fork specifically for implementing and refining the wave-overhangs feature.
- This repository is for source inspection and code changes within the workspace.
- For relevant project context, read `README.md`, `docs/wave-overhangs.md`, and `src/libslic3r/WaveOverhangs.cpp` before making changes related to wave overhangs.
- For wave-overhang-related code, do not add legacy support, compatibility conversions, migration code, or old config-key aliases unless the user explicitly asks for them. This feature has not hit production yet, so breaking changes are acceptable.
- Do not attempt to compile or build the project unless the user explicitly asks for it.
