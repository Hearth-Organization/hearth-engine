use std::net::{IpAddr, Ipv4Addr, SocketAddr};
use std::time::{Duration, Instant};

use ggrs::{DesyncDetection, GgrsEvent, PlayerType, SessionBuilder, SessionState, UdpNonBlockingSocket};
use hearth_net::{Command, Game, RtsConfig};
use hearth_rts::{Attack, Grid, Kind, Pos, World};

fn world() -> World {
    let mut soldier = Kind::unit("Soldier", 3.0, 0.4, 60.0);
    soldier.attack = Some(Attack { range: 0.5, damage: 10.0, every: 1.0 });
    let mut w = World::new(Grid::from_text("..............................\n..............................\n.........#....................\n.........#....................\n.........#....................\n..............................\n", 1.0), vec![soldier]);
    for k in 0..6 {
        w.spawn(0, 0, Pos::new(1.5, 0.5 + k as f32 * 0.9));
        w.spawn(0, 1, Pos::new(28.5, 0.5 + k as f32 * 0.9));
    }
    w
}

/// What each player does over the game: both armies attack-move into each other, then team 0 pulls back.
fn script(frame: u32, player: usize) -> Command {
    match (player, frame) {
        (0, 0..=99) => Command::order(1, 2, Pos::new(26.5, 2.5)),
        (0, _) => Command::order(2, 1, Pos::new(2.5, 2.5)),
        (1, _) => Command::order(1, 2, Pos::new(2.5, 2.5)),
        _ => Command::default(),
    }
}

#[test]
fn going_back_and_running_again_always_gives_the_same_game() {
    // The sync test rolls back 7 frames every frame and checks the game comes out the same each time.
    let mut s = SessionBuilder::<RtsConfig>::new().with_num_players(2).unwrap().with_check_distance(7).start_synctest_session().unwrap();
    let mut g = Game::new(world(), 2);
    for f in 0..300u32 {
        for p in 0..2 {
            s.add_local_input(p, script(f, p)).unwrap();
        }
        let requests = s.advance_frame().expect("the game came out differently after going back: it isn't deterministic");
        g.handle(requests);
    }
    assert_eq!(g.frame, 300);
    assert!(g.world.units.len() < 12, "the armies met and fought");
}

#[test]
fn the_same_inputs_on_two_computers_give_the_same_game() {
    let a = Game::new(world(), 2);
    let mut b = a.clone();
    let mut a = a;
    for f in 0..200u32 {
        let inputs = [(script(f, 0), ggrs::InputStatus::Confirmed), (script(f, 1), ggrs::InputStatus::Confirmed)];
        a.advance(&inputs);
        b.advance(&inputs);
    }
    assert_eq!(a.checksum(), b.checksum());
}

fn localhost(port: u16) -> SocketAddr {
    SocketAddr::new(IpAddr::V4(Ipv4Addr::new(127, 0, 0, 1)), port)
}

#[test]
fn two_players_over_the_network_stay_in_step() {
    let (p1, p2) = (17805, 17806);
    let mut s1 = SessionBuilder::<RtsConfig>::new()
        .with_desync_detection_mode(DesyncDetection::On { interval: 10 })
        .add_player(PlayerType::Local, 0)
        .unwrap()
        .add_player(PlayerType::Remote(localhost(p2)), 1)
        .unwrap()
        .start_p2p_session(UdpNonBlockingSocket::bind_to_port(p1).unwrap())
        .unwrap();
    let mut s2 = SessionBuilder::<RtsConfig>::new()
        .with_desync_detection_mode(DesyncDetection::On { interval: 10 })
        .add_player(PlayerType::Remote(localhost(p1)), 0)
        .unwrap()
        .add_player(PlayerType::Local, 1)
        .unwrap()
        .start_p2p_session(UdpNonBlockingSocket::bind_to_port(p2).unwrap())
        .unwrap();
    let deadline = Instant::now() + Duration::from_secs(10);
    while !(s1.current_state() == SessionState::Running && s2.current_state() == SessionState::Running) {
        assert!(Instant::now() < deadline, "the two players didn't connect");
        s1.poll_remote_clients();
        s2.poll_remote_clients();
        std::thread::sleep(Duration::from_millis(5));
    }
    let (mut g1, mut g2) = (Game::new(world(), 2), Game::new(world(), 2));
    let deadline = Instant::now() + Duration::from_secs(30);
    while g1.frame < 150 || g2.frame < 150 {
        assert!(Instant::now() < deadline, "the game stalled at frames {} and {}", g1.frame, g2.frame);
        s1.poll_remote_clients();
        s2.poll_remote_clients();
        for e in s1.events().chain(s2.events()) {
            assert!(!matches!(e, GgrsEvent::DesyncDetected { .. }), "the two games went different ways: {e:?}");
        }
        if g1.frame < 200 && s1.add_local_input(0, script(g1.frame as u32, 0)).is_ok() {
            if let Ok(r) = s1.advance_frame() {
                g1.handle(r);
            }
        }
        if g2.frame < 200 && s2.add_local_input(1, script(g2.frame as u32, 1)).is_ok() {
            if let Ok(r) = s2.advance_frame() {
                g2.handle(r);
            }
        }
        std::thread::sleep(Duration::from_millis(2));
    }
}
