//! The Simple editor's When / Then cards, saved as Hearth Script (GUIDE.md, "Rules become Hearth Script").
//!
//! A card like "When you pick up the Gym key, unlock the Gym door and play the sound Lock click" becomes:
//!
//! ```text
//! // Rule: When you pick up the Gym key, unlock the Gym door and play the sound Lock click
//! when picked_up(item) {
//!     if item == "Gym key" {
//!         unlock("Gym door")
//!         play("Lock click")
//!     }
//! }
//! ```

/// One card. `event` is the game's event (`picked_up`), `params` what it gives (`item`), `only_if` the optional
/// "which one" (`item` is `"Gym key"`), `then` the actions in order.
#[derive(Debug, Clone, PartialEq)]
pub struct Rule {
    pub said: String,
    pub event: String,
    pub params: Vec<String>,
    pub only_if: Vec<(String, Arg)>,
    pub then: Vec<Action>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Action {
    pub call: String,
    pub args: Vec<Arg>,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Arg {
    Text(String),
    Num(f64),
    Bool(bool),
}

fn arg(a: &Arg) -> String {
    match a {
        Arg::Text(t) => {
            let mut s = String::from("\"");
            for c in t.chars() {
                match c {
                    '"' => s.push_str("\\\""),
                    '\\' => s.push_str("\\\\"),
                    '{' => s.push_str("\\{"),
                    '}' => s.push_str("\\}"),
                    '\n' => s.push_str("\\n"),
                    c => s.push(c),
                }
            }
            s.push('"');
            s
        }
        Arg::Num(n) if n.fract() == 0.0 && n.abs() < 1e15 => format!("{}", *n as i64),
        Arg::Num(n) => format!("{n}"),
        Arg::Bool(b) => format!("{b}"),
    }
}

fn is_name(s: &str) -> bool {
    let mut c = s.chars();
    matches!(c.next(), Some(f) if f.is_ascii_alphabetic() || f == '_') && c.all(|c| c.is_ascii_alphanumeric() || c == '_')
}

/// The Hearth Script for some cards, in order. A card with a name that isn't a plain word is an error, so a card
/// can never write code that does something else.
pub fn to_script(rules: &[Rule]) -> Result<String, String> {
    let mut out = String::new();
    for (i, r) in rules.iter().enumerate() {
        let names = std::iter::once(&r.event).chain(&r.params).chain(r.only_if.iter().map(|(n, _)| n)).chain(r.then.iter().map(|a| &a.call));
        for n in names {
            if !is_name(n) {
                return Err(format!("rule {}: `{n}` isn't a plain name (letters, numbers and _)", i + 1));
            }
        }
        if i > 0 {
            out.push('\n');
        }
        let said: String = r.said.chars().map(|c| if c == '\n' { ' ' } else { c }).collect();
        out.push_str(&format!("// Rule: {said}\nwhen {}({}) {{\n", r.event, r.params.join(", ")));
        let mut indent = "    ".to_string();
        if !r.only_if.is_empty() {
            let cond: Vec<String> = r.only_if.iter().map(|(n, v)| format!("{n} == {}", arg(v))).collect();
            out.push_str(&format!("    if {} {{\n", cond.join(" and ")));
            indent.push_str("    ");
        }
        for a in &r.then {
            let args: Vec<String> = a.args.iter().map(arg).collect();
            out.push_str(&format!("{indent}{}({})\n", a.call, args.join(", ")));
        }
        if !r.only_if.is_empty() {
            out.push_str("    }\n");
        }
        out.push_str("}\n");
    }
    Ok(out)
}
