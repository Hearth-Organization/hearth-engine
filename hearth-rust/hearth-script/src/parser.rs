//! Tokens into statements. Every "expected …" error says what was found instead and, where it helps, what to write.

use std::rc::Rc;

use crate::ast::*;
use crate::error::{ScriptError, Where};
use crate::lexer::{lex, Tok, Token};

pub fn parse(src: &str) -> Result<Vec<Stmt>, ScriptError> {
    let tokens = lex(src)?;
    let mut p = Parser { t: tokens, i: 0, in_fn: 0, in_loop: 0 };
    let mut out = Vec::new();
    p.skip_lines();
    while !p.at(&Tok::Eof) {
        out.push(p.statement(true)?);
        p.end_statement()?;
        p.skip_lines();
    }
    Ok(out)
}

struct Parser {
    t: Vec<Token>,
    i: usize,
    in_fn: usize,
    in_loop: usize,
}

impl Parser {
    fn peek(&self) -> &Tok {
        &self.t[self.i].tok
    }

    fn here(&self) -> Where {
        self.t[self.i].at
    }

    fn at(&self, t: &Tok) -> bool {
        self.peek() == t
    }

    fn next(&mut self) -> Token {
        let t = self.t[self.i].clone();
        if self.i + 1 < self.t.len() {
            self.i += 1;
        }
        t
    }

    fn eat(&mut self, t: &Tok) -> bool {
        if self.at(t) {
            self.next();
            true
        } else {
            false
        }
    }

    fn skip_lines(&mut self) {
        while matches!(self.peek(), Tok::Newline | Tok::Semi) {
            self.next();
        }
    }

    fn expect(&mut self, t: &Tok, what: &str) -> Result<Where, ScriptError> {
        if self.at(t) {
            return Ok(self.next().at);
        }
        Err(ScriptError::at(self.here(), format!("expected `{}` {what}, but found {}", t.symbol(), self.peek().describe())))
    }

    fn name(&mut self, what: &str) -> Result<(String, Where), ScriptError> {
        match self.peek().clone() {
            Tok::Ident(n) => {
                let at = self.next().at;
                Ok((n, at))
            }
            other if !other.symbol().is_empty() && other.symbol().chars().all(char::is_alphabetic) => Err(ScriptError::at(
                self.here(),
                format!("`{}` is one of Hearth Script's own words, so it can't be {what}", other.symbol()),
            )
            .hint("pick another name")),
            other => Err(ScriptError::at(self.here(), format!("expected {what}, but found {}", other.describe()))),
        }
    }

    fn end_statement(&mut self) -> Result<(), ScriptError> {
        match self.peek() {
            Tok::Newline | Tok::Semi | Tok::Eof | Tok::RBrace => Ok(()),
            other => Err(ScriptError::at(self.here(), format!("expected the end of the line, but found {}", other.describe()))
                .hint("put each statement on its own line, or separate them with ;")),
        }
    }

    fn block(&mut self, what: &str) -> Result<Vec<Stmt>, ScriptError> {
        let open = self.here();
        self.expect(&Tok::LBrace, what)?;
        let mut out = Vec::new();
        self.skip_lines();
        while !self.at(&Tok::RBrace) {
            if self.at(&Tok::Eof) {
                return Err(ScriptError::at(open, "this `{` is never closed").hint("add a `}` where the block ends"));
            }
            out.push(self.statement(false)?);
            self.end_statement()?;
            self.skip_lines();
        }
        self.next();
        Ok(out)
    }

    fn function(&mut self, name: String, at: Where) -> Result<Rc<FnDef>, ScriptError> {
        self.expect(&Tok::LParen, "after the function's name")?;
        let mut params = Vec::new();
        while !self.at(&Tok::RParen) {
            let (p, pat) = self.name("a name for what the function is given")?;
            if params.contains(&p) {
                return Err(ScriptError::at(pat, format!("`{p}` is given twice")));
            }
            params.push(p);
            if !self.eat(&Tok::Comma) {
                break;
            }
        }
        self.expect(&Tok::RParen, "after what the function is given")?;
        self.in_fn += 1;
        let saved_loop = std::mem::replace(&mut self.in_loop, 0);
        let body = self.block("to start the function's body");
        self.in_loop = saved_loop;
        self.in_fn -= 1;
        Ok(Rc::new(FnDef { name, params, body: body?, at }))
    }

    fn statement(&mut self, top: bool) -> Result<Stmt, ScriptError> {
        let at = self.here();
        match self.peek().clone() {
            Tok::Keep => {
                self.next();
                if !top {
                    return Err(ScriptError::at(at, "`keep let` only works at the top of a script, not inside a block"));
                }
                if !self.at(&Tok::Let) {
                    return Err(ScriptError::at(self.here(), "`keep` goes before `let`").hint("keep let name = value"));
                }
                self.let_statement(true)
            }
            Tok::Let => self.let_statement(false),
            Tok::Fn => {
                self.next();
                let (name, nat) = self.name("the function's name")?;
                Ok(Stmt::Fn(self.function(name, nat)?))
            }
            Tok::When => {
                self.next();
                if !top {
                    return Err(ScriptError::at(at, "`when` only works at the top of a script, not inside a block"));
                }
                let (event, eat) = self.name("the event's name (like picked_up)")?;
                if !self.at(&Tok::LParen) {
                    return Err(ScriptError::at(self.here(), format!("expected `(` after `{event}`")).hint(format!("when {event}() {{ … }}")));
                }
                Ok(Stmt::When { def: self.function(event.clone(), eat)?, event })
            }
            Tok::If => self.if_statement(),
            Tok::While => {
                self.next();
                let cond = self.expression()?;
                self.in_loop += 1;
                let body = self.block("to start the loop");
                self.in_loop -= 1;
                Ok(Stmt::While { cond, body: body?, at })
            }
            Tok::For => {
                self.next();
                let (name, _) = self.name("a name for each item (for item in list)")?;
                self.expect(&Tok::In, "(for item in list)")?;
                let over = self.expression()?;
                self.in_loop += 1;
                let body = self.block("to start the loop");
                self.in_loop -= 1;
                Ok(Stmt::For { name, over, body: body?, at })
            }
            Tok::Return => {
                self.next();
                if self.in_fn == 0 {
                    return Err(ScriptError::at(at, "`return` only works inside a function or a `when`"));
                }
                let value = if matches!(self.peek(), Tok::Newline | Tok::Semi | Tok::RBrace | Tok::Eof) { None } else { Some(self.expression()?) };
                Ok(Stmt::Return(value, at))
            }
            Tok::Break | Tok::Continue => {
                let t = self.next().tok;
                if self.in_loop == 0 {
                    return Err(ScriptError::at(at, format!("`{}` only works inside a loop", t.symbol())));
                }
                Ok(if t == Tok::Break { Stmt::Break(at) } else { Stmt::Continue(at) })
            }
            _ => {
                let target = self.expression()?;
                let op = match self.peek() {
                    Tok::Eq => Some(None),
                    Tok::PlusEq => Some(Some(BinOp::Add)),
                    Tok::MinusEq => Some(Some(BinOp::Sub)),
                    _ => None,
                };
                if let Some(op) = op {
                    let eq_at = self.next().at;
                    if !matches!(target, Expr::Name(..) | Expr::Field { .. } | Expr::Index { .. }) {
                        return Err(ScriptError::at(eq_at, "only a name, a record's field or a list's item can be changed with ="));
                    }
                    let value = self.expression()?;
                    return Ok(Stmt::Assign { target, op, value, at: eq_at });
                }
                Ok(Stmt::Expr(target))
            }
        }
    }

    fn let_statement(&mut self, keep: bool) -> Result<Stmt, ScriptError> {
        let at = self.expect(&Tok::Let, "")?;
        let (name, _) = self.name("a name after `let`")?;
        if !self.at(&Tok::Eq) {
            return Err(ScriptError::at(self.here(), format!("expected `=` after `let {name}`")).hint(format!("let {name} = 0")));
        }
        self.next();
        let value = self.expression()?;
        Ok(Stmt::Let { name, keep, value, at })
    }

    fn if_statement(&mut self) -> Result<Stmt, ScriptError> {
        self.expect(&Tok::If, "")?;
        let cond = self.expression()?;
        let then = self.block("after the if's condition")?;
        // `else` may be on the next line after the `}`.
        let save = self.i;
        self.skip_lines();
        let otherwise = if self.eat(&Tok::Else) {
            if self.at(&Tok::If) {
                vec![self.if_statement()?]
            } else {
                self.block("after `else`")?
            }
        } else {
            self.i = save;
            Vec::new()
        };
        Ok(Stmt::If { cond, then, otherwise })
    }

    // ---------- expressions, lowest to highest: or, and, not, comparisons, + -, * / %, unary -, calls ----------

    fn expression(&mut self) -> Result<Expr, ScriptError> {
        if self.at(&Tok::Fn) {
            let at = self.next().at;
            return Ok(Expr::Lambda(self.function("(fn)".into(), at)?));
        }
        self.or()
    }

    fn or(&mut self) -> Result<Expr, ScriptError> {
        let mut left = self.and()?;
        while self.at(&Tok::Or) {
            let at = self.next().at;
            let right = self.and()?;
            left = Expr::Binary { op: BinOp::Or, left: Box::new(left), right: Box::new(right), at };
        }
        Ok(left)
    }

    fn and(&mut self) -> Result<Expr, ScriptError> {
        let mut left = self.not()?;
        while self.at(&Tok::And) {
            let at = self.next().at;
            let right = self.not()?;
            left = Expr::Binary { op: BinOp::And, left: Box::new(left), right: Box::new(right), at };
        }
        Ok(left)
    }

    fn not(&mut self) -> Result<Expr, ScriptError> {
        if self.at(&Tok::Not) {
            let at = self.next().at;
            let value = self.not()?;
            return Ok(Expr::Unary { not: true, value: Box::new(value), at });
        }
        self.compare()
    }

    fn compare(&mut self) -> Result<Expr, ScriptError> {
        let left = self.sum()?;
        let op = match self.peek() {
            Tok::EqEq => BinOp::Eq,
            Tok::NotEq => BinOp::NotEq,
            Tok::Lt => BinOp::Lt,
            Tok::LtEq => BinOp::LtEq,
            Tok::Gt => BinOp::Gt,
            Tok::GtEq => BinOp::GtEq,
            _ => return Ok(left),
        };
        let at = self.next().at;
        let right = self.sum()?;
        if matches!(self.peek(), Tok::EqEq | Tok::NotEq | Tok::Lt | Tok::LtEq | Tok::Gt | Tok::GtEq) {
            return Err(ScriptError::at(self.here(), "two comparisons in a row").hint("join them with `and`: a < b and b < c"));
        }
        Ok(Expr::Binary { op, left: Box::new(left), right: Box::new(right), at })
    }

    fn sum(&mut self) -> Result<Expr, ScriptError> {
        let mut left = self.product()?;
        loop {
            let op = match self.peek() {
                Tok::Plus => BinOp::Add,
                Tok::Minus => BinOp::Sub,
                _ => return Ok(left),
            };
            let at = self.next().at;
            let right = self.product()?;
            left = Expr::Binary { op, left: Box::new(left), right: Box::new(right), at };
        }
    }

    fn product(&mut self) -> Result<Expr, ScriptError> {
        let mut left = self.unary()?;
        loop {
            let op = match self.peek() {
                Tok::Star => BinOp::Mul,
                Tok::Slash => BinOp::Div,
                Tok::Percent => BinOp::Rem,
                _ => return Ok(left),
            };
            let at = self.next().at;
            let right = self.unary()?;
            left = Expr::Binary { op, left: Box::new(left), right: Box::new(right), at };
        }
    }

    fn unary(&mut self) -> Result<Expr, ScriptError> {
        if self.at(&Tok::Minus) {
            let at = self.next().at;
            let value = self.unary()?;
            return Ok(Expr::Unary { not: false, value: Box::new(value), at });
        }
        self.postfix()
    }

    fn postfix(&mut self) -> Result<Expr, ScriptError> {
        let mut e = self.primary()?;
        loop {
            match self.peek() {
                Tok::LParen => {
                    let at = self.next().at;
                    let mut args = Vec::new();
                    while !self.at(&Tok::RParen) {
                        args.push(self.expression()?);
                        if !self.eat(&Tok::Comma) {
                            break;
                        }
                    }
                    self.expect(&Tok::RParen, "to close the call")?;
                    e = Expr::Call { callee: Box::new(e), args, at };
                }
                Tok::Dot => {
                    let at = self.next().at;
                    let (name, _) = self.name("a field's name after `.`")?;
                    e = Expr::Field { of: Box::new(e), name, at };
                }
                Tok::LBracket => {
                    let at = self.next().at;
                    let index = self.expression()?;
                    self.expect(&Tok::RBracket, "to close the [")?;
                    e = Expr::Index { of: Box::new(e), index: Box::new(index), at };
                }
                _ => return Ok(e),
            }
        }
    }

    fn primary(&mut self) -> Result<Expr, ScriptError> {
        let at = self.here();
        match self.next().tok {
            Tok::Num(n) => Ok(Expr::Num(n)),
            Tok::Str(s) => text(&s, at),
            Tok::True => Ok(Expr::Bool(true)),
            Tok::False => Ok(Expr::Bool(false)),
            Tok::Nil => Ok(Expr::Nil),
            Tok::Ident(n) => Ok(Expr::Name(n, at)),
            Tok::LParen => {
                let e = self.expression()?;
                self.expect(&Tok::RParen, "to close the (")?;
                Ok(e)
            }
            Tok::LBracket => {
                let mut items = Vec::new();
                while !self.at(&Tok::RBracket) {
                    items.push(self.expression()?);
                    if !self.eat(&Tok::Comma) {
                        break;
                    }
                }
                self.expect(&Tok::RBracket, "to close the list")?;
                Ok(Expr::List(items))
            }
            Tok::LBrace => {
                let mut fields: Vec<(String, Expr)> = Vec::new();
                self.skip_lines();
                while !self.at(&Tok::RBrace) {
                    let (name, nat) = match self.peek().clone() {
                        Tok::Str(s) => (s, self.next().at),
                        _ => self.name("a field's name (like name: value)")?,
                    };
                    if fields.iter().any(|(f, _)| *f == name) {
                        return Err(ScriptError::at(nat, format!("the field `{name}` is given twice")));
                    }
                    self.expect(&Tok::Colon, "between a field's name and its value")?;
                    fields.push((name, self.expression()?));
                    self.skip_lines();
                    if !self.eat(&Tok::Comma) {
                        break;
                    }
                    self.skip_lines();
                }
                self.skip_lines();
                self.expect(&Tok::RBrace, "to close the record")?;
                Ok(Expr::Record(fields))
            }
            other => Err(ScriptError::at(at, format!("expected a value, but found {}", other.describe()))),
        }
    }
}

/// "Welcome to {name}" into its parts; each {…} is a small expression of its own.
fn text(s: &str, at: Where) -> Result<Expr, ScriptError> {
    let mut parts = Vec::new();
    let mut plain = String::new();
    let chars: Vec<char> = s.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        match chars[i] {
            '{' => {
                let start = i + 1;
                let mut depth = 1;
                i += 1;
                while i < chars.len() && depth > 0 {
                    match chars[i] {
                        '{' => depth += 1,
                        '}' => depth -= 1,
                        _ => {}
                    }
                    i += 1;
                }
                if depth > 0 {
                    return Err(ScriptError::at(at, "a `{` in this text is never closed").hint("write \\{ for a { that isn't a value"));
                }
                let inner: String = chars[start..i - 1].iter().collect();
                let tokens = lex(&inner).map_err(|e| ScriptError::at(at, format!("in the text's {{{inner}}}: {}", e.message)))?;
                let mut p = Parser { t: tokens, i: 0, in_fn: 0, in_loop: 0 };
                let e = p.expression().map_err(|e| ScriptError::at(at, format!("in the text's {{{inner}}}: {}", e.message)))?;
                if !p.at(&Tok::Eof) {
                    return Err(ScriptError::at(at, format!("in the text's {{{inner}}}: one value only")));
                }
                if !plain.is_empty() {
                    parts.push(TextPart::Plain(std::mem::take(&mut plain)));
                }
                parts.push(TextPart::Value(e));
                continue;
            }
            '\u{E000}' => plain.push('{'),
            '\u{E001}' => plain.push('}'),
            c => plain.push(c),
        }
        i += 1;
    }
    if !plain.is_empty() || parts.is_empty() {
        parts.push(TextPart::Plain(plain));
    }
    Ok(Expr::Text(parts))
}
