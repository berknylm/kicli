/// S-expression parser for KiCad 7+ `.kicad_sch` files.
///
/// KiCad uses a Lisp-like s-expression format:
/// ```text
/// (kicad_sch (version 20231120) (generator eeschema)
///   (symbol (lib_id "Device:R") (at 100.0 50.0 0))
/// )
/// ```

#[derive(Debug, Clone, PartialEq)]
pub enum SExpr {
    /// A bare token: symbol name, number, keyword
    Atom(String),
    /// A quoted string value
    Str(String),
    /// A parenthesized list of expressions
    List(Vec<SExpr>),
}

/// Errors that can occur during s-expression parsing.
#[derive(Debug, thiserror::Error)]
pub enum ParseError {
    #[error("unexpected end of input")]
    UnexpectedEof,
    #[error("unexpected token '{0}' at position {1}")]
    UnexpectedToken(String, usize),
    #[error("unclosed parenthesis starting at position {0}")]
    UnclosedParen(usize),
}

struct Tokenizer<'a> {
    input: &'a [u8],
    pos: usize,
}

enum Token {
    LParen { pos: usize },
    RParen,
    Atom(String),
    Str(String),
}

impl<'a> Tokenizer<'a> {
    fn new(input: &'a str) -> Self {
        Tokenizer {
            input: input.as_bytes(),
            pos: 0,
        }
    }

    fn peek(&self) -> Option<u8> {
        self.input.get(self.pos).copied()
    }

    fn advance(&mut self) {
        self.pos += 1;
    }

    fn skip_whitespace_and_comments(&mut self) {
        loop {
            match self.peek() {
                Some(b' ') | Some(b'\t') | Some(b'\r') | Some(b'\n') => {
                    self.advance();
                }
                Some(b';') => {
                    // Skip until end of line
                    while let Some(ch) = self.peek() {
                        self.advance();
                        if ch == b'\n' {
                            break;
                        }
                    }
                }
                _ => break,
            }
        }
    }

    fn read_string(&mut self) -> Result<Token, ParseError> {
        // consume the opening quote
        self.advance();
        let mut result = String::new();
        loop {
            match self.peek() {
                None => return Err(ParseError::UnexpectedEof),
                Some(b'"') => {
                    self.advance();
                    return Ok(Token::Str(result));
                }
                Some(b'\\') => {
                    self.advance();
                    match self.peek() {
                        None => return Err(ParseError::UnexpectedEof),
                        Some(b'"') => {
                            result.push('"');
                            self.advance();
                        }
                        Some(b'\\') => {
                            result.push('\\');
                            self.advance();
                        }
                        Some(b'n') => {
                            result.push('\n');
                            self.advance();
                        }
                        Some(b'r') => {
                            result.push('\r');
                            self.advance();
                        }
                        Some(b't') => {
                            result.push('\t');
                            self.advance();
                        }
                        Some(ch) => {
                            let c = ch as char;
                            self.advance();
                            // unknown escape — keep as-is
                            result.push('\\');
                            result.push(c);
                        }
                    }
                }
                Some(ch) => {
                    result.push(ch as char);
                    self.advance();
                }
            }
        }
    }

    fn read_atom(&mut self) -> Token {
        let mut result = String::new();
        loop {
            match self.peek() {
                None
                | Some(b'(')
                | Some(b')')
                | Some(b'"')
                | Some(b' ')
                | Some(b'\t')
                | Some(b'\r')
                | Some(b'\n')
                | Some(b';') => break,
                Some(ch) => {
                    result.push(ch as char);
                    self.advance();
                }
            }
        }
        Token::Atom(result)
    }

    fn next_token(&mut self) -> Result<Option<Token>, ParseError> {
        self.skip_whitespace_and_comments();
        match self.peek() {
            None => Ok(None),
            Some(b'(') => {
                let pos = self.pos;
                self.advance();
                Ok(Some(Token::LParen { pos }))
            }
            Some(b')') => {
                self.advance();
                Ok(Some(Token::RParen))
            }
            Some(b'"') => Ok(Some(self.read_string()?)),
            Some(_) => Ok(Some(self.read_atom())),
        }
    }
}

fn parse_expr(tokenizer: &mut Tokenizer<'_>) -> Result<SExpr, ParseError> {
    match tokenizer.next_token()? {
        None => Err(ParseError::UnexpectedEof),
        Some(Token::RParen) => Err(ParseError::UnexpectedToken(
            ")".to_string(),
            tokenizer.pos,
        )),
        Some(Token::Atom(s)) => Ok(SExpr::Atom(s)),
        Some(Token::Str(s)) => Ok(SExpr::Str(s)),
        Some(Token::LParen { pos: open_pos }) => {
            let mut children = Vec::new();
            loop {
                tokenizer.skip_whitespace_and_comments();
                match tokenizer.peek() {
                    None => {
                        return Err(ParseError::UnclosedParen(open_pos));
                    }
                    Some(b')') => {
                        tokenizer.advance();
                        return Ok(SExpr::List(children));
                    }
                    _ => {
                        children.push(parse_expr(tokenizer)?);
                    }
                }
            }
        }
    }
}

impl SExpr {
    /// Parse a KiCad s-expression string into an `SExpr` tree.
    pub fn parse(input: &str) -> Result<SExpr, ParseError> {
        let mut tokenizer = Tokenizer::new(input);
        parse_expr(&mut tokenizer)
    }

    /// If this is an `Atom`, return its string value.
    pub fn as_atom(&self) -> Option<&str> {
        match self {
            SExpr::Atom(s) => Some(s.as_str()),
            _ => None,
        }
    }

    /// If this is a `Str` (quoted string), return its value.
    pub fn as_str(&self) -> Option<&str> {
        match self {
            SExpr::Str(s) => Some(s.as_str()),
            _ => None,
        }
    }

    /// Return the string content regardless of whether it is an Atom or Str.
    pub fn as_text(&self) -> Option<&str> {
        match self {
            SExpr::Atom(s) => Some(s.as_str()),
            SExpr::Str(s) => Some(s.as_str()),
            _ => None,
        }
    }

    /// If this is a `List`, return its children.
    pub fn as_list(&self) -> Option<&[SExpr]> {
        match self {
            SExpr::List(v) => Some(v.as_slice()),
            _ => None,
        }
    }

    /// Find the first child `List` that starts with `key` as an atom.
    ///
    /// For a list `(foo (bar 1) (bar 2))`, calling `get("bar")` returns
    /// the first `(bar 1)` child.
    pub fn get(&self, key: &str) -> Option<&SExpr> {
        let children = self.as_list()?;
        children.iter().find(|child| {
            if let SExpr::List(items) = child {
                items.first().and_then(|f| f.as_atom()) == Some(key)
            } else {
                false
            }
        })
    }

    /// Find all child lists that start with `key` as an atom.
    pub fn get_all(&self, key: &str) -> Vec<&SExpr> {
        let children = match self.as_list() {
            Some(c) => c,
            None => return Vec::new(),
        };
        children
            .iter()
            .filter(|child| {
                if let SExpr::List(items) = child {
                    items.first().and_then(|f| f.as_atom()) == Some(key)
                } else {
                    false
                }
            })
            .collect()
    }

    /// Get the first text child (atom or string) of a named child list.
    ///
    /// For `(foo (bar "hello"))`, `atom_value("bar")` returns `Some("hello")`.
    pub fn atom_value(&self, key: &str) -> Option<&str> {
        let child = self.get(key)?;
        let items = child.as_list()?;
        // items[0] is the key atom, items[1] is the value
        items.get(1)?.as_text()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_simple_atom() {
        let e = SExpr::parse("hello").unwrap();
        assert_eq!(e, SExpr::Atom("hello".to_string()));
    }

    #[test]
    fn test_quoted_string() {
        let e = SExpr::parse(r#""Device:R""#).unwrap();
        assert_eq!(e, SExpr::Str("Device:R".to_string()));
    }

    #[test]
    fn test_empty_list() {
        let e = SExpr::parse("()").unwrap();
        assert_eq!(e, SExpr::List(vec![]));
    }

    #[test]
    fn test_nested() {
        let e = SExpr::parse("(kicad_sch (version 20231120))").unwrap();
        let inner = e.get("version").unwrap();
        if let SExpr::List(items) = inner {
            assert_eq!(items[1].as_atom(), Some("20231120"));
        }
    }

    #[test]
    fn test_escape_sequences() {
        let e = SExpr::parse(r#""hello \"world\"""#).unwrap();
        assert_eq!(e, SExpr::Str(r#"hello "world""#.to_string()));
    }
}
