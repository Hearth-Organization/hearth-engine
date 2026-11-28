# Hearth Script

Hearth Engine's language for what happens in a game: doors, keys, monsters, rules. Small on purpose, so anyone can
read it and Claude can write it reliably from this one page. A change loads into the running game the moment the
file is saved (under a tenth of a second), and a mistake is explained in plain words with where it is; the game keeps
running the last version that worked.

## The whole language

```
// A comment runs to the end of the line.

let speed = 3.5                 // a name for a value
speed = speed + 1               // change it
keep let found_keys = 0         // `keep`: survives reloading the script (the rest starts over)

let name = "Night School"
let line = "Welcome to {name}"  // {…} puts a value into text
let list = [1, 2, 3]
let door = {name: "Gym door", locked: true}   // a record
door.locked = false
list[0] = 10

fn add(a, b) {                  // a function
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
for i in range(0, 5) { say(i) }  // 0, 1, 2, 3, 4

when picked_up(item) {          // runs every time the game says "picked_up"
    if item == "Gym key" {
        unlock("Gym door")
        play("Lock click")
    }
}
```

- Values: numbers (`3`, `3.5`), text (`"…"`, with `\n`, `\"`, `\{`), `true` / `false`, `nil` (nothing), lists `[…]`,
  records `{name: value}`, functions.
- Maths: `+ - * / %`; `+` also joins text and lists. Compare: `== != < <= > >=`. Logic: `and or not`.
- `x += 1`, `x -= 1` change a name, a record field or a list item.
- `break` and `continue` in loops; `return` in functions (`return` alone gives `nil`).
- Lines end a statement; `;` may also separate them. A line may continue after `(`, `[`, `{`, `,` or an operator.

## Built in

`say(x…)` (to the console) · `len(x)` · `range(from, to)` · `text(x)` · `number(text)` · `keys(record)` ·
`push(list, x)` · `pop(list)` · `contains(list or text, x)` · `floor(n)` · `round(n)` · `abs(n)` · `min(a, b)` ·
`max(a, b)` · `random()` (0 to 1) · `random_between(a, b)`.

The game adds its own: Noclip Protocol gives `unlock`, `play`, `spawn`, `move_to`, `sanity`… (each game's list is
in its own guide).

## Events

`when name(args) { … }` runs whenever the game fires `name`. Several `when`s for the same event all run, in the order
they're written. The game's events are things like `start`, `tick(dt)`, `picked_up(item)`, `touched(who, what)`,
`seen(monster)`, `heard(sound)`, `died()`.

## Safe by design

- A script can only call what the game gives it: no files, no network, no programs.
- A loop that never ends can't freeze the game: after 10 million steps in one go, it stops with a message.
- A mistake never stops the game: the error is shown, and the last good version keeps running.

## Rules become Hearth Script

The Simple editor's When / Then cards ("When you pick up the Gym key, unlock the gym door and play a click") are
saved as Hearth Script, so anyone can open the code and keep going:

```
// Rule: When you pick up the Gym key, unlock the Gym door and play the sound Lock click
when picked_up(item) {
    if item == "Gym key" {
        unlock("Gym door")
        play("Lock click")
    }
}
```
