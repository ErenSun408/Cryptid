# Monocypher

This directory vendors an unmodified source subset of Monocypher, used for
Ed25519 signatures, X25519 key exchange and Elligator2 (P5.14).

- Upstream: https://monocypher.org/
- Release: `4.0.2`
- Download archive: `https://monocypher.org/download/monocypher-4.0.2.tar.gz`
- Archive SHA-512 (matches the published `monocypher-4.0.2.tar.gz.sha512`):
  `bf275d4c53ff94af6cdc723a4e002e9f080f4d1436c86c76bb37870b34807f1d7b32331d8ff8a1aeb369e946f3769021e03e63efac25b82efc5abf54dc084714`
- Archive SHA-256:
  `38D07179738C0C90677DBA3CEB7A7B8496BCFEA758BA1A53E803FED30AE0879C`
- License choice for usb-vault: 2-clause BSD (see `LICENCE.md`)

`src/monocypher.c`, `src/monocypher.h`, `src/optional/monocypher-ed25519.c`,
`src/optional/monocypher-ed25519.h` and `LICENCE.md` were copied
byte-for-byte from the release archive. The optional module provides the
SHA-512 based Ed25519; the core `crypto_eddsa_*` functions use BLAKE2b and are
not Ed25519.
