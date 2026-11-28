use std::cell::RefCell;
use std::collections::BTreeMap;
use std::fmt;
use std::rc::Rc;

use crate::ast::FnDef;
use crate::interp::Scope;

/// What a game function gets and gives back. Errors are plain words; the interpreter adds the line.
pub type NativeFn = dyn Fn(&[Value]) -> Result<Value, String>;

#[derive(Clone)]
pub enum Value {
    Nil,
    Bool(bool),
    Num(f64),
    Text(Rc<str>),
    List(Rc<RefCell<Vec<Value>>>),
    Record(Rc<RefCell<BTreeMap<String, Value>>>),
    Func(Rc<FnDef>, Rc<RefCell<Scope>>),
    Native(Rc<str>, Rc<NativeFn>),
}

impl Value {
    pub fn text(s: &str) -> Value {
        Value::Text(Rc::from(s))
    }

    pub fn list(items: Vec<Value>) -> Value {
        Value::List(Rc::new(RefCell::new(items)))
    }

    pub fn record(fields: Vec<(&str, Value)>) -> Value {
        Value::Record(Rc::new(RefCell::new(fields.into_iter().map(|(k, v)| (k.to_string(), v)).collect())))
    }

    /// What it's called in an error ("a number", "some text").
    pub fn kind(&self) -> &'static str {
        match self {
            Value::Nil => "nil",
            Value::Bool(_) => "true or false",
            Value::Num(_) => "a number",
            Value::Text(_) => "some text",
            Value::List(_) => "a list",
            Value::Record(_) => "a record",
            Value::Func(..) | Value::Native(..) => "a function",
        }
    }

    /// `if` and loops: nil and false are no; everything else is yes.
    pub fn truthy(&self) -> bool {
        !matches!(self, Value::Nil | Value::Bool(false))
    }

    pub fn as_num(&self) -> Option<f64> {
        match self {
            Value::Num(n) => Some(*n),
            _ => None,
        }
    }

    pub fn as_text(&self) -> Option<&str> {
        match self {
            Value::Text(s) => Some(s),
            _ => None,
        }
    }

    pub fn equals(&self, other: &Value) -> bool {
        match (self, other) {
            (Value::Nil, Value::Nil) => true,
            (Value::Bool(a), Value::Bool(b)) => a == b,
            (Value::Num(a), Value::Num(b)) => a == b,
            (Value::Text(a), Value::Text(b)) => a == b,
            (Value::List(a), Value::List(b)) => {
                Rc::ptr_eq(a, b) || {
                    let (a, b) = (a.borrow(), b.borrow());
                    a.len() == b.len() && a.iter().zip(b.iter()).all(|(x, y)| x.equals(y))
                }
            }
            (Value::Record(a), Value::Record(b)) => {
                Rc::ptr_eq(a, b) || {
                    let (a, b) = (a.borrow(), b.borrow());
                    a.len() == b.len() && a.iter().zip(b.iter()).all(|((ka, va), (kb, vb))| ka == kb && va.equals(vb))
                }
            }
            (Value::Func(a, _), Value::Func(b, _)) => Rc::ptr_eq(a, b),
            (Value::Native(a, _), Value::Native(b, _)) => a == b,
            _ => false,
        }
    }
}

fn number(n: f64) -> String {
    if n.fract() == 0.0 && n.abs() < 1e15 {
        format!("{}", n as i64)
    } else {
        format!("{n}")
    }
}

impl fmt::Display for Value {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Value::Nil => write!(f, "nil"),
            Value::Bool(b) => write!(f, "{b}"),
            Value::Num(n) => write!(f, "{}", number(*n)),
            Value::Text(s) => write!(f, "{s}"),
            Value::List(items) => {
                write!(f, "[")?;
                for (i, v) in items.borrow().iter().enumerate() {
                    if i > 0 {
                        write!(f, ", ")?;
                    }
                    match v {
                        Value::Text(s) => write!(f, "\"{s}\"")?,
                        other => write!(f, "{other}")?,
                    }
                }
                write!(f, "]")
            }
            Value::Record(fields) => {
                write!(f, "{{")?;
                for (i, (k, v)) in fields.borrow().iter().enumerate() {
                    if i > 0 {
                        write!(f, ", ")?;
                    }
                    match v {
                        Value::Text(s) => write!(f, "{k}: \"{s}\"")?,
                        other => write!(f, "{k}: {other}")?,
                    }
                }
                write!(f, "}}")
            }
            Value::Func(def, _) => write!(f, "fn {}", def.name),
            Value::Native(name, _) => write!(f, "fn {name}"),
        }
    }
}

impl fmt::Debug for Value {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{self}")
    }
}

impl From<f64> for Value {
    fn from(n: f64) -> Value {
        Value::Num(n)
    }
}

impl From<bool> for Value {
    fn from(b: bool) -> Value {
        Value::Bool(b)
    }
}

impl From<&str> for Value {
    fn from(s: &str) -> Value {
        Value::text(s)
    }
}
