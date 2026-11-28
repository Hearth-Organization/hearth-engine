//! Running statements. Every error says what went wrong and where, in plain words; a loop that never ends or a
//! function that calls itself forever stops with a message instead of freezing the game.

use std::cell::RefCell;
use std::collections::{BTreeMap, HashMap};
use std::rc::Rc;

use crate::ast::*;
use crate::error::{closest, ScriptError, Where};
use crate::value::Value;

#[derive(Default)]
pub struct Scope {
    pub vars: HashMap<String, Value>,
    pub parent: Option<Rc<RefCell<Scope>>>,
}

impl Scope {
    pub fn child(parent: &Rc<RefCell<Scope>>) -> Rc<RefCell<Scope>> {
        Rc::new(RefCell::new(Scope { vars: HashMap::new(), parent: Some(parent.clone()) }))
    }

    pub fn get(scope: &Rc<RefCell<Scope>>, name: &str) -> Option<Value> {
        let s = scope.borrow();
        match s.vars.get(name) {
            Some(v) => Some(v.clone()),
            None => s.parent.as_ref().and_then(|p| Scope::get(p, name)),
        }
    }

    fn set(scope: &Rc<RefCell<Scope>>, name: &str, value: Value) -> bool {
        let mut s = scope.borrow_mut();
        if let Some(v) = s.vars.get_mut(name) {
            *v = value;
            return true;
        }
        match s.parent.clone() {
            Some(p) => {
                drop(s);
                Scope::set(&p, name, value)
            }
            None => false,
        }
    }

    fn names(scope: &Rc<RefCell<Scope>>, out: &mut Vec<String>) {
        let s = scope.borrow();
        out.extend(s.vars.keys().cloned());
        if let Some(p) = &s.parent {
            Scope::names(p, out);
        }
    }
}

pub enum Flow {
    Normal,
    Return(Value),
    Break,
    Continue,
}

pub struct Interp<'a> {
    pub steps: u64,
    pub step_limit: u64,
    pub depth: usize,
    /// `keep let` values from before a reload, by name.
    pub kept: &'a HashMap<String, Value>,
    pub handlers: Vec<(String, Rc<FnDef>, Rc<RefCell<Scope>>)>,
}

// Kept low so a script can never use up the game thread's stack (each level is several Rust calls).
const MAX_DEPTH: usize = 64;

type R<T> = Result<T, ScriptError>;

impl Interp<'_> {
    fn step(&mut self, at: Where) -> R<()> {
        self.steps += 1;
        if self.steps > self.step_limit {
            return Err(ScriptError::at(at, "this ran too long and was stopped (a loop that never ends?)")
                .hint("check the loop's condition becomes false"));
        }
        Ok(())
    }

    pub fn run_top(&mut self, stmts: &[Stmt], scope: &Rc<RefCell<Scope>>) -> R<()> {
        self.hoist(stmts, scope);
        for s in stmts {
            match s {
                Stmt::When { event, def } => self.handlers.push((event.clone(), def.clone(), scope.clone())),
                Stmt::Let { name, keep: true, value, at } => {
                    self.step(*at)?;
                    let v = match self.kept.get(name) {
                        Some(old) => old.clone(),
                        None => self.eval(value, scope)?,
                    };
                    scope.borrow_mut().vars.insert(name.clone(), v);
                }
                other => match self.exec(other, scope)? {
                    Flow::Normal => {}
                    _ => unreachable!("the parser only allows return, break and continue where they belong"),
                },
            }
        }
        Ok(())
    }

    /// Functions can be used before the line that makes them.
    fn hoist(&mut self, stmts: &[Stmt], scope: &Rc<RefCell<Scope>>) {
        for s in stmts {
            if let Stmt::Fn(def) = s {
                let f = Value::Func(def.clone(), scope.clone());
                scope.borrow_mut().vars.insert(def.name.clone(), f);
            }
        }
    }

    pub fn block(&mut self, stmts: &[Stmt], scope: &Rc<RefCell<Scope>>) -> R<Flow> {
        self.hoist(stmts, scope);
        for s in stmts {
            match self.exec(s, scope)? {
                Flow::Normal => {}
                other => return Ok(other),
            }
        }
        Ok(Flow::Normal)
    }

    fn exec(&mut self, s: &Stmt, scope: &Rc<RefCell<Scope>>) -> R<Flow> {
        match s {
            Stmt::Let { name, value, at, .. } => {
                self.step(*at)?;
                let v = self.eval(value, scope)?;
                scope.borrow_mut().vars.insert(name.clone(), v);
            }
            Stmt::Assign { target, op, value, at } => {
                self.step(*at)?;
                let mut v = self.eval(value, scope)?;
                if let Some(op) = op {
                    let old = self.eval(target, scope)?;
                    v = self.binary(*op, old, v, *at)?;
                }
                self.assign(target, v, scope)?;
            }
            Stmt::Expr(e) => {
                self.eval(e, scope)?;
            }
            Stmt::If { cond, then, otherwise } => {
                let inner = Scope::child(scope);
                return if self.eval(cond, scope)?.truthy() { self.block(then, &inner) } else { self.block(otherwise, &inner) };
            }
            Stmt::While { cond, body, at } => loop {
                self.step(*at)?;
                if !self.eval(cond, scope)?.truthy() {
                    break;
                }
                match self.block(body, &Scope::child(scope))? {
                    Flow::Break => break,
                    Flow::Return(v) => return Ok(Flow::Return(v)),
                    _ => {}
                }
            },
            Stmt::For { name, over, body, at } => {
                let items: Vec<Value> = match self.eval(over, scope)? {
                    Value::List(l) => l.borrow().clone(),
                    Value::Text(t) => t.chars().map(|c| Value::text(&c.to_string())).collect(),
                    Value::Record(r) => r.borrow().keys().map(|k| Value::text(k)).collect(),
                    other => {
                        return Err(ScriptError::at(*at, format!("`for` goes through a list, text or record, not {}", other.kind()))
                            .hint("for i in range(0, 10) counts"))
                    }
                };
                for item in items {
                    self.step(*at)?;
                    let inner = Scope::child(scope);
                    inner.borrow_mut().vars.insert(name.clone(), item);
                    match self.block(body, &inner)? {
                        Flow::Break => break,
                        Flow::Return(v) => return Ok(Flow::Return(v)),
                        _ => {}
                    }
                }
            }
            Stmt::Fn(_) => {} // made by hoist
            Stmt::When { .. } => {}
            Stmt::Return(e, _) => {
                let v = match e {
                    Some(e) => self.eval(e, scope)?,
                    None => Value::Nil,
                };
                return Ok(Flow::Return(v));
            }
            Stmt::Break(_) => return Ok(Flow::Break),
            Stmt::Continue(_) => return Ok(Flow::Continue),
        }
        Ok(Flow::Normal)
    }

    fn not_defined(&self, name: &str, at: Where, scope: &Rc<RefCell<Scope>>) -> ScriptError {
        let mut names = Vec::new();
        Scope::names(scope, &mut names);
        let e = ScriptError::at(at, format!("`{name}` isn't defined"));
        match closest(name, names.iter().map(String::as_str)) {
            Some(c) => e.hint(format!("did you mean `{c}`?")),
            None => e.hint(format!("make it first with `let {name} = …`")),
        }
    }

    fn assign(&mut self, target: &Expr, v: Value, scope: &Rc<RefCell<Scope>>) -> R<()> {
        match target {
            Expr::Name(name, at) => {
                if !Scope::set(scope, name, v) {
                    return Err(self.not_defined(name, *at, scope));
                }
                Ok(())
            }
            Expr::Field { of, name, at } => match self.eval(of, scope)? {
                Value::Record(r) => {
                    r.borrow_mut().insert(name.clone(), v);
                    Ok(())
                }
                other => Err(ScriptError::at(*at, format!("only a record has fields, and this is {}", other.kind()))),
            },
            Expr::Index { of, index, at } => {
                let container = self.eval(of, scope)?;
                let index = self.eval(index, scope)?;
                match container {
                    Value::List(l) => {
                        let len = l.borrow().len();
                        let i = list_index(&index, len, *at)?;
                        l.borrow_mut()[i] = v;
                        Ok(())
                    }
                    Value::Record(r) => match index {
                        Value::Text(k) => {
                            r.borrow_mut().insert(k.to_string(), v);
                            Ok(())
                        }
                        other => Err(ScriptError::at(*at, format!("a record's fields are named by text, not {}", other.kind()))),
                    },
                    other => Err(ScriptError::at(*at, format!("only a list or a record has items, and this is {}", other.kind()))),
                }
            }
            _ => unreachable!("the parser only allows names, fields and items here"),
        }
    }

    pub fn eval(&mut self, e: &Expr, scope: &Rc<RefCell<Scope>>) -> R<Value> {
        match e {
            Expr::Num(n) => Ok(Value::Num(*n)),
            Expr::Bool(b) => Ok(Value::Bool(*b)),
            Expr::Nil => Ok(Value::Nil),
            Expr::Text(parts) => {
                let mut s = String::new();
                for p in parts {
                    match p {
                        TextPart::Plain(t) => s.push_str(t),
                        TextPart::Value(e) => s.push_str(&self.eval(e, scope)?.to_string()),
                    }
                }
                Ok(Value::text(&s))
            }
            Expr::Name(name, at) => {
                self.step(*at)?;
                Scope::get(scope, name).ok_or_else(|| self.not_defined(name, *at, scope))
            }
            Expr::List(items) => {
                let mut out = Vec::with_capacity(items.len());
                for i in items {
                    out.push(self.eval(i, scope)?);
                }
                Ok(Value::list(out))
            }
            Expr::Record(fields) => {
                let mut out = BTreeMap::new();
                for (k, v) in fields {
                    out.insert(k.clone(), self.eval(v, scope)?);
                }
                Ok(Value::Record(Rc::new(RefCell::new(out))))
            }
            Expr::Unary { not, value, at } => {
                let v = self.eval(value, scope)?;
                if *not {
                    return Ok(Value::Bool(!v.truthy()));
                }
                match v {
                    Value::Num(n) => Ok(Value::Num(-n)),
                    other => Err(ScriptError::at(*at, format!("`-` goes before a number, not {}", other.kind()))),
                }
            }
            Expr::Binary { op: BinOp::And, left, right, .. } => {
                let l = self.eval(left, scope)?;
                if !l.truthy() {
                    return Ok(Value::Bool(false));
                }
                Ok(Value::Bool(self.eval(right, scope)?.truthy()))
            }
            Expr::Binary { op: BinOp::Or, left, right, .. } => {
                let l = self.eval(left, scope)?;
                if l.truthy() {
                    return Ok(Value::Bool(true));
                }
                Ok(Value::Bool(self.eval(right, scope)?.truthy()))
            }
            Expr::Binary { op, left, right, at } => {
                self.step(*at)?;
                let l = self.eval(left, scope)?;
                let r = self.eval(right, scope)?;
                self.binary(*op, l, r, *at)
            }
            Expr::Field { of, name, at } => match self.eval(of, scope)? {
                Value::Record(r) => {
                    let r = r.borrow();
                    match r.get(name) {
                        Some(v) => Ok(v.clone()),
                        None => {
                            let e = ScriptError::at(*at, format!("this record has no field `{name}`"));
                            Err(match closest(name, r.keys().map(String::as_str)) {
                                Some(c) => e.hint(format!("did you mean `{c}`?")),
                                None => e.hint(format!("its fields: {}", r.keys().cloned().collect::<Vec<_>>().join(", "))),
                            })
                        }
                    }
                }
                other => Err(ScriptError::at(*at, format!("only a record has fields, and this is {}", other.kind()))),
            },
            Expr::Index { of, index, at } => {
                let container = self.eval(of, scope)?;
                let index = self.eval(index, scope)?;
                match container {
                    Value::List(l) => {
                        let l = l.borrow();
                        let i = list_index(&index, l.len(), *at)?;
                        Ok(l[i].clone())
                    }
                    Value::Text(t) => {
                        let chars: Vec<char> = t.chars().collect();
                        let i = list_index(&index, chars.len(), *at)?;
                        Ok(Value::text(&chars[i].to_string()))
                    }
                    Value::Record(r) => match index {
                        Value::Text(k) => Ok(r.borrow().get(&*k).cloned().unwrap_or(Value::Nil)),
                        other => Err(ScriptError::at(*at, format!("a record's fields are named by text, not {}", other.kind()))),
                    },
                    other => Err(ScriptError::at(*at, format!("only a list, text or record has items, and this is {}", other.kind()))),
                }
            }
            Expr::Call { callee, args, at } => {
                self.step(*at)?;
                let f = self.eval(callee, scope)?;
                let mut values = Vec::with_capacity(args.len());
                for a in args {
                    values.push(self.eval(a, scope)?);
                }
                let what = match callee.as_ref() {
                    Expr::Name(n, _) => format!("`{n}`"),
                    _ => "this".into(),
                };
                self.call(&f, &values, *at, &what, false)
            }
            Expr::Lambda(def) => Ok(Value::Func(def.clone(), scope.clone())),
        }
    }

    /// Calls a function. `lenient`: an event may give more values than its `when` takes (they're left out).
    pub fn call(&mut self, f: &Value, args: &[Value], at: Where, what: &str, lenient: bool) -> R<Value> {
        match f {
            Value::Native(name, native) => native(args).map_err(|m| ScriptError::at(at, format!("{name}: {m}"))),
            Value::Func(def, closure) => {
                if args.len() > def.params.len() && !lenient {
                    return Err(ScriptError::at(
                        at,
                        format!("{what} takes {} value{}, but was given {}", def.params.len(), if def.params.len() == 1 { "" } else { "s" }, args.len()),
                    ));
                }
                if self.depth >= MAX_DEPTH {
                    return Err(ScriptError::at(at, format!("{what} calls itself too many times and was stopped"))
                        .hint("a function that calls itself needs a way to stop"));
                }
                let inner = Scope::child(closure);
                for (i, p) in def.params.iter().enumerate() {
                    inner.borrow_mut().vars.insert(p.clone(), args.get(i).cloned().unwrap_or(Value::Nil));
                }
                self.depth += 1;
                let flow = self.block(&def.body, &inner);
                self.depth -= 1;
                match flow? {
                    Flow::Return(v) => Ok(v),
                    _ => Ok(Value::Nil),
                }
            }
            other => Err(ScriptError::at(at, format!("{what} is {}, not a function, so it can't be called", other.kind()))),
        }
    }

    fn binary(&mut self, op: BinOp, l: Value, r: Value, at: Where) -> R<Value> {
        use BinOp::*;
        Ok(match (op, &l, &r) {
            (Eq, _, _) => Value::Bool(l.equals(&r)),
            (NotEq, _, _) => Value::Bool(!l.equals(&r)),
            (Add, Value::Num(a), Value::Num(b)) => Value::Num(a + b),
            (Add, Value::Text(a), _) => Value::text(&format!("{a}{r}")),
            (Add, _, Value::Text(b)) => Value::text(&format!("{l}{b}")),
            (Add, Value::List(a), Value::List(b)) => {
                let mut out = a.borrow().clone();
                out.extend(b.borrow().iter().cloned());
                Value::list(out)
            }
            (Sub, Value::Num(a), Value::Num(b)) => Value::Num(a - b),
            (Mul, Value::Num(a), Value::Num(b)) => Value::Num(a * b),
            (Div | Rem, Value::Num(_), Value::Num(b)) if *b == 0.0 => {
                return Err(ScriptError::at(at, "dividing by zero").hint("check the number isn't 0 first"));
            }
            (Div, Value::Num(a), Value::Num(b)) => Value::Num(a / b),
            (Rem, Value::Num(a), Value::Num(b)) => Value::Num(a.rem_euclid(*b)),
            (Lt, Value::Num(a), Value::Num(b)) => Value::Bool(a < b),
            (LtEq, Value::Num(a), Value::Num(b)) => Value::Bool(a <= b),
            (Gt, Value::Num(a), Value::Num(b)) => Value::Bool(a > b),
            (GtEq, Value::Num(a), Value::Num(b)) => Value::Bool(a >= b),
            (Lt, Value::Text(a), Value::Text(b)) => Value::Bool(a < b),
            (LtEq, Value::Text(a), Value::Text(b)) => Value::Bool(a <= b),
            (Gt, Value::Text(a), Value::Text(b)) => Value::Bool(a > b),
            (GtEq, Value::Text(a), Value::Text(b)) => Value::Bool(a >= b),
            _ => {
                let sym = match op {
                    Add => "+",
                    Sub => "-",
                    Mul => "*",
                    Div => "/",
                    Rem => "%",
                    Lt => "<",
                    LtEq => "<=",
                    Gt => ">",
                    GtEq => ">=",
                    _ => "?",
                };
                let e = ScriptError::at(at, format!("`{sym}` can't work on {} and {}", l.kind(), r.kind()));
                return Err(if matches!((&l, &r), (Value::Text(_), Value::Num(_)) | (Value::Num(_), Value::Text(_))) {
                    e.hint("number(text) turns text into a number")
                } else if matches!(l, Value::Nil) || matches!(r, Value::Nil) {
                    e.hint("one side is nil: something wasn't set yet")
                } else {
                    e
                });
            }
        })
    }
}

fn list_index(index: &Value, len: usize, at: Where) -> R<usize> {
    let Value::Num(n) = index else {
        return Err(ScriptError::at(at, format!("a list's items are numbered, so the [ ] needs a number, not {}", index.kind())));
    };
    if n.fract() != 0.0 {
        return Err(ScriptError::at(at, format!("item {n} isn't a whole number")));
    }
    let i = if *n < 0.0 { len as f64 + n } else { *n };
    if i < 0.0 || i >= len as f64 {
        return Err(ScriptError::at(at, format!("there's no item {n}: this has {len} item{} (the first is 0)", if len == 1 { "" } else { "s" })));
    }
    Ok(i as usize)
}
