//! Read Telegram Desktop streaming-media cache maps without copying their bytes.
//!
//! The on-disk integers are in native endianness, as in
//! `Media::Streaming::SerializeComplexPartsMap`. Data is borrowed from the input.

use std::collections::BTreeMap;

pub const PART_SIZE: usize = 128 * 1024;
const MAX_HEADER_PARTS: usize = 80;

/// A sparse map of offsets to media data borrowed from the serialized input.
pub type Parts<'a> = BTreeMap<u32, &'a [u8]>;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ParseError {
    Truncated,
    OutOfRange,
}

/// Parses one cached map, returning its parts and the remaining input.
///
/// Duplicate offsets retain the first part, matching `try_emplace` in the C++
/// reader. Incomplete or out-of-range complex records are rejected instead of
/// returning partially populated data. Integers have native byte order.
pub fn parse_map(data: &[u8], max_size: usize) -> Result<(Parts<'_>, &[u8]), ParseError> {
    let mut parts = BTreeMap::new();
    if data.len().is_multiple_of(PART_SIZE) || data.len() == max_size {
        if data.len() > max_size || data.len() > u32::MAX as usize {
            return Err(ParseError::OutOfRange);
        }
        for (index, part) in data.chunks(PART_SIZE).enumerate() {
            parts.insert((index * PART_SIZE) as u32, part);
        }
        return Ok((parts, &[]));
    }

    let mut remaining = data;
    let count = take_u32(&mut remaining)? as usize;
    // The original reader treats invalid counts as an empty map and returns the
    // remaining input, which can contain an included first slice.
    if count == 0 || count > MAX_HEADER_PARTS {
        return Ok((parts, remaining));
    }
    for _ in 0..count {
        let offset = take_u32(&mut remaining)?;
        let size = take_u32(&mut remaining)? as usize;
        let end = (offset as usize)
            .checked_add(size)
            .ok_or(ParseError::OutOfRange)?;
        if size == 0 || offset as usize >= max_size || end > max_size {
            return Err(ParseError::OutOfRange);
        }
        let part = remaining.get(..size).ok_or(ParseError::Truncated)?;
        remaining = &remaining[size..];
        parts.entry(offset).or_insert(part);
    }
    Ok((parts, remaining))
}

fn take_u32(input: &mut &[u8]) -> Result<u32, ParseError> {
    let bytes: [u8; 4] = input
        .get(..4)
        .ok_or(ParseError::Truncated)?
        .try_into()
        .unwrap();
    *input = &input[4..];
    Ok(u32::from_ne_bytes(bytes))
}
