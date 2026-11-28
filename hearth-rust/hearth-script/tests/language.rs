use std::cell::RefCell;
use std::rc::Rc;

use hearth_script::rules::{to_script, Action, Arg, Rule};
use hearth_script::{Engine, Value};

fn run(src: &str) -> Vec<String> {
    let mut e = Engine::new("test.hs").with_seed(7);
    let r = e.load(src);
    let out = e.take_output();
    if let Err(err) = r {
        panic!("{err}\noutput so far: {out:?}");
    }
    out
}

fn mistake(src: &str) -> String {
    let mut e = Engine::new("test.hs");
    match e.load(src) {
        Ok(()) => panic!("expected a mistake in: {src}"),
        Err(err) => err.to_string(),
    }
}

#[test]
fn the_guide_example_runs() {
    let src = r#"
let speed = 3.5
speed = speed + 1
keep let found_keys = 0
let name = "Night School"
let line = "Welcome to {name}"
let list = [1, 2, 3]
let door = {name: "Gym door", locked: true}
door.locked = false
list[0] = 10
fn add(a, b) {
    return a + b
}
if speed > 3 and not door.locked {
    say("fast")
} else if speed > 1 {
    say("slow")
} else {
    say("stopped")
}
while found_keys < 3 { found_keys += 1 }
for item in list { say(item) }
for i in range(0, 5) { say(i) }
say(line, add(2, 3), found_keys)
"#;
    assert_eq!(run(src), ["fast", "10", "2", "3", "0", "1", "2", "3", "4", "Welcome to Night School 5 3"]);
}

#[test]
fn values_print_plainly() {
    assert_eq!(run(r#"say([1, "a", nil], {b: 2, a: "x"}, 2.5, true)"#), [r#"[1, "a", nil] {a: "x", b: 2} 2.5 true"#]);
    assert_eq!(run(r#"say("a \{b\} {1 + 1}")"#), ["a {b} 2"]);
    assert_eq!(run("say(7 % 3, -7 % 3, 1 / 4)"), ["1 2 0.25"]);
}

#[test]
fn loops_break_continue_and_functions() {
    let src = r#"
let total = 0
for i in range(0, 10) {
    if i == 2 { continue }
    if i == 5 { break }
    total += i
}
say(total)
fn fact(n) { if n <= 1 { return 1 } else { return n * fact(n - 1) } }
say(fact(5))
say(later())
fn later() { return "hoisted" }
let double = fn(x) { return x * 2 }
say(double(21))
fn counter() {
    let n = 0
    return fn() { n += 1
        return n }
}
let c = counter()
c()
say(c())
"#;
    assert_eq!(run(src), ["8", "120", "hoisted", "42", "2"]);
}

#[test]
fn built_ins() {
    let src = r#"
let l = [3]
push(l, 4)
say(len(l), pop(l), len(l), len("héllo"), keys({b: 1, a: 2}))
say(contains([1, 2], 2), contains("door", "oo"), contains({a: 1}, "b"))
say(floor(2.7), round(2.5), abs(-3), min(1, 2), max(1, 2), number("12") + 1, number("x"), text(5) + "!")
let r = random()
let d = random_between(1, 6)
say(r >= 0 and r < 1, d >= 1 and d <= 6 and floor(d) == d)
"#;
    assert_eq!(run(src), [r#"2 4 1 5 ["a", "b"]"#, "true true false", "2 3 3 1 2 13 nil 5!", "true true"]);
}

#[test]
fn events_run_in_order_and_get_their_values() {
    let mut e = Engine::new("door.hs");
    let unlocked = Rc::new(RefCell::new(Vec::new()));
    let u = unlocked.clone();
    e.register("unlock", move |args| {
        u.borrow_mut().push(args[0].to_string());
        Ok(Value::Nil)
    });
    e.load(
        r#"
keep let picked = 0
when picked_up(item) {
    picked += 1
    if item == "Gym key" { unlock("Gym door") }
}
when picked_up() { say("second") }
when tick(dt) { say("tick {dt}") }
"#,
    )
    .unwrap();
    assert!(e.listens("picked_up") && !e.listens("died"));
    e.fire("picked_up", &["Gym key".into()]).unwrap();
    e.fire("picked_up", &["Torch".into(), 2.0.into()]).unwrap();
    e.fire("tick", &[0.5.into()]).unwrap();
    e.fire("nobody_listens", &[]).unwrap();
    assert_eq!(*unlocked.borrow(), ["Gym door"]);
    assert_eq!(e.take_output(), ["second", "second", "tick 0.5"]);
    assert_eq!(e.get("picked").unwrap().as_num(), Some(2.0));
}

#[test]
fn reload_keeps_kept_values_and_a_mistake_keeps_the_last_good_version() {
    let mut e = Engine::new("door.hs");
    e.load("keep let keys = 0\nlet other = 1\nwhen got() { keys += 1\nother += 1 }").unwrap();
    e.fire("got", &[]).unwrap();
    e.fire("got", &[]).unwrap();
    e.load("keep let keys = 0\nlet other = 1\nwhen got() { say(keys, other) }").unwrap();
    e.fire("got", &[]).unwrap();
    assert_eq!(e.take_output(), ["2 1"]);

    let err = e.load("when got() { say(\"new\") ").unwrap_err();
    assert!(err.to_string().starts_with("door.hs, line 1"), "{err}");
    e.fire("got", &[]).unwrap();
    assert_eq!(e.take_output(), ["2 1"], "the last good version still runs");

    // A mistake while running the top of the script also keeps the old one.
    assert!(e.load("let a = nope").is_err());
    e.fire("got", &[]).unwrap();
    assert_eq!(e.take_output(), ["2 1"]);
}

#[test]
fn a_loop_that_never_ends_is_stopped() {
    let mut e = Engine::new("loop.hs");
    e.step_limit = 100_000;
    let err = e.load("while true { }").unwrap_err().to_string();
    assert!(err.contains("ran too long"), "{err}");
    let err = e.load("let n = 0\nwhile true { n += 1 }").unwrap_err().to_string();
    assert!(err.contains("ran too long") && err.contains("line 2"), "{err}");
    let err = mistake("fn f() { return f() }\nf()");
    assert!(err.contains("calls itself too many times"), "{err}");
}

#[test]
fn mistakes_are_explained_in_plain_words() {
    let e = mistake("let speed = 1\nsay(sped)");
    assert!(e.contains("line 2") && e.contains("`sped` isn't defined") && e.contains("did you mean `speed`"), "{e}");
    let e = mistake("let d = {locked: true}\nsay(d.lokced)");
    assert!(e.contains("no field `lokced`") && e.contains("did you mean `locked`"), "{e}");
    let e = mistake(r#"say("a" - 1)"#);
    assert!(e.contains("can't work on some text and a number") && e.contains("number(text)"), "{e}");
    let e = mistake("say([1, 2][2])");
    assert!(e.contains("there's no item 2") && e.contains("the first is 0"), "{e}");
    let e = mistake("say(1 / 0)");
    assert!(e.contains("dividing by zero"), "{e}");
    let e = mistake("let x = 5\nx()");
    assert!(e.contains("not a function"), "{e}");
    let e = mistake("fn f(a) { }\nf(1, 2)");
    assert!(e.contains("takes 1 value, but was given 2"), "{e}");
    let e = mistake("say(len(1, 2))");
    assert!(e.contains("len:"), "{e}");
    let e = mistake("nope = 1");
    assert!(e.contains("isn't defined") && e.contains("let nope"), "{e}");
}

#[test]
fn games_can_call_script_functions() {
    let mut e = Engine::new("lib.hs");
    e.load("fn greet(who) { return \"hi {who}\" }").unwrap();
    assert_eq!(e.call("greet", &["Ithil".into()]).unwrap().to_string(), "hi Ithil");
    assert!(e.call("missing", &[]).is_err());
}

#[test]
fn rules_become_hearth_script_that_runs() {
    let rules = [
        Rule {
            said: "When you pick up the Gym key, unlock the Gym door and play the sound Lock click".into(),
            event: "picked_up".into(),
            params: vec!["item".into()],
            only_if: vec![("item".into(), Arg::Text("Gym key".into()))],
            then: vec![
                Action { call: "unlock".into(), args: vec![Arg::Text("Gym door".into())] },
                Action { call: "play".into(), args: vec![Arg::Text("Lock \"click\" {x}".into())] },
            ],
        },
        Rule { said: "When the game starts, wait 2".into(), event: "start".into(), params: vec![], only_if: vec![], then: vec![Action { call: "wait".into(), args: vec![Arg::Num(2.0), Arg::Bool(true)] }] },
    ];
    let src = to_script(&rules).unwrap();
    assert!(src.starts_with("// Rule: When you pick up the Gym key"), "{src}");
    assert!(src.contains("when picked_up(item) {\n    if item == \"Gym key\" {\n        unlock(\"Gym door\")\n"), "{src}");

    let mut e = Engine::new("rules.hs");
    let calls = Rc::new(RefCell::new(Vec::new()));
    for name in ["unlock", "play", "wait"] {
        let c = calls.clone();
        e.register(name, move |args| {
            c.borrow_mut().push(format!("{name} {}", args.iter().map(|a| a.to_string()).collect::<Vec<_>>().join(" ")));
            Ok(Value::Nil)
        });
    }
    e.load(&src).unwrap_or_else(|err| panic!("{err}\n{src}"));
    e.fire("picked_up", &["Torch".into()]).unwrap();
    e.fire("picked_up", &["Gym key".into()]).unwrap();
    e.fire("start", &[]).unwrap();
    assert_eq!(*calls.borrow(), ["unlock Gym door", "play Lock \"click\" {x}", "wait 2 true"]);

    let mut bad = rules[1].clone();
    bad.then[0].call = "wait(); delete_everything".into();
    assert!(to_script(&[bad]).is_err());
}
