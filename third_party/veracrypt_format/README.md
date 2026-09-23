# Patched VeraCrypt FormatDLL boundary

Current P4.6 product SHA-256: `D099EA1D153320E00B16B3A6004231D74639FBB967EDDEFA16D75588DB38500D`. Apply the USB Vault, geometry, partition and inplace patches in that order to the pinned upstream source, and copy `UsbVaultGeometry.inl`, `UsbVaultPartition.inl` and `UsbVaultInplace.inl` beside the SDK source. The application and packaging allowlists use this reviewed binary; earlier hashes below are historical.

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

The geometry-only packaging binary described in this historical section included the read-only geometry export.
Apply the existing patch to the pinned source tree, then apply
`veracrypt-1.26.29-geometry.patch` and copy `UsbVaultGeometry.inl` beside
`src/FormatDLL/VeraCryptFormatSDK.cpp`. Build x64 Release Crypto and Zip,
generate `Format/FormatCom_h.h` from `FormatCom.idl` with MIDL
(`/env x64 /h FormatCom_h.h /iid FormatCom_i.c`, working directory `src/Format`),
then build FormatDLL. Keep the reviewed DLL in local dependency storage,
separate from disposable build outputs.

Previous geometry-only SHA-256: `416C9DABB1ABD83F4A0A36ACC1BAB233727B8E359A388507AD5E6DCF6363F184`.
The current distribution includes all four patches, all three includes, this README,
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

This export was introduced as a separate candidate and promoted in P4.4 after source and isolated-device review. Apply
`veracrypt-1.26.29-partition.patch` after the existing two patches and copy
`UsbVaultPartition.inl` beside the SDK source. Build with the same pinned
upstream, Crypto, Zip and generated MIDL header. The patch adds one export;
it does not change the public options structure or replace existing file APIs.
Future distribution must include the third patch and the new include as well
as the existing source/notice obligations. Any future replacement requires a separate source and binary review before changing local dependency storage and the fixed allowlists.

Candidate SHA-256 after the NTFS lock fix:
`878425F46FDEAD0ADE7E3E664F2BA6A445D9CA6DBAE260C93AA22A46FA70F3AA`.
The earlier RAW-only candidate was `94D37A2D8C70707190076680587833CFA3C26ABD3426629EFAE9F31DA0225309`.
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
identified RAW skips that control. NTFS must grant it before locking. NTFS
metadata is queried before locking, its boot signature/sector size/serial are
checked, and its complete first sector must be unchanged when read through the
same locked DASD handle. Filesystem information calls on locked NTFS returned
ERROR_NOT_READY in integration testing, including the NTFS metadata FSCTL.
Dismount follows a
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

### NTFS/helper validation after the lock fix

The final candidate above passed `--run-owned-vhd-helper` on a fresh MBR/512
NTFS VHD: refusal and throwing confirmation preserved the first 64 KiB and
filesystem identity, while accepted confirmation completed helper format,
mount/write/unmount/remount/readback. The separate `--run-owned-vhd-handle`
regression also passed RAW guards, contention, cancellation, and normal/hidden
file-container formats and mounts. Both returned zero and removed their own
successful fixtures. Earlier failed fixtures are retained detached as evidence.
No diagnostic logging is present in this candidate. This supersedes the earlier
NTFS/helper validation gap, but does not validate product card authentication,
physical disks, GPT/4Kn, in-place encryption or pause/resume. At the end of that historical NTFS experiment, production storage and the allowlist still used the geometry-only DLL. The P4.4 promotion supersedes that deployment state.


### P4.4 product integration and verification limits

The reviewed partition include matches the pinned build input. The product now binds explicit erase consent to the authenticated card session and retained target, removes verified original volume access paths before formatting, stores encrypted partition identities and mounts through the dedicated provider API. The same reviewed binary passed the owned NTFS helper/provider round trip including original drive removal. File-container regression evidence from the earlier reviewed-binary run remains applicable.

The final combined acceptance executable now also offers `--run-owned-vhd-session`: synthetic authenticated roots, real KDF and encrypted index storage inside a fresh fixture directory, real helper/driver, cross-session readback and removal cleanup. This is not physical-card authentication. After an earlier UAC cancellation, the user explicitly requested a new elevation. All three final owned-VHD modes (session, helper and handle) exited zero with empty stderr and removed their fixtures. The combined session and quiet-prewrite cancellation assertions passed, as did original-access removal and normal/hidden file-container regression. This closes the P4.4 single-partition MVP gate, not physical-card or physical-disk qualification. Build, focused regression, WebView integration and strict analysis passed; see DEVELOPMENT_LOG.md for exact evidence and limits. No new installer was produced.


## NTFS in-place conversion boundary (P4.5)

The fourth patch includes `UsbVaultInplace.inl` in the separately distributed DLL. Its five synchronous exports are mirrored in `src/volume_creation/veracrypt_format_abi.h`: open an authorized retained handle in begin/resume mode, encrypt a block, save an authenticated range, finalize, and close. One operation lives in each helper process. The application never loads the DLL. Reproduction review applied all four patches to the pinned source and compared all modified files and includes with the reviewed build input, allowing only UTF-8 BOM and newline differences.

Begin requires a clean ordinary NTFS MBR/512 partition, exactly 64 binary password bytes, the existing system/boot/paging/BitLocker exclusions and same-handle identity revalidation. The DLL asks Windows to shrink the NTFS filesystem by the 256 KiB header reserve without changing the partition table. An unshrinkable filesystem fails explicitly; there is no destructive-format fallback or tail-cluster relocation implementation. AES-XTS, SHA-512 and PIM 0 remain fixed. Data moves backwards by 128 KiB using complete 64 KiB locked buffers. Preparation conceals the original NTFS start until finalization; this is not a secure erase guarantee.

Normal pause finishes the current block, flushes data, writes a freshly salted authenticated backup header with its continuous encrypted suffix, then flushes the header. Only that successful result may create a paused permit inside the encrypted fixed-size container index. Resume requires fresh card authentication, the exact saved partition identity, the persisted paused permit, a RAW structural gate, an exclusive handle, the concealed NTFS boot signature and authenticated in-place backup header. RAW filesystem metadata can be unknown after preparation; this separate resume chain does not loosen initial NTFS/BitLocker checks. Wrong passwords, unsupported layouts and ordinary RAW volumes are rejected. Progress supplied by a page is never accepted.

The product consumes the paused permit into an uncertain record before another write and rejects mounting or forgetting incomplete entries. Errors, revocation, a failed state save, power loss or forced process termination do not promise safe resume. Successful completion writes the primary header, flushes, restores a normal record and permits ordinary mounting. Header reserve areas and unused hidden-header slots are randomized; no password-role or hidden-existence marker is added. The v4 encrypted pending record must not be edited by older clients that do not support it; completed records return to the existing normal representation. Helper protocol v8 requires matching application/helper versions.

Owned-VHD tests exercise real KDF, encrypted index persistence, a new authenticated session, independent elevated helper processes, wrong-key refusal, pause, resume, original-file byte comparison, and driver cleanup. Synthetic authenticated roots stand in for physical-card authentication. Physical cards, real disks, GPT/4Kn and broader compatibility remain P4.6 work; arbitrary interruption recovery is outside this MVP. Source/license distribution obligations above still apply. No installer delivery is implied by promotion of the local dependency.


## P4.6 GPT boundary

`VeraCryptFormatPartitionGptBinary` has the legacy partition-format arguments followed by `const unsigned char* partitionId, uint32_t idBytes`. `VeraCryptInplaceOpenGptBinary` similarly appends those arguments to the legacy in-place-open ABI. Exactly 16 nonzero GUID bytes are required and copied before use. GPT Basic Data type, allowed attributes (zero or NO_DRIVE_LETTER), GUID, offset, length, and 512-byte geometry are checked on the retained handle before and after locking. Legacy exports remain MBR-only. The helper requires the GPT export before confirmation or access-path removal; missing exports never trigger an MBR fallback.

The application persists GPT GUIDs only in authenticated version-five index slots and carries them in version-nine helper IPC. Existing MBR index records and DLL exports retain their formats. Do not use older clients to rewrite an index containing GPT records. Production partition tables are never rewritten by these exports.

Seven elevated owned-VHD scenarios passed in `out/p46-backend-20260922-200226`, including GPT format, direct/helper/session pause and authenticated resume, original-file byte comparison, MBR regression, and normal/hidden file containers. Source reproduction is recorded in `out/p46-source-proof`. Physical hardware, real card authentication, hotplug, and cross-reboot coverage remain outstanding. The previous P4.5 product digest was `242EAB54B82ED682150AE5E7A171A45C4A227753D3F266D9E95791622F0F5382`; it is historical, not the current allowlist.
