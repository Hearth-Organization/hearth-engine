use std::rc::Rc;

use crate::error::Where;

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum BinOp {
    Add,
    Sub,
    Mul,
    Div,
    Rem,
    Eq,
    NotEq,
    Lt,
    LtEq,
    Gt,
    GtEq,
    And,
    Or,
}

#[derive(Debug, Clone)]
pub enum Expr {
    Num(f64),
    /// Text with its {value} parts: pieces of plain text and values, in order.
    Text(Vec<TextPart>),
    Bool(bool),
    Nil,
    Name(String, Where),
    List(Vec<Expr>),
    Record(Vec<(String, Expr)>),
    Unary { not: bool, value: Box<Expr>, at: Where },
    Binary { op: BinOp, left: Box<Expr>, right: Box<Expr>, at: Where },
    Call { callee: Box<Expr>, args: Vec<Expr>, at: Where },
    Field { of: Box<Expr>, name: String, at: Where },
    Index { of: Box<Expr>, index: Box<Expr>, at: Where },
    Lambda(Rc<FnDef>),
}

#[derive(Debug, Clone)]
pub enum TextPart {
    Plain(String),
    Value(Expr),
}

#[derive(Debug)]
pub struct FnDef {
    pub name: String,
    pub params: Vec<String>,
    pub body: Vec<Stmt>,
    pub at: Where,
}

#[derive(Debug, Clone)]
pub enum Stmt {
    Let { name: String, keep: bool, value: Expr, at: Where },
    /// `target = value`, `target += value`, `target -= value`.
    Assign { target: Expr, op: Option<BinOp>, value: Expr, at: Where },
    Expr(Expr),
    If { cond: Expr, then: Vec<Stmt>, otherwise: Vec<Stmt> },
    While { cond: Expr, body: Vec<Stmt>, at: Where },
    For { name: String, over: Expr, body: Vec<Stmt>, at: Where },
    Fn(Rc<FnDef>),
    When { event: String, def: Rc<FnDef> },
    Return(Option<Expr>, Where),
    Break(Where),
    Continue(Where),
}
