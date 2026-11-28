//! Hearth Script: Hearth Engine's small language for what happens in a game (see GUIDE.md).
//!
//! ```
//! let mut engine = hearth_script::Engine::new("door.hs");
//! engine.register("unlock", |args| { println!("unlocked {}", args[0]); Ok(hearth_script::Value::Nil) });
//! engine.load("when picked_up(item) { if item == \"Gym key\" { unlock(\"Gym door\") } }").unwrap();
//! engine.fire("picked_up", &["Gym key".into()]).unwrap();
//! ```

pub mod ast;
mod builtins;
pub mod error;
pub mod interp;
pub mod lexer;
pub mod parser;
pub mod rules;
pub mod value;

use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;

pub use error::{ScriptError, Where};
pub use value::{NativeFn, Value};

use ast::FnDef;
use interp::{Interp, Scope};

type Handler = (String, Rc<FnDef>, Rc<RefCell<Scope>>);

/// One script, loaded and running. Reloading keeps `keep let` values, and a script with a mistake never replaces the
/// last good one.
pub struct Engine {
    name: String,
    natives: Vec<(String, Rc<NativeFn>)>,
    globals: Option<Rc<RefCell<Scope>>>,
    handlers: Vec<Handler>,
    output: Rc<RefCell<Vec<String>>>,
    seed: u64,
    /// Steps one load, event or call may take before it's stopped (a loop that never ends).
    pub step_limit: u64,
}

impl Engine {
    pub fn new(name: &str) -> Engine {
        let seed = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos() as u64)
            .unwrap_or(0x9E37_79B9_7F4A_7C15);
        Engine {
            name: name.to_string(),
            natives: Vec::new(),
            globals: None,
            handlers: Vec::new(),
            output: Rc::new(RefCell::new(Vec::new())),
            seed,
            step_limit: 10_000_000,
        }
    }

    /// The same random numbers every run (for tests and replays).
    pub fn with_seed(mut self, seed: u64) -> Engine {
        self.seed = seed;
        self
    }

    /// Gives scripts a game function (`unlock`, `play`, …). Takes effect at the next load.
    pub fn register(&mut self, name: &str, f: impl Fn(&[Value]) -> Result<Value, String> + 'static) {
        self.natives.retain(|(n, _)| n != name);
        self.natives.push((name.to_string(), Rc::new(f)));
    }

    /// Loads (or reloads) the script. On a mistake, the error comes back and the last good version keeps running.
    pub fn load(&mut self, src: &str) -> Result<(), ScriptError> {
        let program = parser::parse(src).map_err(|e| e.in_script(&self.name))?;
        let root = Rc::new(RefCell::new(Scope::default()));
        {
            let mut r = root.borrow_mut();
            for (name, f) in builtins::all(self.output.clone(), self.seed) {
                r.vars.insert(name.to_string(), Value::Native(Rc::from(name), f));
            }
            for (name, f) in &self.natives {
                r.vars.insert(name.clone(), Value::Native(Rc::from(name.as_str()), f.clone()));
            }
        }
        let globals = Scope::child(&root);
        let kept: HashMap<String, Value> = match &self.globals {
            Some(old) => old.borrow().vars.clone(),
            None => HashMap::new(),
        };
        let mut it = Interp { steps: 0, step_limit: self.step_limit, depth: 0, kept: &kept, handlers: Vec::new() };
        if let Err(e) = it.run_top(&program, &globals) {
            let handlers = std::mem::take(&mut it.handlers);
            drop(it);
            Self::free(&globals, handlers);
            return Err(e.in_script(&self.name));
        }
        let handlers = std::mem::take(&mut it.handlers);
        drop(it);
        if let Some(old) = self.globals.replace(globals) {
            let old_handlers = std::mem::replace(&mut self.handlers, handlers);
            Self::free(&old, old_handlers);
        } else {
            self.handlers = handlers;
        }
        Ok(())
    }

    /// Functions hold their scope and the scope holds them; clearing it lets an old version's memory go.
    fn free(scope: &Rc<RefCell<Scope>>, handlers: Vec<Handler>) {
        drop(handlers);
        let vars = std::mem::take(&mut scope.borrow_mut().vars);
        drop(vars);
    }

    pub fn is_loaded(&self) -> bool {
        self.globals.is_some()
    }

    /// Runs every `when event(…)`, in the order they're written. All of them run even if one has a mistake; the
    /// first mistake comes back.
    pub fn fire(&mut self, event: &str, args: &[Value]) -> Result<(), ScriptError> {
        let empty = HashMap::new();
        let mut first = None;
        let handlers: Vec<Handler> = self.handlers.iter().filter(|h| h.0 == event).cloned().collect();
        for (_, def, scope) in handlers {
            let mut it = Interp { steps: 0, step_limit: self.step_limit, depth: 0, kept: &empty, handlers: Vec::new() };
            let f = Value::Func(def.clone(), scope);
            if let Err(e) = it.call(&f, args, def.at, &format!("when {event}"), true) {
                first.get_or_insert(e.in_script(&self.name));
            }
        }
        first.map_or(Ok(()), Err)
    }

    /// Whether anything listens for this event.
    pub fn listens(&self, event: &str) -> bool {
        self.handlers.iter().any(|h| h.0 == event)
    }

    /// Calls a function the script made.
    pub fn call(&mut self, name: &str, args: &[Value]) -> Result<Value, ScriptError> {
        let f = self.get(name).ok_or_else(|| ScriptError::at(Where::default(), format!("the script has no `{name}`")).in_script(&self.name))?;
        let empty = HashMap::new();
        let mut it = Interp { steps: 0, step_limit: self.step_limit, depth: 0, kept: &empty, handlers: Vec::new() };
        it.call(&f, args, Where::default(), &format!("`{name}`"), false).map_err(|e| e.in_script(&self.name))
    }

    /// A value the script made at its top level.
    pub fn get(&self, name: &str) -> Option<Value> {
        self.globals.as_ref().and_then(|g| g.borrow().vars.get(name).cloned())
    }

    /// What `say` wrote since the last call.
    pub fn take_output(&mut self) -> Vec<String> {
        std::mem::take(&mut *self.output.borrow_mut())
    }
}

impl Drop for Engine {
    fn drop(&mut self) {
        if let Some(g) = self.globals.take() {
            Self::free(&g, std::mem::take(&mut self.handlers));
        }
    }
}
