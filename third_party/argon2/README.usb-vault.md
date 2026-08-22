# Argon2 reference implementation

This directory vendors an unmodified, minimal source subset of the official
P-H-C Argon2 reference implementation.

- Upstream repository: https://github.com/P-H-C/phc-winner-argon2
- Release tag: `20190702`
- Upstream commit: `62358ba2123abd17fccf2a108a301d4b52c01a7c`
- Download archive: `phc-winner-argon2-20190702.zip`
- Archive SHA-256:
  `506A80B90AC3CA8407636F3B26B7FED87A55F2F1275CDE850CD6698903C4F008`
- License choice for usb-vault: Apache License 2.0

The source files and `LICENSE` were copied byte-for-byte from the release
archive. usb-vault builds `src/ref.c` and defines `ARGON2_NO_THREADS`, matching
the reference, single-threaded implementation required by KDF v1 (`p=1`).
