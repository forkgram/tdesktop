# rustgram-media-cache

First isolated Rust port of the streaming media cache map reader in
`Telegram/SourceFiles/media/streaming/media_streaming_reader.cpp`. It parses
contiguous and sparse cached parts as borrowed slices and keeps duplicate-offset
semantics. The source format stores integers in native endianness. Malformed
records are rejected rather than exposing a partial result.

Run `cargo test --manifest-path rust/media-cache/Cargo.toml`. The library is not
yet linked to Forkgram and does not open or change a Telegram profile. It does
not provide a Telegram client or change the installed application's memory use.
