# GitHub Copilot Instructions

A digest of [`AGENTS.md`](../AGENTS.md), which is authoritative and wins on any conflict. It is
repeated here because Copilot Chat on GitHub.com reads this file but not `AGENTS.md`.

-   **Project:** SKSE plugin for DirectX 11 graphics on Skyrim SE/AE/VR, a fork of Community Shaders. The runtime identity stays `CommunityShaders` (DLL, settings path, log); the public name is "Open Shaders".
-   **PRs and git:** Conventional Commits `type(scope): description`, title <= 50 chars, body wrapped to 72, target `dev` (never `main`). Never force-push shared branches, hand-create `v*` tags, or bump the `CMakeLists.txt` version. Merge upstream, never cherry-pick.
-   **Comments:** none by default. Write one (2 lines at most) only when the WHY is non-obvious and a future edit would silently break without it. Describe present code only. Doxygen on public declarations.
-   **Code:** minimal churn (touch only what the change requires), no placeholders, reuse `Util::SetResourceName`, `Util::GetGameSettingValue` and the cached `globals::game::*` pointers. No feature may depend on a non-core feature.
-   **Graphics:** name every D3D11 resource with `Util::SetResourceName`; wrap every render pass entry point in `CS_GPU_PASS("Feature::Pass")`.
-   **VR:** non-VR is the primary path; keep VR divergence minimal (`globals::game::isVR` in C++, `#if defined(VR)` in HLSL).
-   **Build (Windows):** `./BuildRelease.bat ALL`. Linux/WSL can only cross-compile with `Linux-ClangCL`; shaders need Windows.
-   **Testing:** verify runtime-affecting changes in game via devbench on VR plus one flat variant (SE or AE).

<<<<<<< HEAD
Detailed workflows are under `docs/development/`.
||||||| 12b261724
**For comprehensive development guidance, architecture details, and complete build instructions, see:**

-   **`.claude/CLAUDE.md`** - Concise guide to build tooling, project conventions, and release rules
-   **`AI-INSTRUCTIONS.md`** - Quick reference that also points to .claude/CLAUDE.md

This file provides Copilot-specific guidance while avoiding duplication of the comprehensive documentation above.

## Project Overview

SKSE64 plugin providing modular DirectX 11 graphics enhancements for Skyrim SE/AE. Features runtime shader compilation, 25+ graphics features, and cross-platform Skyrim variant support.

## Environment and Build Essentials

### Windows-Only Requirements

-   Visual Studio Community 2026 with "Desktop development with C++" workload (VS 2022 via the `ALL-VS2022` preset)
-   CMake 4.2+, Git, vcpkg with VCPKG_ROOT environment variable, Windows SDK
-   **NEVER CANCEL BUILDS**: 45-60 minutes build time, 15-30 minutes shader validation

### Linux/WSL Limitations

-   **Cannot build or validate shaders** - requires Windows fxc.exe compiler
-   **Limited to**: Code review, documentation, Python tooling only

### Primary Build Command (Windows)

```powershell
./BuildRelease.bat        # Shipping build, ALL preset (universal SE/AE binary)
./BuildDevFast.bat        # Fastest iteration: DLL only
```

### Essential Repository Setup

```bash
git clone https://github.com/community-shaders/skyrim-community-shaders.git --recursive
cd skyrim-community-shaders
git submodule update --init --recursive  # If not cloned with --recursive
```

## GitHub Copilot Role Guidelines

### Act as Expert

**Graphics programming and Skyrim modding expert** with deep knowledge of:

-   DirectX 11/12 rendering pipelines and performance optimization
-   SKSE plugin development and CommonLibSSE-NG runtime targeting
-   HLSL shader development and GPU compute programming
-   ImGui interface design and Skyrim engine integration

### Proactive Issue Identification

**Flag potential problems before they occur:**

-   **Performance Impact**: Graphics features affect rendering performance - suggest user toggles
-   **Runtime Compatibility**: Warn about SE/AE compatibility issues, suggest `REL::RelocateMember()` patterns
-   **Buffer Conflicts**: Highlight GPU register conflicts, recommend hlslkit buffer scanning
-   **Security Risks**: Validate user input, prevent DirectX crashes from malformed configurations

### Code Quality Standards

-   **Complete Solutions**: No TODO/FIXME placeholders - provide fully functional code
-   **Performance Conscious**: Always consider GPU workload and user experience
-   **Cross-Platform**: Ensure changes work across SE/AE variants using runtime detection
-   **Error Handling**: Include proper resource management and graceful degradation

## Architecture Quick Reference

### Core Systems Access (`src/Globals.h`)

```cpp
// Feature registry - all graphics features globally accessible
globals::features::lightLimitFix
globals::features::screenSpaceGI
globals::features::volumetricLighting
// ... 25+ more features

// Core systems
globals::state         // Feature lifecycle management
globals::shaderCache   // Runtime shader compilation
globals::d3d::*       // DirectX 11 device/context access
```

### Feature Development Pattern

1. Inherit from `Feature` class (`src/Feature.h`)
2. Implement `DrawSettings()`, `LoadSettings()`, `SaveSettings()`
3. Add shaders to `features/YourFeature/Shaders/`
4. Register in `globals::features` namespace
5. Use template in `docs/new-feature-template/` as starting point

### Common Development Commands

```bash
# Fast shader deployment (dev iteration - no DLL build)
# See docs/development/shader-workflow.md and docs/development/vscode-setup.md
cmake --build ./build/ALL --target COPY_SHADERS

# Shader validation (targeted testing recommended during development)
cmake --build ./build/ALL --target prepare_shaders
hlslkit-compile --shader-dir build/ALL/aio/Shaders/[specific-feature] --output-dir build/ShaderCache --config .github/configs/shader-validation.yaml

# Pre-commit validation
pre-commit run --all-files
```

## Key Differences from .claude/CLAUDE.md

This file focuses on Copilot-specific guidance while `.claude/CLAUDE.md` provides:

-   Build wrappers and shader refactor verification (`tools/verify-shader-refactor.ps1`)
-   Feature release stages (Alpha / Beta / Unreleased) and versioning rules
-   D3D11 resource naming, UI constant, and i18n conventions
-   Commit types and release branch rules

Refer to `.claude/CLAUDE.md` for project rules not covered in this Copilot-specific summary.
=======
**For comprehensive development guidance, architecture details, and complete build instructions, see:**

- **`.claude/CLAUDE.md`** - Concise guide to build tooling, project conventions, and release rules
- **`AI-INSTRUCTIONS.md`** - Quick reference that also points to .claude/CLAUDE.md

This file provides Copilot-specific guidance while avoiding duplication of the comprehensive documentation above.

## Project Overview

SKSE64 plugin providing modular DirectX 11 graphics enhancements for Skyrim SE/AE. Features runtime shader compilation, 25+ graphics features, and cross-platform Skyrim variant support.

## Environment and Build Essentials

### Windows-Only Requirements

- Visual Studio Community 2026 with "Desktop development with C++" workload (VS 2022 via the `ALL-VS2022` preset)
- CMake 4.2+, Git, vcpkg with VCPKG_ROOT environment variable, Windows SDK
- **NEVER CANCEL BUILDS**: 45-60 minutes build time, 15-30 minutes shader validation

### Linux/WSL Limitations

- **Cannot build or validate shaders** - requires Windows fxc.exe compiler
- **Limited to**: Code review, documentation, Python tooling only

### Primary Build Command (Windows)

```powershell
./BuildRelease.bat        # Shipping build, ALL preset (universal SE/AE binary)
./BuildDevFast.bat        # Fastest iteration: DLL only
```

### Essential Repository Setup

```bash
git clone https://github.com/community-shaders/skyrim-community-shaders.git --recursive
cd skyrim-community-shaders
git submodule update --init --recursive  # If not cloned with --recursive
```

## GitHub Copilot Role Guidelines

### Act as Expert

**Graphics programming and Skyrim modding expert** with deep knowledge of:

- DirectX 11/12 rendering pipelines and performance optimization
- SKSE plugin development and CommonLibSSE-NG runtime targeting
- HLSL shader development and GPU compute programming
- ImGui interface design and Skyrim engine integration

### Proactive Issue Identification

**Flag potential problems before they occur:**

- **Performance Impact**: Graphics features affect rendering performance - suggest user toggles
- **Runtime Compatibility**: Warn about SE/AE compatibility issues, suggest `REL::RelocateMember()` patterns
- **Buffer Conflicts**: Highlight GPU register conflicts, recommend hlslkit buffer scanning
- **Security Risks**: Validate user input, prevent DirectX crashes from malformed configurations

### Code Quality Standards

- **Complete Solutions**: No TODO/FIXME placeholders - provide fully functional code
- **Performance Conscious**: Always consider GPU workload and user experience
- **Cross-Platform**: Ensure changes work across SE/AE variants using runtime detection
- **Error Handling**: Include proper resource management and graceful degradation

## Architecture Quick Reference

### Core Systems Access (`src/Globals.h`)

```cpp
// Feature registry - all graphics features globally accessible
globals::features::lightLimitFix
globals::features::screenSpaceGI
globals::features::volumetricLighting
// ... 25+ more features

// Core systems
globals::state         // Feature lifecycle management
globals::shaderCache   // Runtime shader compilation
globals::d3d::*       // DirectX 11 device/context access
```

### Feature Development Pattern

1. Inherit from `Feature` class (`src/Feature.h`)
2. Implement `DrawSettings()`, `LoadSettings()`, `SaveSettings()`
3. Add shaders to `features/YourFeature/Shaders/`
4. Register in `globals::features` namespace
5. Use template in `docs/new-feature-template/` as starting point

### Common Development Commands

```bash
# Fast shader deployment (dev iteration - no DLL build)
# See docs/development/shader-workflow.md and docs/development/vscode-setup.md
cmake --build ./build/ALL --target COPY_SHADERS

# Shader validation (targeted testing recommended during development)
cmake --build ./build/ALL --target prepare_shaders
hlslkit-compile --shader-dir build/ALL/aio/Shaders/[specific-feature] --output-dir build/ShaderCache --config .github/configs/shader-validation.yaml

# Pre-commit validation
pre-commit run --all-files
```

## Key Differences from .claude/CLAUDE.md

This file focuses on Copilot-specific guidance while `.claude/CLAUDE.md` provides:

- Build wrappers and shader refactor verification (`tools/verify-shader-refactor.ps1`)
- Feature release stages (Alpha / Beta / Unreleased) and versioning rules
- D3D11 resource naming, UI constant, and i18n conventions
- Commit types and release branch rules

Refer to `.claude/CLAUDE.md` for project rules not covered in this Copilot-specific summary.
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
