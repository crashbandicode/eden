# Source identity

Publication snapshot: protocol-v4 companion from the October 30, 2026 v8 source
package. The source package manifest SHA256 is
4f17798df5723a9f5a3040f4e95881a606fb49c0d19d7d8ab8f473454277c0fc.
Only publication documentation and MANIFEST.sha256 changed during import into
Eden; runtime and test sources retain the validated package bytes. MANIFEST.sha256
now binds this published tree. The original package's historical provenance follows.

Version 0.1.0-lan-fixture.20261016. cmd/internal and diagnostic sources are exact
copies of the privately validated LAN endpoint implementation on 2026-10-16.
MANIFEST.sha256 binds the files. No older bin/nsolan is included.
Original validated binary SHA256:
41a0da889d95adf9e36c34aa94acbab92860aba026fb0e0dcfe49b40be6f7dd1.
Fresh source builds have their own identity and receipt.

Vendored Nextendo NEX commit: 4f1ca895ad33e87bd5fa1a531842e8809d62586c.
Original codeload archive SHA256:
0ecc8d3a2dadc582e0d26f88a0a0ae533d1d5069dbfda3cccb59837e470a08df.
Original provenance and PolyForm Shield license are preserved. go.mod/go.sum pin
other modules. Recipe: Go 1.27.2, CGO disabled, trimpath, buildvcs=false.

The Smash server was a protocol reference, not a vendored source dependency:
NextendoNetwork/super-smash-bros-ultimate fbd45364519d7e41045d32468848ae49e05d0910.
No captured initialization replies are distributed. Its repository license does
not establish redistribution rights for captured game responses.
