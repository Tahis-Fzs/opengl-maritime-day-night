# OpenGL Maritime Day/Night Lab Project

This repository contains a C++ **OpenGL/GLUT** simulation of a maritime environment with automatic day/night cycling, hierarchical scene geometry, and cinematic camera framing.

The project demonstrates sky/ocean rendering, lighthouse spotlighting, sailing ships with deck crew, daytime birds, distant horizon ships, and smooth lighting transitions driven entirely by timers (no keyboard day/night toggle).

> **Note:** This repository is intended for coursework and educational use (Computer Graphics lab).

---

## Project Overview

The scene is a single-file OpenGL 1.x + GLUT application that builds a coastal maritime view: wavy ocean mesh, rocky lighthouse island, main sailing ship, background ships, and an automatic day↔night blend every **15 seconds**.

### Main scene elements

| Element | Description |
|---|---|
| Sky / atmosphere | Gradient sky, daytime clouds, night stars and moon, smooth lighting blend |
| Ocean | Dense wavy triangle-strip mesh with foam streaks and subtle horizon curvature |
| Lighthouse | Rocky shore / island, tapered tower, rotating spotlight + cone mesh at night |
| Main ship | Hierarchical hull / mast / sails with deck crew (`drawCrew`) |
| Birds | Daytime flock animation (`drawBirds`) |
| Distant ships | Horizon silhouettes for depth (`drawDistantShips`) |
| Camera | Automatic cinematic orbit/track (no manual controls) |

---

## Features

1. Automatic **Day/Night** switching every **15 seconds**
2. Smooth sky, lighting, and environment transition via `gDayNightBlend`
3. Denser **wavy ocean mesh** with foam streaks
4. Lighthouse on rocky shore with rotating **spotlight + cone mesh** at night
5. Main sailing ship with **deck crew**
6. **Birds** during the day
7. **Multiple distant ships** on the horizon
8. Automatic **cinematic camera** for demo framing
9. **Double-buffered** rendering (`GLUT_DOUBLE`)

---

## Repository Structure

```text
opengl-maritime-day-night/
├── README.md
├── main.cpp                 # Full scene, animation, lighting, callbacks
├── .gitignore
└── .vscode/
    ├── tasks.json           # Build+run task for VS Code / Cursor
    └── launch.json
```

Compiled binaries (`maritime_sim`, `maritime_lab`) are ignored by Git and should be rebuilt locally.

---

## Requirements

| Item | Value |
|---|---|
| Language | C++17 |
| API | OpenGL 1.x + GLUT |
| Platform | macOS (primary) |
| Compiler | `clang++` (Apple Command Line Tools / Xcode) |
| Frameworks | `-framework OpenGL` · `-framework GLUT` |

---

## Build and Run (macOS)

### Terminal

From the project folder:

```bash
clang++ -std=c++17 main.cpp -o maritime_sim \
  -isystem /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1 \
  -framework OpenGL -framework GLUT
./maritime_sim
```

If the compiler reports missing standard C++ headers (for example, `cmath` not found), install/reinstall Apple Command Line Tools:

```bash
xcode-select --install
```

### VS Code Task

Use the provided task:

- `OpenGL/GLUT: build+run active file`

This task compiles with:

- `-framework OpenGL`
- `-framework GLUT`

---

## Controls and Behavior

- The simulation starts in daytime; transitions are driven only by the timer.
- Every **15 seconds**, the target toggles between day and night.
- Transitions are smoothed over several seconds via `gDayNightBlend`.
- The main ship **drifts horizontally** and **rocks** on the waves; background ships sway gently.
- The lighthouse beam **spins faster as night increases**; the cone is drawn only when night blend is nonzero.
- **Birds** fly across the sky when the scene is mostly day.
- Camera is automatic (no manual controls), tuned to keep lighthouse, beam, water, and ships in frame.

---

## Modular Draw Functions

Lab organization of the scene into draw modules:

| Function | Role |
|---|---|
| `drawSkybox()` | Gradient sky, sun/moon, stars, daytime clouds |
| `drawOcean()` | Wavy triangle-strip mesh grid |
| `drawLighthouse()` | Terrain + tower |
| `drawShip()` | Hierarchical ship (calls `drawCrew`) |
| `drawCrew()` | Simple deck figures |
| `drawBirds()` | Daytime flock animation |
| `drawDistantShips()` | Horizon silhouettes |

Supporting helpers:

- `drawLighthouseBeamCone()` — night spotlight cone
- `setupLighting()` — day/night lighting setup

---

## Technical Notes

- Callbacks: `glutDisplayFunc`, `glutIdleFunc`, `glutTimerFunc`
- Window: double-buffered (`GLUT_DOUBLE`)
- Day/night interval: `kCycleIntervalSec = 15.0f`
- Transition duration: `kTransitionDurationSec = 4.0f`
- Primary source file: `main.cpp`

---

## Clone

```bash
git clone https://github.com/Tahis-Fzs/opengl-maritime-day-night.git
cd opengl-maritime-day-night
```

---

## License

Choose a license before public reuse if needed. For a course lab, leaving the repository unlicensed (or adding MIT) is typical.

---

## Disclaimer

This software is for educational / Computer Graphics lab purposes only.
