//! Linked prefabs: a copy is the original plus its own changes. Change the original and every copy follows; a
//! value a copy changed stays its own; `null` in a copy's changes removes that part.

use serde_json::{Map, Value};

/// The copy as it is: the original with the copy's changes laid over it.
pub fn instance(original: &Value, changes: &Value) -> Value {
    match (original, changes) {
        (Value::Object(o), Value::Object(c)) => {
            let mut out = Map::new();
            for (k, v) in o {
                match c.get(k) {
                    Some(Value::Null) => {}
                    Some(cv) => {
                        out.insert(k.clone(), instance(v, cv));
                    }
                    None => {
                        out.insert(k.clone(), v.clone());
                    }
                }
            }
            for (k, v) in c {
                if !o.contains_key(k) && !v.is_null() {
                    out.insert(k.clone(), v.clone());
                }
            }
            Value::Object(out)
        }
        (_, c) => c.clone(),
    }
}

/// What a copy changed, worked out from the copy as edited (so editing a copy in the editor only records the
/// parts that really differ).
pub fn changes(original: &Value, edited: &Value) -> Value {
    match (original, edited) {
        (Value::Object(o), Value::Object(e)) => {
            let mut out = Map::new();
            for (k, ov) in o {
                match e.get(k) {
                    None => {
                        out.insert(k.clone(), Value::Null);
                    }
                    Some(ev) if ev != ov => {
                        out.insert(k.clone(), changes(ov, ev));
                    }
                    _ => {}
                }
            }
            for (k, ev) in e {
                if !o.contains_key(k) {
                    out.insert(k.clone(), ev.clone());
                }
            }
            Value::Object(out)
        }
        (o, e) if o == e => Value::Object(Map::new()),
        (_, e) => e.clone(),
    }
}

/// Gives a copy's change back to the original ("apply to prefab"): the original takes the copy's value at `path`
/// and the copy forgets that change.
pub fn push_to_original(original: &mut Value, changes: &mut Value, path: &[&str]) -> Result<(), String> {
    let edited = instance(original, changes);
    let mut from = &edited;
    for p in path {
        from = from.get(*p).ok_or_else(|| format!("the copy has nothing at {}", path.join(".")))?;
    }
    let value = from.clone();
    let mut target = original;
    for (i, p) in path.iter().enumerate() {
        if i + 1 == path.len() {
            target.as_object_mut().ok_or("the original isn't a record there")?.insert(p.to_string(), value.clone());
            break;
        }
        target = target.as_object_mut().ok_or("the original isn't a record there")?.entry(p.to_string()).or_insert(Value::Object(Map::new()));
    }
    // The copy's change is now the original's: forget it.
    let mut c = &mut *changes;
    for (i, p) in path.iter().enumerate() {
        let Some(obj) = c.as_object_mut() else { break };
        if i + 1 == path.len() {
            obj.remove(*p);
            break;
        }
        let Some(next) = obj.get_mut(*p) else { break };
        c = next;
    }
    Ok(())
}
