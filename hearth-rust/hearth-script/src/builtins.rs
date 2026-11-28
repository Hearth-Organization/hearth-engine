//! What every script has, with no game attached (GUIDE.md, "Built in").

use std::cell::{Cell, RefCell};
use std::rc::Rc;

use crate::value::{NativeFn, Value};

fn want_num(args: &[Value], i: usize, what: &str) -> Result<f64, String> {
    match args.get(i) {
        Some(Value::Num(n)) => Ok(*n),
        Some(other) => Err(format!("{what} needs a number, not {}", other.kind())),
        None => Err(format!("{what} needs a number")),
    }
}

fn count(args: &[Value], n: usize, usage: &str) -> Result<(), String> {
    if args.len() != n {
        return Err(format!("takes {n} value{} ({usage}), but was given {}", if n == 1 { "" } else { "s" }, args.len()));
    }
    Ok(())
}

/// Every built-in, by name. `say` writes to `out`.
pub fn all(out: Rc<RefCell<Vec<String>>>, seed: u64) -> Vec<(&'static str, Rc<NativeFn>)> {
    let state = Rc::new(Cell::new(seed | 1));
    let next = move || {
        // xorshift64*: plenty for games, and no outside code.
        let mut x = state.get();
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        state.set(x);
        (x.wrapping_mul(0x2545_F491_4F6C_DD1D) >> 11) as f64 / (1u64 << 53) as f64
    };
    let next = Rc::new(next);
    let next2 = next.clone();

    vec![
        (
            "say",
            Rc::new(move |args: &[Value]| {
                let line = args.iter().map(|a| a.to_string()).collect::<Vec<_>>().join(" ");
                out.borrow_mut().push(line);
                Ok(Value::Nil)
            }),
        ),
        (
            "len",
            Rc::new(|args: &[Value]| {
                count(args, 1, "len(list)")?;
                Ok(Value::Num(match &args[0] {
                    Value::List(l) => l.borrow().len() as f64,
                    Value::Text(t) => t.chars().count() as f64,
                    Value::Record(r) => r.borrow().len() as f64,
                    other => return Err(format!("needs a list, text or record, not {}", other.kind())),
                }))
            }),
        ),
        (
            "range",
            Rc::new(|args: &[Value]| {
                count(args, 2, "range(from, to)")?;
                let (a, b) = (want_num(args, 0, "range")?, want_num(args, 1, "range")?);
                if (b - a).abs() > 10_000_000.0 {
                    return Err("that range is too big (over 10 million numbers)".into());
                }
                let mut out = Vec::new();
                let mut i = a;
                while i < b {
                    out.push(Value::Num(i));
                    i += 1.0;
                }
                Ok(Value::list(out))
            }),
        ),
        (
            "text",
            Rc::new(|args: &[Value]| {
                count(args, 1, "text(x)")?;
                Ok(Value::text(&args[0].to_string()))
            }),
        ),
        (
            "number",
            Rc::new(|args: &[Value]| {
                count(args, 1, "number(text)")?;
                match &args[0] {
                    Value::Num(n) => Ok(Value::Num(*n)),
                    Value::Text(t) => match t.trim().parse::<f64>() {
                        Ok(n) if n.is_finite() => Ok(Value::Num(n)),
                        _ => Ok(Value::Nil),
                    },
                    other => Err(format!("needs text, not {}", other.kind())),
                }
            }),
        ),
        (
            "keys",
            Rc::new(|args: &[Value]| {
                count(args, 1, "keys(record)")?;
                match &args[0] {
                    Value::Record(r) => Ok(Value::list(r.borrow().keys().map(|k| Value::text(k)).collect())),
                    other => Err(format!("needs a record, not {}", other.kind())),
                }
            }),
        ),
        (
            "push",
            Rc::new(|args: &[Value]| {
                count(args, 2, "push(list, x)")?;
                match &args[0] {
                    Value::List(l) => {
                        l.borrow_mut().push(args[1].clone());
                        Ok(Value::Nil)
                    }
                    other => Err(format!("needs a list first, not {}", other.kind())),
                }
            }),
        ),
        (
            "pop",
            Rc::new(|args: &[Value]| {
                count(args, 1, "pop(list)")?;
                match &args[0] {
                    Value::List(l) => Ok(l.borrow_mut().pop().unwrap_or(Value::Nil)),
                    other => Err(format!("needs a list, not {}", other.kind())),
                }
            }),
        ),
        (
            "contains",
            Rc::new(|args: &[Value]| {
                count(args, 2, "contains(list, x)")?;
                Ok(Value::Bool(match (&args[0], &args[1]) {
                    (Value::List(l), x) => l.borrow().iter().any(|v| v.equals(x)),
                    (Value::Text(t), Value::Text(x)) => t.contains(&**x),
                    (Value::Record(r), Value::Text(x)) => r.borrow().contains_key(&**x),
                    (Value::Text(_), other) => return Err(format!("looks for text in text, not {}", other.kind())),
                    (other, _) => return Err(format!("needs a list or text first, not {}", other.kind())),
                }))
            }),
        ),
        ("floor", Rc::new(|args: &[Value]| Ok(Value::Num(want_num(args, 0, "floor")?.floor())))),
        ("round", Rc::new(|args: &[Value]| Ok(Value::Num(want_num(args, 0, "round")?.round())))),
        ("abs", Rc::new(|args: &[Value]| Ok(Value::Num(want_num(args, 0, "abs")?.abs())))),
        (
            "min",
            Rc::new(|args: &[Value]| {
                count(args, 2, "min(a, b)")?;
                Ok(Value::Num(want_num(args, 0, "min")?.min(want_num(args, 1, "min")?)))
            }),
        ),
        (
            "max",
            Rc::new(|args: &[Value]| {
                count(args, 2, "max(a, b)")?;
                Ok(Value::Num(want_num(args, 0, "max")?.max(want_num(args, 1, "max")?)))
            }),
        ),
        (
            "random",
            Rc::new(move |args: &[Value]| {
                count(args, 0, "random()")?;
                Ok(Value::Num((*next)()))
            }),
        ),
        (
        "random_between",
        Rc::new(move |args: &[Value]| {
            count(args, 2, "random_between(a, b)")?;
            let (a, b) = (want_num(args, 0, "random_between")?, want_num(args, 1, "random_between")?);
            let (lo, hi) = if a <= b { (a, b) } else { (b, a) };
            if lo.fract() == 0.0 && hi.fract() == 0.0 {
                // Whole numbers: each of lo..=hi equally likely (a dice roll).
                Ok(Value::Num((lo + ((*next2)() * (hi - lo + 1.0)).floor()).min(hi)))
            } else {
                Ok(Value::Num(lo + (*next2)() * (hi - lo)))
            }
        }),
        ),
    ]
}
