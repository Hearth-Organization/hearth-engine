//! Text into tokens. Line breaks end statements, except inside ( [ and after a token that can't end one.

use crate::error::{ScriptError, Where};

#[derive(Debug, Clone, PartialEq)]
pub enum Tok {
    Num(f64),
    /// Text, with its {…} parts still inside (the parser reads them).
    Str(String),
    Ident(String),
    Let,
    Keep,
    Fn,
    Return,
    If,
    Else,
    While,
    For,
    In,
    When,
    And,
    Or,
    Not,
    True,
    False,
    Nil,
    Break,
    Continue,
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Comma,
    Dot,
    Colon,
    Semi,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Eq,
    EqEq,
    NotEq,
    Lt,
    LtEq,
    Gt,
    GtEq,
    PlusEq,
    MinusEq,
    Newline,
    Eof,
}

impl Tok {
    /// How a person would name it in an error.
    pub fn describe(&self) -> String {
        match self {
            Tok::Num(n) => format!("the number {n}"),
            Tok::Str(_) => "some text".into(),
            Tok::Ident(n) => format!("`{n}`"),
            Tok::Newline => "the end of the line".into(),
            Tok::Eof => "the end of the script".into(),
            other => format!("`{}`", other.symbol()),
        }
    }

    pub fn symbol(&self) -> &'static str {
        match self {
            Tok::Let => "let",
            Tok::Keep => "keep",
            Tok::Fn => "fn",
            Tok::Return => "return",
            Tok::If => "if",
            Tok::Else => "else",
            Tok::While => "while",
            Tok::For => "for",
            Tok::In => "in",
            Tok::When => "when",
            Tok::And => "and",
            Tok::Or => "or",
            Tok::Not => "not",
            Tok::True => "true",
            Tok::False => "false",
            Tok::Nil => "nil",
            Tok::Break => "break",
            Tok::Continue => "continue",
            Tok::LParen => "(",
            Tok::RParen => ")",
            Tok::LBrace => "{",
            Tok::RBrace => "}",
            Tok::LBracket => "[",
            Tok::RBracket => "]",
            Tok::Comma => ",",
            Tok::Dot => ".",
            Tok::Colon => ":",
            Tok::Semi => ";",
            Tok::Plus => "+",
            Tok::Minus => "-",
            Tok::Star => "*",
            Tok::Slash => "/",
            Tok::Percent => "%",
            Tok::Eq => "=",
            Tok::EqEq => "==",
            Tok::NotEq => "!=",
            Tok::Lt => "<",
            Tok::LtEq => "<=",
            Tok::Gt => ">",
            Tok::GtEq => ">=",
            Tok::PlusEq => "+=",
            Tok::MinusEq => "-=",
            _ => "",
        }
    }

    /// A line break after this token doesn't end the statement (the line obviously goes on).
    fn continues(&self) -> bool {
        matches!(
            self,
            Tok::Comma | Tok::Dot | Tok::Colon | Tok::Plus | Tok::Minus | Tok::Star | Tok::Slash | Tok::Percent | Tok::Eq | Tok::EqEq
                | Tok::NotEq | Tok::Lt | Tok::LtEq | Tok::Gt | Tok::GtEq | Tok::PlusEq | Tok::MinusEq | Tok::And | Tok::Or | Tok::Not
                | Tok::LParen | Tok::LBracket | Tok::LBrace | Tok::Newline
        )
    }
}

#[derive(Debug, Clone)]
pub struct Token {
    pub tok: Tok,
    pub at: Where,
}

pub fn lex(src: &str) -> Result<Vec<Token>, ScriptError> {
    let chars: Vec<char> = src.chars().collect();
    let mut out: Vec<Token> = Vec::new();
    let (mut i, mut line, mut col) = (0usize, 1u32, 1u32);
    let mut depth: Vec<char> = Vec::new(); // open ( and [ (line breaks inside them don't count)
    macro_rules! push {
        ($t:expr, $at:expr) => {
            out.push(Token { tok: $t, at: $at })
        };
    }
    while i < chars.len() {
        let c = chars[i];
        let at = Where { line, col };
        if c == '\n' {
            let inside = matches!(depth.last(), Some('(') | Some('['));
            let after = out.last().map(|t| t.tok.continues()).unwrap_or(true);
            if !inside && !after {
                push!(Tok::Newline, at);
            }
            i += 1;
            line += 1;
            col = 1;
            continue;
        }
        if c.is_whitespace() {
            i += 1;
            col += 1;
            continue;
        }
        if c == '/' && chars.get(i + 1) == Some(&'/') {
            while i < chars.len() && chars[i] != '\n' {
                i += 1;
            }
            continue;
        }
        if c.is_ascii_digit() {
            let start = i;
            while i < chars.len() && (chars[i].is_ascii_digit() || chars[i] == '_') {
                i += 1;
            }
            if i + 1 < chars.len() && chars[i] == '.' && chars[i + 1].is_ascii_digit() {
                i += 1;
                while i < chars.len() && (chars[i].is_ascii_digit() || chars[i] == '_') {
                    i += 1;
                }
            }
            let text: String = chars[start..i].iter().filter(|c| **c != '_').collect();
            col += (i - start) as u32;
            let n = text.parse::<f64>().map_err(|_| ScriptError::at(at, format!("`{text}` isn't a number")))?;
            push!(Tok::Num(n), at);
            continue;
        }
        if c.is_alphabetic() || c == '_' {
            let start = i;
            while i < chars.len() && (chars[i].is_alphanumeric() || chars[i] == '_') {
                i += 1;
            }
            let word: String = chars[start..i].iter().collect();
            col += (i - start) as u32;
            let tok = match word.as_str() {
                "let" => Tok::Let,
                "keep" => Tok::Keep,
                "fn" => Tok::Fn,
                "return" => Tok::Return,
                "if" => Tok::If,
                "else" => Tok::Else,
                "while" => Tok::While,
                "for" => Tok::For,
                "in" => Tok::In,
                "when" => Tok::When,
                "and" => Tok::And,
                "or" => Tok::Or,
                "not" => Tok::Not,
                "true" => Tok::True,
                "false" => Tok::False,
                "nil" => Tok::Nil,
                "break" => Tok::Break,
                "continue" => Tok::Continue,
                _ => Tok::Ident(word),
            };
            push!(tok, at);
            continue;
        }
        if c == '"' {
            let mut s = String::new();
            i += 1;
            col += 1;
            loop {
                let Some(&ch) = chars.get(i) else {
                    return Err(ScriptError::at(at, "this text never ends").hint("text starts and ends with \""));
                };
                if ch == '\n' {
                    return Err(ScriptError::at(at, "this text runs past the end of the line").hint("end it with \" on the same line"));
                }
                i += 1;
                col += 1;
                match ch {
                    '"' => break,
                    '\\' => {
                        let Some(&e) = chars.get(i) else { continue };
                        i += 1;
                        col += 1;
                        match e {
                            'n' => s.push('\n'),
                            't' => s.push('\t'),
                            '"' => s.push('"'),
                            '\\' => s.push('\\'),
                            // \{ keeps a { that isn't the start of a {value}.
                            '{' => s.push('\u{E000}'),
                            '}' => s.push('\u{E001}'),
                            other => {
                                return Err(ScriptError::at(Where { line, col: col - 2 }, format!("`\\{other}` isn't something text can hold"))
                                    .hint("use \\n for a new line, \\\" for a quote, \\{ for a {"))
                            }
                        }
                    }
                    other => s.push(other),
                }
            }
            push!(Tok::Str(s), at);
            continue;
        }
        let two: String = chars[i..(i + 2).min(chars.len())].iter().collect();
        let tok2 = match two.as_str() {
            "==" => Some(Tok::EqEq),
            "!=" => Some(Tok::NotEq),
            "<=" => Some(Tok::LtEq),
            ">=" => Some(Tok::GtEq),
            "+=" => Some(Tok::PlusEq),
            "-=" => Some(Tok::MinusEq),
            "&&" | "||" => {
                return Err(ScriptError::at(at, format!("`{two}` isn't used here")).hint(if two == "&&" { "write `and`" } else { "write `or`" }))
            }
            _ => None,
        };
        if let Some(t) = tok2 {
            push!(t, at);
            i += 2;
            col += 2;
            continue;
        }
        let tok = match c {
            '(' => Tok::LParen,
            ')' => Tok::RParen,
            '{' => Tok::LBrace,
            '}' => Tok::RBrace,
            '[' => Tok::LBracket,
            ']' => Tok::RBracket,
            ',' => Tok::Comma,
            '.' => Tok::Dot,
            ':' => Tok::Colon,
            ';' => Tok::Semi,
            '+' => Tok::Plus,
            '-' => Tok::Minus,
            '*' => Tok::Star,
            '/' => Tok::Slash,
            '%' => Tok::Percent,
            '=' => Tok::Eq,
            '<' => Tok::Lt,
            '>' => Tok::Gt,
            '!' => return Err(ScriptError::at(at, "`!` isn't used here").hint("write `not`, or `!=` for not equal")),
            other => return Err(ScriptError::at(at, format!("`{other}` isn't part of Hearth Script"))),
        };
        match tok {
            Tok::LParen => depth.push('('),
            Tok::LBracket => depth.push('['),
            Tok::LBrace => depth.push('{'),
            Tok::RParen | Tok::RBracket | Tok::RBrace => {
                depth.pop();
            }
            _ => {}
        }
        // A line break before a closing } still ends the statement inside it: the parser takes the } as an end too.
        push!(tok, at);
        i += 1;
        col += 1;
    }
    out.push(Token { tok: Tok::Eof, at: Where { line, col } });
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn toks(s: &str) -> Vec<Tok> {
        lex(s).unwrap().into_iter().map(|t| t.tok).collect()
    }

    #[test]
    fn lines_end_statements_but_not_inside_brackets() {
        assert_eq!(toks("let a = 1\nlet b = 2"), vec![Tok::Let, Tok::Ident("a".into()), Tok::Eq, Tok::Num(1.0), Tok::Newline, Tok::Let, Tok::Ident("b".into()), Tok::Eq, Tok::Num(2.0), Tok::Eof]);
        assert_eq!(toks("f(1,\n2)"), vec![Tok::Ident("f".into()), Tok::LParen, Tok::Num(1.0), Tok::Comma, Tok::Num(2.0), Tok::RParen, Tok::Eof]);
        assert_eq!(toks("1 +\n2"), vec![Tok::Num(1.0), Tok::Plus, Tok::Num(2.0), Tok::Eof]);
    }

    #[test]
    fn plain_words_for_other_languages_habits() {
        let e = lex("if a && b {}").unwrap_err();
        assert!(e.to_string().contains("write `and`"), "{e}");
        let e = lex("let s = \"no end").unwrap_err();
        assert_eq!(e.at.line, 1);
    }
}
