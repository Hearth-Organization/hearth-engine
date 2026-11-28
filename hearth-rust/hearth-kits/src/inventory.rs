//! Bags of slots with stacks, and crafting from recipes.

#[derive(Clone, Debug, PartialEq)]
pub struct Stack {
    pub item: String,
    pub count: u32,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ItemKind {
    pub name: String,
    /// How many fit in one slot (1 for swords, 99 for arrows).
    pub max_stack: u32,
}

#[derive(Clone, Debug)]
pub struct Inventory {
    pub slots: Vec<Option<Stack>>,
}

impl Inventory {
    pub fn new(slots: usize) -> Inventory {
        Inventory { slots: vec![None; slots] }
    }

    pub fn count(&self, item: &str) -> u32 {
        self.slots.iter().flatten().filter(|s| s.item == item).map(|s| s.count).sum()
    }

    /// Room left for an item: in its stacks that aren't full, and in empty slots.
    pub fn room_for(&self, kind: &ItemKind) -> u32 {
        self.slots
            .iter()
            .map(|s| match s {
                None => kind.max_stack,
                Some(s) if s.item == kind.name => kind.max_stack.saturating_sub(s.count),
                Some(_) => 0,
            })
            .sum()
    }

    /// Adds items: fills stacks of the same item first, then empty slots. Returns how many didn't fit.
    pub fn add(&mut self, kind: &ItemKind, mut n: u32) -> u32 {
        let max = kind.max_stack.max(1);
        for s in self.slots.iter_mut().flatten().filter(|s| s.item == kind.name) {
            let put = (max.saturating_sub(s.count)).min(n);
            s.count += put;
            n -= put;
        }
        for slot in self.slots.iter_mut().filter(|s| s.is_none()) {
            if n == 0 {
                break;
            }
            let put = max.min(n);
            *slot = Some(Stack { item: kind.name.clone(), count: put });
            n -= put;
        }
        n
    }

    /// Takes items away (from the last stacks first). Takes nothing and says so when there aren't enough.
    pub fn take(&mut self, item: &str, n: u32) -> Result<(), String> {
        let have = self.count(item);
        if have < n {
            return Err(format!("you need {n} {item} and have {have}"));
        }
        let mut left = n;
        for slot in self.slots.iter_mut().rev() {
            if left == 0 {
                break;
            }
            if let Some(s) = slot {
                if s.item == item {
                    let t = s.count.min(left);
                    s.count -= t;
                    left -= t;
                    if s.count == 0 {
                        *slot = None;
                    }
                }
            }
        }
        Ok(())
    }

    /// Moves a slot onto another: joins stacks of the same item, swaps otherwise.
    pub fn move_slot(&mut self, from: usize, to: usize, kinds: &[ItemKind]) {
        if from == to || from >= self.slots.len() || to >= self.slots.len() {
            return;
        }
        match (self.slots[from].clone(), self.slots[to].clone()) {
            (Some(a), Some(mut b)) if a.item == b.item => {
                let max = kinds.iter().find(|k| k.name == a.item).map(|k| k.max_stack).unwrap_or(1);
                let put = max.saturating_sub(b.count).min(a.count);
                b.count += put;
                self.slots[to] = Some(b);
                self.slots[from] = (a.count > put).then(|| Stack { item: a.item, count: a.count - put });
            }
            _ => self.slots.swap(from, to),
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct Recipe {
    pub name: String,
    pub needs: Vec<(String, u32)>,
    pub makes: Vec<(String, u32)>,
    /// A crafting place needed ("Workbench"), if any.
    pub at: Option<String>,
}

/// Why a recipe can't be made right now, or nothing when it can.
pub fn can_craft(inv: &Inventory, r: &Recipe, kinds: &[ItemKind], near: &[String]) -> Result<(), String> {
    if let Some(at) = &r.at {
        if !near.iter().any(|n| n == at) {
            return Err(format!("{} needs a {at} nearby", r.name));
        }
    }
    let missing: Vec<String> = r.needs.iter().filter(|(i, n)| inv.count(i) < *n).map(|(i, n)| format!("{} {i}", n - inv.count(i))).collect();
    if !missing.is_empty() {
        return Err(format!("{} needs {} more", r.name, missing.join(" and ")));
    }
    // Room for what it makes, counting the space the ingredients free up.
    let mut after = inv.clone();
    for (i, n) in &r.needs {
        after.take(i, *n)?;
    }
    for (i, n) in &r.makes {
        let kind = kinds.iter().find(|k| &k.name == i).cloned().unwrap_or(ItemKind { name: i.clone(), max_stack: 1 });
        if after.add(&kind, *n) > 0 {
            return Err(format!("there's no room for the {i}"));
        }
    }
    Ok(())
}

pub fn craft(inv: &mut Inventory, r: &Recipe, kinds: &[ItemKind], near: &[String]) -> Result<(), String> {
    can_craft(inv, r, kinds, near)?;
    for (i, n) in &r.needs {
        inv.take(i, *n)?;
    }
    for (i, n) in &r.makes {
        let kind = kinds.iter().find(|k| &k.name == i).cloned().unwrap_or(ItemKind { name: i.clone(), max_stack: 1 });
        inv.add(&kind, *n);
    }
    Ok(())
}
