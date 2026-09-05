# Vendored security backports

- glib 0.18.5: `VariantStrIter::impl_get` passes the out pointer as `&mut p`, backporting gtk-rs-core PR #1343 (RUSTSEC-2024-0429).

