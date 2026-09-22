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

## Read-only geometry boundary (P3.25)

The reviewed packaging binary now includes the read-only geometry export.
Apply the existing patch to the pinned source tree, then apply
`veracrypt-1.26.29-geometry.patch` and copy `UsbVaultGeometry.inl` beside
`src/FormatDLL/VeraCryptFormatSDK.cpp`. Build x64 Release Crypto and Zip,
generate `Format/FormatCom_h.h` from `FormatCom.idl` with MIDL
(`/env x64 /h FormatCom_h.h /iid FormatCom_i.c`, working directory `src/Format`),
then build FormatDLL. Keep the reviewed DLL in local dependency storage,
separate from disposable build outputs.

Reviewed SHA-256: `416C9DABB1ABD83F4A0A36ACC1BAB233727B8E359A388507AD5E6DCF6363F184`.
The distribution includes both patches, the geometry include, this README,
and upstream License/NOTICE. A rebuilt binary must be reviewed before changing
the allowlist; compiler/build metadata may change its digest.

`VeraCryptReadHiddenGeometry` is synchronous and requires SDK initialization.
Its ABI is mirrored in `src/volume_creation/veracrypt_format_abi.h`: two encrypted
512-byte headers, two binary passwords with explicit lengths (1–128 bytes), the
host file length, and a 32-byte output of four `uint64_t` values:

1. Outer encrypted data start, relative to the container file.
2. Outer encrypted data length.
3. Hidden protection start, relative to the container file.
4. Hidden protection length (the header's `hiddenVolumeSize`, not the requested
   creation size, which also includes a trailing filesystem reserve).

The helper must read both primary headers from the same validated,
caller-accessible open file and keep the result bound to that container and
authenticated maintenance session. This function cannot establish file identity
from header bytes. It does not open files, mount volumes, write data, or export
decrypted headers or keys. Only the helper may call it in production; the
explicit integration-test executable also loads the candidate DLL directly.
No geometry is persisted or sent to the page in this slice.

The reader fixes SHA-512/PIM 0, checks both passwords, retains upstream header
CRC validation, requires v5 non-system fully encrypted 512-byte-sector layouts,
and validates bounds by subtraction to prevent overflow. Wrong passwords,
corrupt headers and unsupported layouts share one generic rejection. A valid
32-byte output buffer is zeroed before any failure. Invalid output buffers or
lengths are rejected without dereferencing them. Password copies live in dedicated
locked pages and are burned on every exit; upstream crypto objects are closed
and burned before return. Secrets remain governed by the upstream cryptographic
implementation's existing allocation and locking behavior.

Build `hidden_geometry_integration_tests` with integration tests enabled and
pass an absolute candidate DLL path and the disposable 128/64 MiB fixture from
`decoy_maintenance_integration_tests`. The test is opt-in and not registered in
CTest. It only reads that fixed-password fixture, checks exact geometry, failure
output clearing, wrong passwords, corrupt/swapped headers, embedded-NUL suffixes,
host/header/password/output lengths, and successful reuse after failure. It does
not authenticate a security card or mount a volume.


The protected-space and upload helper use this export through protocol v4. It opens
both headers under caller impersonation, matches file identity and the mounted
protection-header digest, and rechecks mount identity and headers after scanning.
The pinned driver records the last authenticated header digest; for a protected
outer mount this is the hidden protection header. Geometry stays inside the
helper. The allowlist was updated after source comparison against the pinned
upstream and reviewed patches, geometry boundary tests, and authenticated
measurement/upload integration on disposable containers. Full application
hardware acceptance remains a separate requirement.

## Candidate partition-handle boundary (P4.1)

This candidate is **not** the packaged/allowlisted DLL above. Apply
`veracrypt-1.26.29-partition.patch` after the existing two patches and copy
`UsbVaultPartition.inl` beside the SDK source. Build with the same pinned
upstream, Crypto, Zip and generated MIDL header. The patch adds one export;
it does not change the public options structure or replace existing file APIs.
Future distribution must include the third patch and the new include as well
as the existing source/notice obligations. Do not promote the candidate by
copying it into local dependency storage without a separate review.

Candidate SHA-256 from this run:
`94D37A2D8C70707190076680587833CFA3C26ABD3426629EFAE9F31DA0225309`.
The candidate exports `VeraCryptFormatPartitionBinary(HANDLE, uint64_t offset,
uint64_t length, const unsigned char* password, uint32_t passwordBytes,
VeraCrypt_Progress_Callback, void*)`. Initialization is required. The caller
supplies a dedicated synchronous read/write partition handle and exactly 64
password bytes. The handle remains caller-owned; duplicates share its file
position, so the caller must not use or close it concurrently. Close it after
the operation; failure is not a promise that its original filesystem state or
position can be reused. No secret, handle or raw target comes from the page.

The export fixes AES/SHA-512/PIM 0, ordinary FAT quick format, no hidden volume,
MBR recognized non-active IFS partitions and 512-byte logical sectors. It checks
partition offset/length, device partition number, actual length and geometry on
the same object before and after acquiring a filesystem lock. It only accepts
an explicitly identified RAW or NTFS filesystem. RAW has no filesystem bounds
to extend and rejects extended-DASD control on the tested system; only positively
identified RAW skips that control. NTFS must grant it. Dismount follows a
successful lock; every error stops the operation. No path reopen, temporary
drive, pseudo-format or forced shared-open fallback is used by this export.

The core duplicates the held handle and explicitly seeks to partition offset
zero before its first header write. The caller's password copy is held in
locked allocated pages and burned on exit. Completion requires successful
format, flush and unlock. Progress is bounded to 0–100 but is not completion:
only return code 0 proves success. A rejected/throwing callback latches
cancellation; the device quick-FAT branch stops instead of ignoring its final
callback. Abort returns 16 and may leave a partially formatted partition. This
is neither NTFS in-place encryption nor pause/resume. No failure deletes,
reformats or automatically retries a target.

This is a backend I/O contract, **not target authorization**. The future helper
must bind the handle to the authenticated card operation, stable disk/partition
identity and the user's explicit destructive-format confirmation, exclude
system/boot/paging/BitLocker and unsupported targets, and revalidate at the
privilege boundary. Locking alone is not that authorization. Existing legacy
exports still retain their legacy behavior; production device creation must
use the new contract after review, never silently fall back to them.

For isolated verification, put the opt-in `partition_device_integration_tests`
executable beside the candidate and run `--run-owned-vhd-handle` as administrator.
It accepts no existing disk input. Tests cover invalid/non-disk handles,
password length, offset/length/alignment, lock contention with unchanged boundary
sectors, an independent disposable cancellation fixture, progress and retained
caller handles, device format/mount/write/unmount/remount/readback, and existing
normal/hidden file-container format and mount regressions. Final run returned
0 with no retained mounts. MBR/512 RAW VHD was exercised; NTFS input, real disks,
GPT, 4Kn, helper IPC, card authentication, full fault injection and in-place
pause/resume are not validated by this candidate run.
