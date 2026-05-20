//! Internal runtime utilities.
//!
//! * [`settings`] — loads, validates, and (when necessary) rewrites the
//!   on-disk `config.json`. Surfaces a [`settings::LoadOutcome`] so the
//!   caller can distinguish a clean load from a freshly-written default
//!   and from a parsed-but-invalid file (the last case preserves the
//!   user's edits intact).
//! * [`translator`] — the [`translator::Translator`] state machine. Holds
//!   the cumulative mouse button mask, decodes each KMBox Net command,
//!   spawns linear / cubic-bezier interpolation workers when needed, and
//!   forwards 9-byte packets to the serial writer thread over an mpsc
//!   channel.

pub mod settings;
pub mod translator;
