# DXVK runtime and DXGI overrides

Sogen builds the host GPU bridge and guest Vulkan shim. Its root-provisioning
script downloads DXVK separately; the analyzer build does not compile DXVK.
The current local Windows root contains Microsoft D3D11/DXGI files, so the
Dawn launcher selects a separate DXVK runtime with exact guest file mappings.

The authorized source is `D:/Dawn/refs/dxvk`, commit
`7df3596e`. `build.cmd` builds x64 and x86 D3D11/DXGI under `built/<arch>/src`,
using VS2022, Meson, optimization and full PDB output. Commands:

```bat
build.cmd
build.cmd x64
build.cmd x86
```

Both architectures built successfully. Each DLL's CodeView GUID and age
matched its PDB. Sogen's Vulkan shims were built for both architectures and
checked the same way. Guest copies are staged outside the reference tree at
`D:/Sunrise/D2/sogen/graphics/dxvk-7df3596e-pdb-20260915`.

| Architecture | DLL | PDB GUID | Age |
| --- | --- | --- | ---: |
| x64 | dxgi.dll | 467ae7c3-804e-4279-af7a-35d7335d017e | 1 |
| x64 | d3d11.dll | 498a0e58-10f2-4ef6-b823-75db8ee06fca | 1 |
| x64 | vulkan-1.dll | 5e6dd37d-9d79-4240-9c7d-023952f08ee6 | 1 |
| x86 | dxgi.dll | c965ddb5-1a0f-4080-9597-869644029acb | 1 |
| x86 | d3d11.dll | 3dfe451b-a7a6-4fcf-82e8-c17c57143f02 | 1 |
| x86 | vulkan-1.dll | 4a19a627-67dd-4fbc-9372-ce716344882f | 1 |

## Selection

The analyzer already accepts repeated `-p GUEST_FILE HOST_FILE` mappings.
For example, select a DXGI build without replacing the Windows root file:

```text
-p c:\windows\system32\dxgi.dll D:\runtime\x64\dxgi.dll
-p c:\windows\syswow64\dxgi.dll D:\runtime\x86\dxgi.dll
```

An exact system-directory mapping does not override an application-local
or explicitly named DLL. The Dawn launcher also maps the application's
`dxgi.dll`, `d3d11.dll`, and `vulkan-1.dll` names. It selects x64 files for
System32 and x86 for SysWOW64. Mappings are installed before KnownDll section
construction, so file probes, SEC_IMAGE metadata and mapped bytes use the
same selected files.

PatchScanner's `scripts/run_sogen_destiny.py` now defaults fresh runs to DXVK.
`--graphics-runtime DIR` selects a staged runtime; `--dxgi FILE` independently
overrides DXGI for the target architecture. `--renderer windows` explicitly
selects the unmodified root. Required exports, PE architecture, file hashes and
PDB identities are checked before launch. A wrong-architecture override was
rejected; a custom Microsoft DXGI selection preserved the chosen DXVK D3D11
and Vulkan mappings. This is configuration proof, not presentation proof.

Every run records the selected files and hashes in `run.json` and prints them
at the start of the console log. DXVK's own logs go to that run's `dxvk-logs`
directory. A fresh actual-game run with `-e` and `-v` is in progress; it has not
yet established a rendered frame or game boot.

## Checkpoints

Old checkpoints contain their original loaded DLLs and KnownDll metadata.
They do not acquire new renderer defaults. File mappings are not serialized,
so replay must preserve the matching external configuration. The launcher
rejects changing a checkpoint's renderer.

Live host Vulkan objects are not serialized by the GPU bridge. DXVK checkpoint
replay is currently refused by the launcher. A saved file alone does not
establish valid graphics restoration.

Local build and identity evidence is retained in PatchScanner's
`artifacts/source-crosschecks/dxvk-build.log`, `dxvk-runtime-identity.json`,
and `dxvk-runtime-validation.json`.
