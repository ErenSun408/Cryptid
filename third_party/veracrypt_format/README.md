# Patched VeraCrypt FormatDLL boundary

P1.4 uses VeraCrypt 1.26.29 source pinned at commit `b48e31f5`. The source is
not copied into this repository. Apply `veracrypt-1.26.29-usb-vault.patch` to
that exact upstream tree, build the x64 Release `FormatDLL` target, and provide
the resulting DLL through CMake's `USB_VAULT_FORMAT_DLL` cache path.

Keep reviewed local binaries outside build trees, for example in
`out/local-dependencies/VeraCryptFormat.dll`. From an x64 MSVC developer
prompt, configure each required preset explicitly:

```bat
tools\configure-preset.cmd dev -DUSB_VAULT_FORMAT_DLL:FILEPATH="%CD%/out/local-dependencies/VeraCryptFormat.dll"
tools\configure-preset.cmd package-release -DUSB_VAULT_FORMAT_DLL:FILEPATH="%CD%/out/local-dependencies/VeraCryptFormat.dll"
```

Use the same argument for other presets. CMake stores it in each build cache;
repeat it after deleting that build directory. Do not use another build tree
or a disposable spike output as the dependency source.

`out/local-dependencies/` is ignored local input storage, not a build cache.
Preserve its reviewed files and SHA-256 manifest during cleanup. The local
`vendor/ViKey.dll` and `prerequisites/` installers can also be kept there and
passed explicitly to `tools/package-p2-development.ps1`. The packaging script's
existing digest and signature checks still apply. Preserve the patched
VeraCrypt source tree separately for rebuilding and source distribution;
keeping a DLL alone does not replace that source requirement.

The patch has three narrowly scoped purposes:

- expose the already implemented hidden-volume geometry fields;
- preserve the safe hidden-volume size calculation inputs validated by Spike
  3b; and
- add `VeraCryptFormatBinary`, which accepts an explicit password length so a
  64-byte KDF result containing NUL bytes is not truncated by `strlen`.

The patch also carries the already reviewed headless password-change export
from Spike 3b. P1.4 does not expose that operation through its IPC protocol; it
remains packaged for the later A-container re-key workflow required by N-06.

The main product never links or loads this DLL. Only the separately distributed
`usb_vault_volume_creator.exe` loads it, preserving the process and licensing
boundary described by L-01. The patched FormatDLL and the helper distribution
must satisfy VeraCrypt/TrueCrypt license and source-offer obligations; final
distribution still requires the planned legal review.
