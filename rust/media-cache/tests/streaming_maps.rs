use rustgram_media_cache::{parse_map, ParseError, PART_SIZE};

fn complex(entries: &[(u32, &[u8])]) -> Vec<u8> {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(&(entries.len() as u32).to_ne_bytes());
    for (offset, data) in entries {
        bytes.extend_from_slice(&offset.to_ne_bytes());
        bytes.extend_from_slice(&(data.len() as u32).to_ne_bytes());
        bytes.extend_from_slice(data);
    }
    bytes
}

#[test]
fn contiguous_full_and_final_partial_slice_are_borrowed() {
    let input = vec![42; PART_SIZE + 5];
    let (parts, remaining) = parse_map(&input, input.len()).unwrap();
    assert_eq!(parts.len(), 2);
    assert_eq!(parts[&0].len(), PART_SIZE);
    assert_eq!(parts[&(PART_SIZE as u32)], &[42; 5]);
    assert_eq!(parts[&0].as_ptr(), input.as_ptr());
    assert!(remaining.is_empty());
}

#[test]
fn sparse_map_preserves_remaining_and_first_duplicate() {
    let mut input = complex(&[(8, b"first"), (0, b"abc"), (8, b"last")]);
    input.extend_from_slice(b"next map");
    let (parts, remaining) = parse_map(&input, 100).unwrap();
    assert_eq!(parts[&0], b"abc");
    assert_eq!(parts[&8], b"first");
    assert_eq!(remaining, b"next map");
}

#[test]
fn malformed_parts_are_not_returned() {
    let mut truncated = complex(&[(2, b"abcdef")]);
    truncated.pop();
    assert_eq!(parse_map(&truncated, 100), Err(ParseError::Truncated));
    assert_eq!(
        parse_map(&complex(&[(99, b"ab")]), 100),
        Err(ParseError::OutOfRange)
    );
    assert_eq!(
        parse_map(&complex(&[(0, b"")]), 100),
        Err(ParseError::OutOfRange)
    );
}

#[test]
fn impossible_count_leaves_the_rest_for_the_next_map() {
    let mut bytes = 81u32.to_ne_bytes().to_vec();
    bytes.extend_from_slice(b"tail");
    let (parts, remaining) = parse_map(&bytes, 100).unwrap();
    assert!(parts.is_empty());
    assert_eq!(remaining, b"tail");
}
